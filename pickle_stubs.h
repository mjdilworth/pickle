#ifndef PICKLE_STUBS_H
#define PICKLE_STUBS_H

#include <stdbool.h>
#include "drm.h"
#include "egl.h"
#include "mpv.h"

// Frame buffer ring buffer allocation
void preallocate_fb_ring(kms_ctx_t *d, egl_ctx_t *e, int num_buffers);

// Fixed frame rendering function
bool render_frame_fixed(kms_ctx_t *d, egl_ctx_t *e, mpv_player_t *p);

#endif // PICKLE_STUBS_H