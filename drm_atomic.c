#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm.h"
#include "log.h"

// Global property name to ID mapping
struct prop_ids {
    // CRTC properties
    uint32_t crtc_mode_id;
    uint32_t crtc_active;
    
    // Plane properties
    uint32_t plane_fb_id;
    uint32_t plane_crtc_id;
    uint32_t plane_crtc_x;
    uint32_t plane_crtc_y;
    uint32_t plane_src_x;
    uint32_t plane_src_y;
    uint32_t plane_src_w;
    uint32_t plane_src_h;
};

/**
 * Find a property ID by name on a DRM object
 *
 * @param fd DRM file descriptor
 * @param obj_id Object ID
 * @param obj_type Object type (DRM_MODE_OBJECT_*)
 * @param name Property name to find
 * @return Property ID or 0 if not found
 */
uint32_t find_property_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name) {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) {
        LOG_ERROR("Failed to get properties for object %u: %s", obj_id, strerror(errno));
        return 0;
    }
    
    uint32_t prop_id = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) continue;
        
        if (strcmp(prop->name, name) == 0) {
            prop_id = prop->prop_id;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }
    
    drmModeFreeObjectProperties(props);
    return prop_id;
}

/**
 * Find all required atomic property IDs for a DRM configuration
 *
 * @param d DRM context
 * @param props Pointer to property ID structure to fill
 * @return true on success, false on failure
 */
bool find_atomic_properties(kms_ctx_t *d, struct prop_ids *props) {
    if (!d || !props) return false;
    
    // Find CRTC properties
    props->crtc_mode_id = find_property_id(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    props->crtc_active = find_property_id(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    
    // Find plane properties
    props->plane_fb_id = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    props->plane_crtc_id = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    props->plane_crtc_x = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    props->plane_crtc_y = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    props->plane_src_x = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    props->plane_src_y = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    props->plane_src_w = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    props->plane_src_h = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    
    // Check if we found all required properties
    if (!props->crtc_mode_id || !props->crtc_active || !props->plane_fb_id) {
        LOG_ERROR("Failed to find required atomic properties (CRTC MODE_ID:%u, ACTIVE:%u, Plane FB_ID:%u)",
                 props->crtc_mode_id, props->crtc_active, props->plane_fb_id);
        return false;
    }
    
    return true;
}

/**
 * Initialize atomic modesetting for a DRM context
 *
 * @param d DRM context
 * @return true on success, false on failure
 */
bool init_atomic_modesetting(kms_ctx_t *d) {
    if (!d) return false;
    
    // We've already enabled DRM_CLIENT_CAP_ATOMIC, so atomic should be available
    // We'll verify by trying to allocate an atomic request
    drmModeAtomicReqPtr test_req = drmModeAtomicAlloc();
    if (!test_req) {
        LOG_ERROR("Failed to allocate atomic request - atomic modesetting not available");
        d->atomic_supported = false;
        return false;
    }
    drmModeAtomicFree(test_req);
    LOG_DRM("Atomic request allocation successful - atomic modesetting available");
    
    // Get atomic property IDs
    d->prop_ids = calloc(1, sizeof(struct prop_ids));
    if (!d->prop_ids) {
        LOG_ERROR("Out of memory allocating property IDs");
        return false;
    }
    
    if (!find_atomic_properties(d, (struct prop_ids *)d->prop_ids)) {
        free(d->prop_ids);
        d->prop_ids = NULL;
        d->atomic_supported = false;
        return false;
    }
    
    // Temporarily disable atomic modesetting due to segfault issues
    // TODO: Fix atomic page flip event handling
    d->atomic_supported = false;
    LOG_INFO("Atomic modesetting available but disabled (needs debugging)");
    return false;
}

/**
 * Clean up atomic modesetting resources
 *
 * @param d DRM context
 */
void deinit_atomic_modesetting(kms_ctx_t *d) {
    if (!d) return;
    
    if (d->prop_ids) {
        free(d->prop_ids);
        d->prop_ids = NULL;
    }
    
    d->atomic_supported = false;
}

/**
 * Present a framebuffer using atomic modesetting
 *
 * @param d DRM context
 * @param fb_id Framebuffer ID to present
 * @param wait_vsync Whether to wait for vsync
 * @return true on success, false on failure
 */
bool atomic_present_framebuffer(kms_ctx_t *d, uint32_t fb_id, bool wait_vsync) {
    if (!d || !d->atomic_supported || !d->prop_ids) {
        return false;
    }
    
    struct prop_ids *props = (struct prop_ids *)d->prop_ids;
    
    // Create an atomic request
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    if (!req) {
        LOG_ERROR("Failed to allocate atomic request: %s", strerror(errno));
        return false;
    }
    
    // Add properties to the request
    // Set plane properties
    drmModeAtomicAddProperty(req, d->plane, props->plane_fb_id, fb_id);
    drmModeAtomicAddProperty(req, d->plane, props->plane_crtc_id, d->crtc);
    drmModeAtomicAddProperty(req, d->plane, props->plane_crtc_x, 0);
    drmModeAtomicAddProperty(req, d->plane, props->plane_crtc_y, 0);
    
    // Source coordinates are in 16.16 fixed-point format
    drmModeAtomicAddProperty(req, d->plane, props->plane_src_x, 0);
    drmModeAtomicAddProperty(req, d->plane, props->plane_src_y, 0);
    drmModeAtomicAddProperty(req, d->plane, props->plane_src_w, d->mode.hdisplay << 16);
    drmModeAtomicAddProperty(req, d->plane, props->plane_src_h, d->mode.vdisplay << 16);
    
    // Set CRTC properties if it's the first frame
    if (!d->crtc_initialized) {
        drmModeAtomicAddProperty(req, d->crtc, props->crtc_active, 1);
        drmModeAtomicAddProperty(req, d->crtc, props->crtc_mode_id, d->mode_blob_id);
        d->crtc_initialized = true;
    }
    
    // Commit the atomic request
    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
    if (wait_vsync) {
        flags |= DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
    }
    
    int ret = drmModeAtomicCommit(d->fd, req, flags, NULL);
    drmModeAtomicFree(req);
    
    if (ret) {
        LOG_ERROR("Atomic commit failed: %s", strerror(errno));
        return false;
    }
    
    return true;
}