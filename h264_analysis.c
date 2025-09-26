#define _GNU_SOURCE  // For strdup
#include "h264_analysis.h"
#include "utils.h"
#include <mpv/client.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Analyze H.264 profile compatibility using MPV
bool analyze_h264_profile(const char *filename, h264_analysis_result_t *result) {
    if (!filename || !result) {
        return false;
    }
    
    // Initialize result structure
    memset(result, 0, sizeof(h264_analysis_result_t));
    
    LOG_INFO("Analyzing video file for H.264 profile compatibility...");
    
    // Create a temporary MPV handle for analysis
    mpv_handle *temp_mpv = mpv_create();
    if (!temp_mpv) {
        LOG_ERROR("Failed to create MPV handle for analysis");
        return false;
    }
    
    // Set minimal options for analysis - no video/audio output
    mpv_set_option_string(temp_mpv, "vo", "null");
    mpv_set_option_string(temp_mpv, "ao", "null");
    mpv_set_option_string(temp_mpv, "hwdec", "no"); // Force software for analysis
    mpv_set_option_string(temp_mpv, "pause", "yes");
    mpv_set_option_string(temp_mpv, "terminal", "no");
    
    if (mpv_initialize(temp_mpv) < 0) {
        LOG_ERROR("Failed to initialize MPV for analysis");
        mpv_terminate_destroy(temp_mpv);
        return false;
    }
    
    // Load the file
    const char *cmd[] = {"loadfile", filename, NULL};
    if (mpv_command(temp_mpv, cmd) < 0) {
        LOG_ERROR("Failed to load file for analysis: %s", filename);
        mpv_terminate_destroy(temp_mpv);
        return false;
    }
    
    // Wait for video to be ready
    bool video_ready = false;
    for (int tries = 0; tries < 20; tries++) {  // Increased timeout for large files
        mpv_event *ev = mpv_wait_event(temp_mpv, 0.5); // 500ms timeout per try
        if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
            video_ready = true;
            break;
        }
        if (ev->event_id == MPV_EVENT_END_FILE) {
            LOG_ERROR("File ended during analysis");
            break;
        }
        if (ev->event_id == MPV_EVENT_IDLE) {
            // File might be loaded but no video track
            break;
        }
    }
    
    if (video_ready) {
        // Get video codec information
        result->codec_name = mpv_get_property_string(temp_mpv, "video-codec");
        result->format_name = mpv_get_property_string(temp_mpv, "video-format");
        
        mpv_get_property(temp_mpv, "width", MPV_FORMAT_INT64, &result->width);
        mpv_get_property(temp_mpv, "height", MPV_FORMAT_INT64, &result->height);
        mpv_get_property(temp_mpv, "container-fps", MPV_FORMAT_DOUBLE, &result->fps);
        
        // Check if this is H.264
        result->is_h264 = (result->codec_name && strstr(result->codec_name, "h264"));
        
        // Analyze hardware compatibility
        result->hw_compatible = is_h264_hw_compatible(result);
    }
    
    mpv_terminate_destroy(temp_mpv);
    return video_ready;
}

// Check if the analyzed video is compatible with Raspberry Pi hardware decoder
bool is_h264_hw_compatible(h264_analysis_result_t *result) {
    if (!result || !result->is_h264) {
        return false; // Not H.264, so hardware decoder won't help
    }
    
    bool compatible = true;
    char warning[1024] = {0};
    
    // Check resolution limits
    if (result->width > 1920 || result->height > 1080) {
        compatible = false;
        snprintf(warning + strlen(warning), sizeof(warning) - strlen(warning),
                "Resolution %dx%d exceeds hardware decoder limits (max 1920x1080). ",
                (int)result->width, (int)result->height);
    }
    
    // Check chroma format
    if (result->format_name) {
        if (strstr(result->format_name, "yuv444") || 
            strstr(result->format_name, "yuv422") ||
            strstr(result->format_name, "rgb")) {
            compatible = false;
            snprintf(warning + strlen(warning), sizeof(warning) - strlen(warning),
                    "Chroma format %s not supported by hardware decoder (need yuv420p). ",
                    result->format_name);
        }
    }
    
    // Store warning message if incompatible
    if (!compatible && strlen(warning) > 0) {
        result->compatibility_warning = strdup(warning);
    }
    
    return compatible;
}

// Log detailed compatibility information
void log_h264_compatibility_info(const h264_analysis_result_t *result, const char *filename) {
    if (!result) return;
    
    LOG_INFO("Video analysis results for: %s", filename ? filename : "unknown");
    LOG_INFO("  Codec: %s", result->codec_name ? result->codec_name : "unknown");
    LOG_INFO("  Format: %s", result->format_name ? result->format_name : "unknown");
    LOG_INFO("  Resolution: %dx%d", (int)result->width, (int)result->height);
    LOG_INFO("  FPS: %.2f", result->fps);
    
    if (result->is_h264) {
        if (result->hw_compatible) {
            LOG_INFO("✓ Video appears compatible with Raspberry Pi hardware decoder");
        } else {
            LOG_INFO("ℹ️  Video parameters suggest software decoding may be preferred");
            if (result->compatibility_warning) {
                LOG_INFO("  Analysis: %s", result->compatibility_warning);
            }
            LOG_INFO("  For hardware acceleration, consider transcoding with:");
            LOG_INFO("    ffmpeg -i \"%s\" -c:v h264_v4l2m2m -profile:v main -level:v 4.0 \\", filename ? filename : "input.mp4");
            LOG_INFO("           -pix_fmt yuv420p -s 1920x1080 -c:a copy output.mp4");
        }
    } else {
        LOG_INFO("  Not an H.264 video - hardware decoder not applicable");
    }
}

// Clean up analysis result
void free_h264_analysis_result(h264_analysis_result_t *result) {
    if (!result) return;
    
    if (result->codec_name) {
        mpv_free(result->codec_name);
        result->codec_name = NULL;
    }
    if (result->format_name) {
        mpv_free(result->format_name);
        result->format_name = NULL;
    }
    if (result->compatibility_warning) {
        free(result->compatibility_warning);
        result->compatibility_warning = NULL;
    }
    
    memset(result, 0, sizeof(h264_analysis_result_t));
}