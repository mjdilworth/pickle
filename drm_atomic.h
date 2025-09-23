#ifndef PICKLE_DRM_ATOMIC_H
#define PICKLE_DRM_ATOMIC_H

#include <stdbool.h>
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "drm.h"

/**
 * Initialize atomic modesetting for a DRM device
 *
 * @param d Pointer to KMS context
 * @return true if initialization succeeded, false otherwise
 */
bool init_atomic_modesetting(kms_ctx_t *d);

/**
 * Deinitialize atomic modesetting for a DRM device
 *
 * @param d Pointer to KMS context
 */
void deinit_atomic_modesetting(kms_ctx_t *d);

/**
 * Find a property ID by name on a DRM object
 *
 * @param fd DRM file descriptor
 * @param obj_id Object ID
 * @param obj_type Object type (DRM_MODE_OBJECT_*)
 * @param name Property name to find
 * @return Property ID or 0 if not found
 */
uint32_t find_property_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name);

/**
 * Commit a framebuffer to a CRTC using atomic modesetting
 *
 * @param d Pointer to KMS context
 * @param fb_id Framebuffer ID
 * @param user_data User data to pass to the commit
 * @return true if commit succeeded, false otherwise
 */
bool atomic_commit_fb(kms_ctx_t *d, uint32_t fb_id, void *user_data);

/**
 * Present a framebuffer using atomic modesetting
 *
 * @param d Pointer to KMS context
 * @param fb_id Framebuffer ID
 * @param wait_vsync Whether to wait for VSYNC
 * @return true if presentation succeeded, false otherwise
 */
bool atomic_present_framebuffer(kms_ctx_t *d, uint32_t fb_id, bool wait_vsync);

/**
 * Check if atomic modesetting is supported
 *
 * @return true if supported, false otherwise
 */
bool is_atomic_supported(void);

#endif /* PICKLE_DRM_ATOMIC_H */