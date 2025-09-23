#include "pickle_globals.h"
#include "mpv.h"
#include "drm.h"
#include "egl.h"
#include <stdio.h>

// Simple wrapper for MPV rendering (used by pickle_events.c)
bool render_frame_mpv(mpv_handle *mpv, mpv_render_context *mpv_gl, kms_ctx_t *drm, egl_ctx_t *eglc) {
    // Just a placeholder implementation - the real logic should be moved from pickle.c
    // This is just to satisfy the linker
    fprintf(stderr, "render_frame_mpv called but not fully implemented\n");
    return false;
}