#include "drm.h"
#include "utils.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

// Define O_CLOEXEC if not already defined
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif

// Define logging macros if not already defined
#ifndef LOG_DRM
#define LOG_DRM(fmt, ...) fprintf(stderr, "[DRM] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef LOG_ERROR
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef RETURN_ERROR
#define RETURN_ERROR(msg) do { LOG_ERROR("%s", msg); return false; } while(0)
#endif

#ifndef RETURN_ERROR_ERRNO
#define RETURN_ERROR_ERRNO(msg) do { LOG_ERROR("%s: %s", msg, strerror(errno)); return false; } while(0)
#endif

// Global state
static int g_have_master = 0; // set if we successfully become DRM master
static kms_ctx_t *g_kms_ctx = NULL; // global reference to active KMS context

/**
 * Attempt to become DRM master
 * 
 * @param fd DRM device file descriptor
 * @return true if successful or already master, false otherwise
 */
bool ensure_drm_master(int fd) {
    // Attempt to become DRM master; non-fatal if it fails (we may still page flip if compositor allows)
    if (drmSetMaster(fd) == 0) {
        LOG_DRM("Acquired master");
        g_have_master = 1;
        return true;
    }
    LOG_DRM("drmSetMaster failed (%s) – another process may own the display. Modeset might fail.", strerror(errno));
    g_have_master = 0;
    return false;
}

/**
 * Check if we are currently DRM master
 * 
 * @return true if we are currently DRM master, false otherwise
 */
bool is_drm_master(void) {
    return g_have_master != 0;
}

/**
 * Check if atomic modesetting is supported
 * 
 * @return true if atomic modesetting is supported, false otherwise
 */
bool is_atomic_supported(void) {
    if (!g_kms_ctx) return false;
    return g_kms_ctx->atomic_supported;
}

/**
 * Get the global KMS context
 * 
 * @return Pointer to the global KMS context or NULL if not initialized
 */
kms_ctx_t* kms_get_ctx(void) {
    return g_kms_ctx;
}

/**
 * Initialize DRM by scanning available cards and finding one with a connected display
 * 
 * @param d Pointer to kms_ctx structure to initialize
 * @return true on success, false on failure
 */
bool init_drm(kms_ctx_t *d) {
    memset(d, 0, sizeof(*d));
    d->fd = -1; // Initialize to invalid
    
    // Store global reference
    g_kms_ctx = d;

    // Enumerate potential /dev/dri/card* nodes (0-15) to find one with resources + connected connector.
    // On Raspberry Pi 4 with full KMS (dtoverlay=vc4-kms-v3d), the primary render/display node
    // is typically card1 (card0 often firmware emulation or simpledrm early driver). We scan
    // all to stay generic across distro kernels.
    char path[32];
    bool found_card = false;
    
    for (int idx=0; idx<16; ++idx) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", idx);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue; // skip silently; permission or non-existent
        }
        
        drmModeRes *res = drmModeGetResources(fd);
        if (!res) {
            LOG_DRM("card%d: drmModeGetResources failed: %s", idx, strerror(errno));
            close(fd);
            continue;
        }
        
        // Scan for a connected connector
        drmModeConnector *chosen = NULL;
        for (int i=0; i<res->count_connectors; ++i) {
            drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
            if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
                chosen = conn;
                break;
            }
            if (conn) drmModeFreeConnector(conn);
        }
        
        if (!chosen) {
            drmModeFreeResources(res);
            close(fd);
            continue; // try next card
        }
        
        // Found a suitable device
        d->fd = fd;
        d->res = res;
        d->connector = chosen;
        d->connector_id = chosen->connector_id;
        
        // Pick preferred mode if flagged, else first.
        d->mode = chosen->modes[0];
        for (int mi = 0; mi < chosen->count_modes; ++mi) {
            if (chosen->modes[mi].type & DRM_MODE_TYPE_PREFERRED) { 
                d->mode = chosen->modes[mi]; 
                break; 
            }
        }
        
        LOG_DRM("Selected card path %s", path);
        ensure_drm_master(fd);
        found_card = true;
        break;
    }
    
    if (!found_card || d->fd < 0 || !d->connector) {
        LOG_ERROR("Failed to locate a usable DRM device");
        LOG_ERROR("Troubleshooting: Ensure vc4 KMS overlay enabled and you have permission (try sudo or be in 'video' group)");
        return false;
    }

    // Find encoder for connector
    if (d->connector->encoder_id) {
        d->encoder = drmModeGetEncoder(d->fd, d->connector->encoder_id);
    }
    
    if (!d->encoder) {
        for (int i=0; i<d->connector->count_encoders; ++i) {
            d->encoder = drmModeGetEncoder(d->fd, d->connector->encoders[i]);
            if (d->encoder) break;
        }
    }
    
    if (!d->encoder) {
        LOG_ERROR("No encoder found for connector %u", d->connector_id);
        return false;
    }
    
    d->crtc_id = d->encoder->crtc_id;
    d->orig_crtc = drmModeGetCrtc(d->fd, d->crtc_id);
    
    if (!d->orig_crtc) {
        LOG_ERROR("Failed to get original CRTC (%s)", strerror(errno));
        return false;
    }
    
    // Get the CRTC to use
    d->crtc = d->encoder->crtc_id;
    
    // Find a suitable plane for display
    drmModePlaneResPtr planes = drmModeGetPlaneResources(d->fd);
    if (!planes) {
        LOG_ERROR("Failed to get plane resources: %s", strerror(errno));
        return false;
    }
    
    // Look for a primary plane that can be used with our CRTC
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(d->fd, planes->planes[i]);
        if (!plane) continue;
        
        // Check if this plane works with our CRTC
        bool compatible = false;
        for (uint32_t j = 0; j < plane->count_formats; j++) {
            if (plane->possible_crtcs & (1 << 0)) {  // Primary CRTC
                compatible = true;
                break;
            }
        }
        
        if (compatible) {
            d->plane = plane->plane_id;
            drmModeFreePlane(plane);
            break;
        }
        
        drmModeFreePlane(plane);
    }
    
    drmModeFreePlaneResources(planes);
    
    if (!d->plane) {
        LOG_ERROR("Failed to find a suitable plane for display");
        return false;
    }
    
    // Create a blob for the selected mode
    if (drmModeCreatePropertyBlob(d->fd, &d->mode, sizeof(d->mode), &d->mode_blob_id) != 0) {
        LOG_ERROR("Failed to create mode property blob: %s", strerror(errno));
        return false;
    }
    
    // Initialize atomic modesetting if supported
    d->atomic_supported = false;
    d->crtc_initialized = false;
    if (init_atomic_modesetting(d)) {
        LOG_INFO("Atomic modesetting initialized successfully");
    } else {
        LOG_DEBUG("Using legacy modesetting (no atomic support)");
    }
    
    LOG_DRM("Using card with fd=%d connector=%u mode=%s %ux%u@%u", 
        d->fd, d->connector_id, d->mode.name, 
        d->mode.hdisplay, d->mode.vdisplay, d->mode.vrefresh);

    return true;
}

/**
 * Clean up DRM resources and restore original CRTC state
 * 
 * @param d Pointer to kms_ctx structure to clean up
 */
void deinit_drm(kms_ctx_t *d) {
    if (!d) return;
    
    // Clean up atomic modesetting resources
    if (d->atomic_supported) {
        deinit_atomic_modesetting(d);
    }
    
    // Clear global reference if it points to this context
    if (g_kms_ctx == d) {
        g_kms_ctx = NULL;
    }
    
    // Destroy mode blob if created
    if (d->mode_blob_id) {
        drmModeDestroyPropertyBlob(d->fd, d->mode_blob_id);
        d->mode_blob_id = 0;
    }
    
    if (d->orig_crtc) {
        // Restore original CRTC configuration
        drmModeSetCrtc(d->fd, d->orig_crtc->crtc_id, d->orig_crtc->buffer_id,
                      d->orig_crtc->x, d->orig_crtc->y, &d->connector_id, 1, &d->orig_crtc->mode);
        drmModeFreeCrtc(d->orig_crtc);
        d->orig_crtc = NULL;
    }
    
    if (d->encoder) {
        drmModeFreeEncoder(d->encoder);
        d->encoder = NULL;
    }
    
    if (d->connector) {
        drmModeFreeConnector(d->connector);
        d->connector = NULL;
    }
    
    if (d->res) {
        drmModeFreeResources(d->res);
        d->res = NULL;
    }
    
    if (d->fd >= 0) {
        // Drop DRM master if we had it
        if (g_have_master) {
            drmDropMaster(d->fd);
            g_have_master = 0;
        }
        close(d->fd);
        d->fd = -1;
    }
}