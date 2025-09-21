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
    uint32_t crtc_id;
    uint32_t mode_id;
    uint32_t active;
    uint32_t fb_id;
    uint32_t crtc_x;
    uint32_t crtc_y;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_w;
    uint32_t src_h;
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
    props->crtc_id = find_property_id(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "CRTC_ID");
    props->mode_id = find_property_id(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    props->active = find_property_id(d->fd, d->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    
    // Find plane properties
    props->fb_id = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    props->crtc_id = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    props->crtc_x = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    props->crtc_y = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    props->src_x = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    props->src_y = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    props->src_w = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    props->src_h = find_property_id(d->fd, d->plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    
    // Check if we found all required properties
    if (!props->crtc_id || !props->active || !props->fb_id) {
        LOG_ERROR("Failed to find required atomic properties");
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
    
    // Check if atomic modesetting is supported
    uint64_t cap = 0;
    if (drmGetCap(d->fd, DRM_CAP_ATOMIC, &cap) < 0 || !cap) {
        LOG_ERROR("Atomic modesetting not supported by DRM driver - zero-copy performance will be limited");
        d->atomic_supported = false;
        return false;
    }
    
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
    
    d->atomic_supported = true;
    LOG_INFO("Atomic modesetting initialized successfully");
    return true;
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
    drmModeAtomicAddProperty(req, d->plane, props->fb_id, fb_id);
    drmModeAtomicAddProperty(req, d->plane, props->crtc_id, d->crtc);
    drmModeAtomicAddProperty(req, d->plane, props->crtc_x, 0);
    drmModeAtomicAddProperty(req, d->plane, props->crtc_y, 0);
    
    // Source coordinates are in 16.16 fixed-point format
    drmModeAtomicAddProperty(req, d->plane, props->src_x, 0);
    drmModeAtomicAddProperty(req, d->plane, props->src_y, 0);
    drmModeAtomicAddProperty(req, d->plane, props->src_w, d->mode.hdisplay << 16);
    drmModeAtomicAddProperty(req, d->plane, props->src_h, d->mode.vdisplay << 16);
    
    // Set CRTC properties if it's the first frame
    if (!d->crtc_initialized) {
        drmModeAtomicAddProperty(req, d->crtc, props->active, 1);
        drmModeAtomicAddProperty(req, d->crtc, props->mode_id, d->mode_blob_id);
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