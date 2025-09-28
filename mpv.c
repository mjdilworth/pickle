#include "mpv.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <GLES3/gl31.h>  // For GLubyte type
#include <unistd.h>     // For access() function
#include <sys/utsname.h> // For uname to detect platform
#include <stdio.h>      // For FILE operations

// Hardware decoder detection constants
#define PI_MODEL_FILE "/proc/device-tree/model"
#define PI_MAX_MODEL_LEN 256
#define DEFAULT_HWDEC "auto-safe"

// MPV player functions

// Helper to get a readable string for MPV end-file reason
const char *mpv_player_end_reason_str(int reason) {
    switch (reason) {
        case MPV_END_FILE_REASON_EOF: return "eof";
        case MPV_END_FILE_REASON_STOP: return "stop";
        case MPV_END_FILE_REASON_QUIT: return "quit";
        case MPV_END_FILE_REASON_ERROR: return "error";
        case MPV_END_FILE_REASON_REDIRECT: return "redirect";
        default: return "unknown";
    }
}

// Custom strdup implementation to avoid compatibility issues
static char *pickle_strdup(const char *str) {
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str) + 1; // +1 for null terminator
    char *new_str = malloc(len);
    
    if (new_str) {
        memcpy(new_str, str, len);
    }
    
    return new_str;
}

// Detect if running on Raspberry Pi and get model number
static bool detect_raspberry_pi(char *model_buffer, size_t buffer_size) {
    if (!model_buffer || buffer_size == 0) {
        return false;
    }
    
    // Clear the buffer
    memset(model_buffer, 0, buffer_size);
    
    // Check if we can access the model file (only exists on Raspberry Pi)
    if (access(PI_MODEL_FILE, R_OK) != 0) {
        return false;
    }
    
    // Open the model file
    FILE *model_file = fopen(PI_MODEL_FILE, "r");
    if (!model_file) {
        return false;
    }
    
    // Read the model string
    size_t bytes_read = fread(model_buffer, 1, buffer_size - 1, model_file);
    fclose(model_file);
    
    if (bytes_read == 0) {
        return false;
    }
    
    // Ensure null termination
    model_buffer[bytes_read] = '\0';
    
    // Check if it contains "Raspberry Pi"
    return strstr(model_buffer, "Raspberry Pi") != NULL;
}

// Determine the best hardware decoder for the current platform
static const char *get_best_hwdec(void) {
    // Check environment variable first for manual override
    const char *env_hwdec = getenv("PICKLE_HWDEC");
    if (env_hwdec) {
        LOG_INFO("Using hardware decoder from PICKLE_HWDEC environment: %s", env_hwdec);
        return env_hwdec;
    }
    
    // Check if we're on a Raspberry Pi
    char model_buffer[PI_MAX_MODEL_LEN];
    if (detect_raspberry_pi(model_buffer, sizeof(model_buffer))) {
        LOG_INFO("Detected Raspberry Pi hardware: %s", model_buffer);
        
        // Check if it's a Raspberry Pi 4 or newer
        if (strstr(model_buffer, "Raspberry Pi 4") || 
            strstr(model_buffer, "Raspberry Pi 5")) {
            // For Pi 4 and above, v4l2m2m is the preferred decoder
            LOG_INFO("Using v4l2m2m hardware decoder for Raspberry Pi 4/5");
            return "v4l2m2m";
        } else {
            // For older Pi models, try drm decoder
            LOG_INFO("Using drm hardware decoder for older Raspberry Pi");
            return "drm";
        }
    }
    
    // Not a Raspberry Pi, check for other common platforms
    struct utsname system_info;
    if (uname(&system_info) == 0) {
        LOG_INFO("System: %s %s %s", system_info.sysname, system_info.machine, system_info.release);
        
        // Check for ARM architecture (could be other ARM boards)
        if (strstr(system_info.machine, "arm") || strstr(system_info.machine, "aarch64")) {
            LOG_INFO("ARM platform detected, trying v4l2m2m hardware decoder");
            return "v4l2m2m";
        }
        
        // Check for x86 architecture with potential NVIDIA/AMD GPUs
        if (strstr(system_info.machine, "x86_64")) {
            // On x86, auto-safe is a good default as it will try various decoders
            LOG_INFO("x86_64 platform detected, using auto-safe hardware decoder");
            return "auto-safe";
        }
    }
    
    // Default fallback
    LOG_INFO("Unknown platform, using default auto-safe hardware decoder");
    return DEFAULT_HWDEC;
}

// MPV event callback handler
static void mpv_event_callback(void *data) {
    // This callback is called from a different thread
    (void)data;  // Unused, prevent warning
}

// MPV get_proc_address function
static void *mpv_get_proc_address(void *ctx, const char *name) {
    // Cast the context to a function pointer with a compatible signature
    void *(*get_proc_addr)(const char *) = ctx;
    return get_proc_addr(name);
}

// Initialize MPV player
pickle_result_t mpv_player_init(mpv_player_t *player, void *proc_ctx) {
    if (!player) {
        return PICKLE_ERROR_INVALID_PARAM;
    }
    
    // Clear the player structure
    memset(player, 0, sizeof(mpv_player_t));
    
    // Store the proc_context for the get_proc_address function
    player->proc_context = proc_ctx;
    
    // Create MPV instance
    player->handle = mpv_create();
    if (!player->handle) {
        LOG_ERROR("Failed to create mpv instance");
        return PICKLE_ERROR_MPV;
    }
    
    // Set initial options
    const char *best_hwdec = get_best_hwdec();
    mpv_player_set_hwdec(player, best_hwdec);
    mpv_player_set_option_flag(player, "osc", 0);  // Disable on-screen controller
    
    // Set log level to get more verbose output for debugging
    mpv_request_log_messages(player->handle, "warn");
    
    // Initialize MPV
    int mpv_result = mpv_initialize(player->handle);
    if (mpv_result < 0) {
        LOG_ERROR("Failed to initialize mpv: %s", mpv_error_string(mpv_result));
        mpv_player_cleanup(player);
        return PICKLE_ERROR_MPV;
    }
    
    // Initialize the render context
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &(mpv_opengl_init_params){
            .get_proc_address = mpv_get_proc_address,
            .get_proc_address_ctx = player->proc_context,
        }},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &(int){1}},
        {0}
    };
    
    mpv_result = mpv_render_context_create(&player->render_ctx, player->handle, params);
    if (mpv_result < 0) {
        LOG_ERROR("Failed to create mpv render context: %s", mpv_error_string(mpv_result));
        
        // Try again without advanced control if it failed
        LOG_INFO("Retrying without advanced control...");
        params[2] = (mpv_render_param){0}; // Remove the advanced control parameter
        
        mpv_result = mpv_render_context_create(&player->render_ctx, player->handle, params);
        if (mpv_result < 0) {
            LOG_ERROR("Second attempt to create mpv render context failed: %s", mpv_error_string(mpv_result));
            mpv_player_cleanup(player);
            return PICKLE_ERROR_MPV;
        }
    }
    
    // Set the update callback
    mpv_render_context_set_update_callback(player->render_ctx, mpv_event_callback, NULL);
    
    player->initialized = true;
    return PICKLE_SUCCESS;
}

// Clean up MPV player
void mpv_player_cleanup(mpv_player_t *player) {
    if (!player) {
        return;
    }
    
    // Clean up render context
    if (player->render_ctx) {
        mpv_render_context_free(player->render_ctx);
        player->render_ctx = NULL;
    }
    
    // Clean up mpv handle
    if (player->handle) {
        mpv_terminate_destroy(player->handle);
        player->handle = NULL;
    }
    
    // Free allocated strings
    if (player->hwdec_mode) {
        free(player->hwdec_mode);
        player->hwdec_mode = NULL;
    }
    
    if (player->video_file) {
        free(player->video_file);
        player->video_file = NULL;
    }
    
    player->initialized = false;
}

// Set MPV option string
pickle_result_t mpv_player_set_option_string(mpv_player_t *player, const char *name, const char *value) {
    if (!player || !player->handle || !name || !value) {
        return PICKLE_ERROR_INVALID_PARAM;
    }
    
    int result = mpv_set_option_string(player->handle, name, value);
    if (result < 0) {
        LOG_ERROR("Failed to set mpv option '%s' to '%s': %s", name, value, mpv_error_string(result));
        return PICKLE_ERROR_MPV;
    }
    
    return PICKLE_SUCCESS;
}

// Set MPV flag option
pickle_result_t mpv_player_set_option_flag(mpv_player_t *player, const char *name, int value) {
    if (!player || !player->handle || !name) {
        return PICKLE_ERROR_INVALID_PARAM;
    }
    
    int result = mpv_set_option(player->handle, name, MPV_FORMAT_FLAG, &value);
    if (result < 0) {
        LOG_ERROR("Failed to set mpv flag option '%s' to %d: %s", name, value, mpv_error_string(result));
        return PICKLE_ERROR_MPV;
    }
    
    return PICKLE_SUCCESS;
}

// Load and play a file
pickle_result_t mpv_player_load_file(mpv_player_t *player, const char *filename) {
    if (!player || !player->handle || !filename) {
        return PICKLE_ERROR_INVALID_PARAM;
    }
    
    // Store the filename
    if (player->video_file) {
        free(player->video_file);
    }
    player->video_file = pickle_strdup(filename);
    
    // Use MPV command to load the file
    const char *cmd[] = {"loadfile", filename, NULL};
    int result = mpv_command(player->handle, cmd);
    if (result < 0) {
        LOG_ERROR("Failed to load file '%s': %s", filename, mpv_error_string(result));
        return PICKLE_ERROR_MPV;
    }
    
    return PICKLE_SUCCESS;
}

// Set hardware decoder mode with fallback mechanism
void mpv_player_set_hwdec(mpv_player_t *player, const char *hwdec) {
    if (!player || !player->handle || !hwdec) {
        return;
    }
    
    // Store the hwdec mode
    if (player->hwdec_mode) {
        free(player->hwdec_mode);
    }
    player->hwdec_mode = pickle_strdup(hwdec);
    
    // Set the option
    LOG_INFO("Setting hardware decoder to: %s", hwdec);
    mpv_player_set_option_string(player, "hwdec", hwdec);
    
    // Set additional options to help with hardware decoding on Raspberry Pi
    if (strcmp(hwdec, "v4l2m2m") == 0) {
        // These options can help with V4L2 hardware decoding
        mpv_player_set_option_string(player, "hwdec-codecs", "h264,mpeg2video,mpeg4,vc1,hevc");
        mpv_player_set_option_string(player, "vo", "gpu");
        mpv_player_set_option_string(player, "gpu-context", "drm");
        
        // Additional optimizations for V4L2 decoder
        mpv_player_set_option_string(player, "hwdec-image-format", "drm_prime");
        mpv_player_set_option_flag(player, "opengl-dumb-mode", 1); 
        mpv_player_set_option_string(player, "vd-lavc-dr", "yes");
        
        // Disable cache for better performance with hardware decoding
        mpv_player_set_option_string(player, "cache", "no");
    } else if (strcmp(hwdec, "drm") == 0) {
        // DRM-specific options
        mpv_player_set_option_string(player, "hwdec-codecs", "all");
        mpv_player_set_option_string(player, "vo", "gpu");
        mpv_player_set_option_string(player, "gpu-context", "drm");
        mpv_player_set_option_string(player, "hwdec-image-format", "drm_prime");
        
        // Disable cache for better performance with hardware decoding
        mpv_player_set_option_string(player, "cache", "no");
    }
    
    // Set additional optimizations for software decoding
    if (strcmp(hwdec, "disabled") == 0 || strcmp(hwdec, "no") == 0) {
        // Use a larger cache for software decoding
        mpv_player_set_option_string(player, "cache", "yes");
        mpv_player_set_option_string(player, "cache-secs", "10");
        
        // Tell mpv to use more threads for software decoding
        mpv_player_set_option_string(player, "vd-lavc-threads", "4");
        
        // Set VO and other options for better performance with software decoding
        mpv_player_set_option_string(player, "vo", "gpu");
        mpv_player_set_option_string(player, "profile", "sw-fast");
        mpv_player_set_option_string(player, "framedrop", "vo");
    }
}

// Set loop mode
void mpv_player_set_loop(mpv_player_t *player, bool loop) {
    if (!player || !player->handle) {
        return;
    }
    
    player->loop_playback = loop;
    
    // Set the loop option
    if (loop) {
        mpv_player_set_option_string(player, "loop", "inf");
    } else {
        mpv_player_set_option_string(player, "loop", "no");
    }
}

// Process MPV events
bool mpv_player_process_events(mpv_player_t *player) {
    if (!player || !player->handle) {
        return false;
    }
    
    bool should_continue = true;
    bool hwdec_error_detected = false;
    
    // Process all available events
    while (1) {
        mpv_event *event = mpv_wait_event(player->handle, 0);
        if (event->event_id == MPV_EVENT_NONE) {
            break;
        }
        
        switch (event->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                mpv_event_log_message *msg = event->data;
                if (msg && msg->prefix && msg->text) {
                    if (strcmp(msg->level, "error") == 0) {
                        LOG_ERROR("MPV: %s", msg->text);
                        
                        // Check for hardware decoding errors
                        if (strstr(msg->text, "Cannot load libcuda.so.1") ||
                            strstr(msg->text, "hardware decoding failed") ||
                            strstr(msg->text, "AVHWDeviceContext") ||
                            strstr(msg->text, "after creating texture: OpenGL error")) {
                            hwdec_error_detected = true;
                        }
                    } else if (strcmp(msg->level, "warn") == 0) {
                        LOG_INFO("MPV: %s", msg->text);
                    }
                }
                break;
            }
            
            case MPV_EVENT_END_FILE: {
                mpv_event_end_file *end_file = event->data;
                if (end_file) {
                    LOG_INFO("MPV end of file (reason: %s, error: %d)", 
                            mpv_player_end_reason_str(end_file->reason), end_file->error);
                    
                    // If not looping and reached end of file, signal to exit
                    if (!player->loop_playback && end_file->reason == MPV_END_FILE_REASON_EOF) {
                        should_continue = false;
                    }
                }
                break;
            }
            
            case MPV_EVENT_SHUTDOWN: {
                LOG_INFO("MPV shutdown event received");
                should_continue = false;
                break;
            }
            
            default:
                break;
        }
    }
    
    // If we detected hardware decoding errors, try to fall back
    if (hwdec_error_detected && player->hwdec_mode) {
        // Only try to fall back if we haven't already disabled hardware decoding
        if (strcmp(player->hwdec_mode, "disabled") != 0 && strcmp(player->hwdec_mode, "no") != 0) {
            LOG_INFO("Hardware decoding errors detected, falling back to software decoding");
            
            // First try with drm decoder if we were using v4l2m2m
            if (strcmp(player->hwdec_mode, "v4l2m2m") == 0) {
                LOG_INFO("Trying with drm decoder...");
                mpv_player_set_hwdec(player, "drm");
                
                // Store current playback position
                double position = 0;
                mpv_get_property(player->handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
                
                // Reload the current file if we have one
                if (player->video_file) {
                    LOG_INFO("Reloading video at position %f", position);
                    mpv_player_load_file(player, player->video_file);
                    
                    // Restore position
                    char pos_cmd[64];
                    snprintf(pos_cmd, sizeof(pos_cmd), "%.1f", position);
                    mpv_set_property_string(player->handle, "time-pos", pos_cmd);
                    
                    // Return here to give the drm decoder a chance
                    return should_continue;
                }
            }
            
            // If we get here, either we weren't using v4l2m2m or drm also failed
            LOG_INFO("Falling back to software decoding...");
            
            // Try to set software decoding
            mpv_player_set_hwdec(player, "disabled");
            
            // Store current playback position
            double position = 0;
            mpv_get_property(player->handle, "time-pos", MPV_FORMAT_DOUBLE, &position);
            
            // Reload the current file if we have one
            if (player->video_file) {
                LOG_INFO("Reloading video at position %f", position);
                mpv_player_load_file(player, player->video_file);
                
                // Restore position
                char pos_cmd[64];
                snprintf(pos_cmd, sizeof(pos_cmd), "%.1f", position);
                mpv_set_property_string(player->handle, "time-pos", pos_cmd);
            }
        }
    }
    
    return should_continue;
}

// Check if a new frame is available
bool mpv_player_has_frame(mpv_player_t *player) {
    if (!player || !player->render_ctx) {
        return false;
    }
    
    return (mpv_render_context_update(player->render_ctx) & MPV_RENDER_UPDATE_FRAME) != 0;
}

// Check if MPV render context is valid
bool mpv_player_has_render_context(mpv_player_t *player) {
    return player && player->render_ctx;
}