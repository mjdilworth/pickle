#ifndef PICKLE_DRM_H
#define PICKLE_DRM_H

#include <stdbool.h>
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// Some minimal systems or older headers might miss certain DRM macros; guard them.
#ifndef DRM_MODE_TYPE_PREFERRED
#define DRM_MODE_TYPE_PREFERRED  (1<<3)
#endif

// Atomic modesetting support
#ifndef DRM_MODE_ATOMIC_TEST_ONLY
#define DRM_MODE_ATOMIC_TEST_ONLY 0x0100
#endif

#ifndef DRM_MODE_ATOMIC_NONBLOCK
#define DRM_MODE_ATOMIC_NONBLOCK 0x0200
#endif

#ifndef DRM_MODE_ATOMIC_ALLOW_MODESET
#define DRM_MODE_ATOMIC_ALLOW_MODESET 0x0400
#endif

// Define DRM_CAP_ATOMIC if not available
#ifndef DRM_CAP_ATOMIC
#define DRM_CAP_ATOMIC 0x20
#endif

#ifndef DRM_CLIENT_CAP_ATOMIC
#define DRM_CLIENT_CAP_ATOMIC 3
#endif

// Forward declaration
typedef struct kms_ctx kms_ctx_t;
typedef struct egl_ctx egl_ctx_t;

// DRM/KMS context structure
struct kms_ctx {
    int fd;
    drmModeRes *res;
    drmModeConnector *connector;
    drmModeEncoder *encoder;
    drmModeCrtc *orig_crtc;
    uint32_t crtc_id;
    uint32_t connector_id;
    drmModeModeInfo mode;
    
    // Atomic modesetting support
    bool atomic_supported;     // Whether atomic modesetting is supported
    bool crtc_initialized;     // Whether CRTC has been initialized with atomic
    uint32_t crtc;             // CRTC ID for atomic operations
    uint32_t plane;            // Primary plane ID
    uint32_t mode_blob_id;     // Mode blob ID for atomic modesetting
    void *prop_ids;            // Property IDs for atomic modesetting
};

// DRM/KMS initialization and management functions
bool init_drm(kms_ctx_t *d);
void deinit_drm(kms_ctx_t *d);
bool ensure_drm_master(int fd);
bool init_atomic_modesetting(kms_ctx_t *d);
void deinit_atomic_modesetting(kms_ctx_t *d);

// Status accessors
bool is_drm_master(void);
bool is_atomic_supported(void);
kms_ctx_t* kms_get_ctx(void);

// Zero-copy support functions
bool should_use_zero_copy(kms_ctx_t *d, egl_ctx_t *egl_ctx);
bool present_frame_zero_copy(kms_ctx_t *d, egl_ctx_t *egl_ctx, unsigned int texture, float *src_rect, float *dst_rect);

// Atomic modesetting functions
bool atomic_commit_fb(kms_ctx_t *d, uint32_t fb_id, void *user_data);
bool atomic_present_framebuffer(kms_ctx_t *d, uint32_t fb_id, bool wait_vsync);

#endif // PICKLE_DRM_H