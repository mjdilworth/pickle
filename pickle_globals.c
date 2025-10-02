#include "pickle_globals.h"

// Export variables used by other modules
int g_debug = 0;
uint64_t g_frames = 0;
int g_help_toggle_request = 0;
int g_use_v4l2_decoder = 0;

// Stub for render_v4l2_frame function (will be implemented in FFmpeg V4L2 player)
bool render_v4l2_frame(kms_ctx_t *d, egl_ctx_t *e, v4l2_player_t *p) {
    (void)d; (void)e; (void)p;  // Silence unused parameter warnings
    return false;  // Not implemented in this module
}