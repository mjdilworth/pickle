#define _GNU_SOURCE  // For strdup
#include "hwdec_monitor.h"
#include "h264_analysis.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Initialize hardware decoder monitor
void hwdec_monitor_init(hwdec_monitor_t *monitor) {
    if (!monitor) return;
    
    memset(monitor, 0, sizeof(hwdec_monitor_t));
}

// Clean up hardware decoder monitor
void hwdec_monitor_cleanup(hwdec_monitor_t *monitor) {
    if (!monitor) return;
    
    if (monitor->current_file) {
        free(monitor->current_file);
        monitor->current_file = NULL;
    }
    
    memset(monitor, 0, sizeof(hwdec_monitor_t));
}

// Reset monitor for new file
void hwdec_monitor_reset(hwdec_monitor_t *monitor) {
    if (!monitor) return;
    
    monitor->hwdec_checked = false;
    monitor->hwdec_failed = false;
    
    if (monitor->current_file) {
        free(monitor->current_file);
        monitor->current_file = NULL;
    }
}

// Check if hardware decoding failed and provide detailed analysis
bool hwdec_monitor_check_failure(hwdec_monitor_t *monitor, mpv_handle *handle, const char *filename) {
    if (!monitor || !handle || monitor->hwdec_checked) {
        return false;
    }
    
    // Get current hwdec setting
    char *current_hwdec = mpv_get_property_string(handle, "hwdec-current");
    char *requested_hwdec = mpv_get_property_string(handle, "hwdec");
    
    bool hardware_failed = false;
    
    if (current_hwdec && requested_hwdec) {
        // Check if hardware decoding was requested but software is being used
        if (strcmp(current_hwdec, "no") == 0 && 
            (strcmp(requested_hwdec, "auto-safe") == 0 || 
             strcmp(requested_hwdec, "auto") == 0 ||
             strstr(requested_hwdec, "v4l2m2m") != NULL)) {
            
            hardware_failed = true;
            monitor->hwdec_failed = true;
            
            LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            LOG_INFO("� Hardware Decoder Advisory");
            LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            LOG_INFO("Requested: %s → Using: %s", requested_hwdec, current_hwdec);
            LOG_INFO("▶️  Video will play using software decoding (playback continues normally)");
            
            // Try to get video codec information
            char *video_codec = mpv_get_property_string(handle, "video-codec");
            if (video_codec && strstr(video_codec, "h264")) {
                LOG_INFO("📹 H.264 video detected - hardware acceleration not available");
                LOG_INFO("🔍 Compatibility analysis:");
                
                // Perform detailed analysis if requested
                const char *analyze_env = getenv("PICKLE_ANALYZE_VIDEO");
                if (analyze_env && *analyze_env) {
                    h264_analysis_result_t analysis = {0};
                    if (analyze_h264_profile(filename, &analysis)) {
                        log_h264_compatibility_info(&analysis, filename);
                        free_h264_analysis_result(&analysis);
                    }
                } else {
                    // Provide basic information without full analysis
                    LOG_INFO("📋 Possible reasons hardware acceleration is unavailable:");
                    LOG_INFO("   • High/High10 profile (RPi4 supports Baseline/Main only)");
                    LOG_INFO("   • Resolution > 1920x1080");
                    LOG_INFO("   • Non-4:2:0 chroma subsampling (4:2:2, 4:4:4)");
                    LOG_INFO("   • High bitrate or complex encoding settings");
                    LOG_INFO("");
                    LOG_INFO("💡 To enable hardware acceleration, try transcoding:");
                    LOG_INFO("   ffmpeg -i \"%s\" -c:v h264_v4l2m2m \\", filename ? filename : "input.mp4");
                    LOG_INFO("          -profile:v main -level:v 4.0 -pix_fmt yuv420p \\");
                    LOG_INFO("          -c:a copy output.mp4");
                    LOG_INFO("");
                    LOG_INFO("🔬 For detailed analysis, run with: PICKLE_ANALYZE_VIDEO=1");
                }
            } else if (video_codec) {
                LOG_INFO("📹 Video codec: %s (not H.264)", video_codec);
                LOG_INFO("💡 Raspberry Pi hardware decoder only supports H.264");
                LOG_INFO("   Consider transcoding to H.264 for hardware acceleration");
            }
            
            LOG_INFO("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
            
            if (video_codec) mpv_free(video_codec);
        }
    }
    
    if (current_hwdec) mpv_free(current_hwdec);
    if (requested_hwdec) mpv_free(requested_hwdec);
    
    // Store filename for reference
    if (filename && !monitor->current_file) {
        monitor->current_file = strdup(filename);
    }
    
    monitor->hwdec_checked = true;
    return hardware_failed;
}

// Monitor MPV log messages for hardware decoder errors
void hwdec_monitor_log_message(const mpv_event_log_message *lm) {
    if (!lm || !lm->text || !lm->level) return;
    
    // Look for V4L2 M2M decoder specific failures
    if (strstr(lm->text, "v4l2m2m") && 
        (strstr(lm->level, "error") || strstr(lm->level, "warn"))) {
        
        if (strstr(lm->text, "unsupported") || 
            strstr(lm->text, "failed") || 
            strstr(lm->text, "not supported") ||
            strstr(lm->text, "invalid") ||
            strstr(lm->text, "cannot")) {
            
            LOG_ERROR("🔧 V4L2 M2M hardware decoder: %s", lm->text);
            if (strstr(lm->text, "profile") || strstr(lm->text, "format")) {
                LOG_ERROR("💡 This often indicates H.264 profile/format incompatibility");
            }
        }
    }
    
    // Look for general hardware decoding failures
    if ((strstr(lm->text, "hwdec") || strstr(lm->text, "hardware")) && 
        strstr(lm->level, "error")) {
        LOG_ERROR("🔧 Hardware decoding: %s", lm->text);
    }
    
    // Look for decoder initialization failures
    if (strstr(lm->text, "decoder") && strstr(lm->text, "init") && 
        strstr(lm->level, "error")) {
        LOG_ERROR("🔧 Decoder initialization: %s", lm->text);
    }
}