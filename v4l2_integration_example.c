/*
 * Example integration of V4L2 modular demuxer in pickle.c
 * 
 * This shows the minimal changes needed to integrate the V4L2 demuxer
 * in a modular way without breaking existing functionality.
 */

#if defined(USE_V4L2_DECODER) && defined(ENABLE_V4L2_DEMUXER)
#include "v4l2_integration.h"

// Global V4L2 integration instance (in existing globals section)
static v4l2_integration_t *g_v4l2_integration = NULL;

/*
 * V4L2 Integration Functions - Add to pickle.c
 */

// Initialize V4L2 integration (call in main after other initialization)
static bool init_v4l2_integration(void) {
    if (!v4l2_integration_is_available()) {
        LOG_INFO("V4L2 integration not available");
        return false;
    }
    
    g_v4l2_integration = v4l2_integration_create();
    if (!g_v4l2_integration) {
        LOG_ERROR("Failed to create V4L2 integration");
        return false;
    }
    
    LOG_INFO("V4L2 integration initialized");
    return true;
}

// Try to open file with V4L2 integration (call before MPV fallback)
static bool try_v4l2_playback(const char *file_path) {
    if (!g_v4l2_integration) {
        return false;
    }
    
    // Check if this is a container format that needs demuxing
    if (!v4l2_integration_is_container_format(file_path)) {
        LOG_INFO("Not a container format, using MPV: %s", file_path);
        return false;
    }
    
    // Try to open with V4L2 integration
    if (!v4l2_integration_open_file(g_v4l2_integration, file_path)) {
        LOG_WARN("V4L2 integration failed to open: %s", file_path);
        return false;
    }
    
    // Start playback
    if (!v4l2_integration_start_playback(g_v4l2_integration)) {
        LOG_ERROR("Failed to start V4L2 playback");
        return false;
    }
    
    LOG_INFO("V4L2 integration successfully started playback");
    return true;
}

// Process V4L2 packets (call in main loop)
static void process_v4l2_integration(void) {
    if (!g_v4l2_integration) return;
    
    // Process available packets
    int processed = v4l2_integration_process(g_v4l2_integration);
    if (processed > 0) {
        // Optionally log processing statistics
        static int total_processed = 0;
        total_processed += processed;
        
        if (total_processed % 100 == 0) {
            LOG_DEBUG("V4L2 integration processed %d total packets", total_processed);
        }
    }
}

// Cleanup V4L2 integration (call in cleanup/exit)
static void cleanup_v4l2_integration(void) {
    if (g_v4l2_integration) {
        v4l2_integration_destroy(g_v4l2_integration);
        g_v4l2_integration = NULL;
        LOG_INFO("V4L2 integration cleaned up");
    }
}

/*
 * Integration Points in Main Application:
 * 
 * 1. In main() after other initialization:
 *    init_v4l2_integration();
 * 
 * 2. In file opening logic, before MPV initialization:
 *    if (try_v4l2_playback(file_path)) {
 *        // V4L2 is handling playback
 *        g_use_v4l2_decoder = 1;
 *    } else {
 *        // Fall back to MPV
 *        g_use_v4l2_decoder = 0;
 *        // ... existing MPV initialization
 *    }
 * 
 * 3. In main render loop:
 *    process_v4l2_integration();
 * 
 * 4. In cleanup/exit:
 *    cleanup_v4l2_integration();
 * 
 * This approach:
 * - Keeps V4L2 integration modular and separate
 * - Doesn't break existing MPV functionality  
 * - Provides clean fallback when V4L2 can't handle the file
 * - Minimal changes to existing code structure
 * - Easy to disable by setting ENABLE_V4L2_DEMUXER=0
 */

#endif // USE_V4L2_DECODER && ENABLE_V4L2_DEMUXER