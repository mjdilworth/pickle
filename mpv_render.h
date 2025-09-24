#ifndef PICKLE_MPV_RENDER_H
#define PICKLE_MPV_RENDER_H

#include <stdbool.h>
#include "mpv.h"
#include "drm.h"
#include "egl.h"

// Simple wrapper for MPV rendering
bool render_frame_mpv(mpv_handle *mpv, mpv_render_context *mpv_gl, kms_ctx_t *drm, egl_ctx_t *eglc);

#endif // PICKLE_MPV_RENDER_H