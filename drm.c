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

// Define DRM_MODE_OBJECT_PLANE if not available
#ifndef DRM_MODE_OBJECT_PLANE
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeee
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
        
        // Enable atomic modesetting client capability early
        int atomic_ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
        LOG_DRM("DRM_CLIENT_CAP_ATOMIC enable early: ret=%d, fd=%d", atomic_ret, fd);
        
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
    uint32_t crtc_index = 0;
    for (int i = 0; i < (int)d->res->count_crtcs; i++) {
        if (d->res->crtcs[i] == d->crtc) {
            crtc_index = (uint32_t)i;
            break;
        }
    }
    
    LOG_DRM("Looking for plane compatible with CRTC index %u (CRTC ID %u)", crtc_index, d->crtc);
    
    // First, try to find primary planes by checking known plane IDs from modetest
    // Primary planes are often not in the plane resources list on some drivers
    uint32_t primary_plane_candidates[] = {46, 65, 77, 89, 101, 113};
    for (uint32_t i = 0; i < sizeof(primary_plane_candidates)/sizeof(primary_plane_candidates[0]); i++) {
        uint32_t plane_id = primary_plane_candidates[i];
        drmModePlanePtr plane = drmModeGetPlane(d->fd, plane_id);
        if (plane && (plane->possible_crtcs & (1 << crtc_index))) {
            // Verify it's a primary plane
            drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(d->fd, plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; j++) {
                    drmModePropertyPtr prop = drmModeGetProperty(d->fd, props->props[j]);
                    if (prop && strcmp(prop->name, "type") == 0) {
                        if (props->prop_values[j] == 1) {  // DRM_PLANE_TYPE_PRIMARY
                            d->plane = plane_id;
                            LOG_DRM("Found primary plane %u (possible_crtcs=0x%x)", d->plane, plane->possible_crtcs);
                            drmModeFreeProperty(prop);
                            drmModeFreeObjectProperties(props);
                            drmModeFreePlane(plane);
                            drmModeFreePlaneResources(planes);
                            goto found_primary_plane;
                        }
                    }
                    if (prop) drmModeFreeProperty(prop);
                }
                drmModeFreeObjectProperties(props);
            }
        }
        if (plane) drmModeFreePlane(plane);
    }
    
    LOG_DRM("No primary plane found in candidates, checking enumerated planes...");
    LOG_DRM("Total planes available: %u", planes->count_planes);
    
    // First pass: check all planes and log their details
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(d->fd, planes->planes[i]);
        if (!plane) continue;
        
        // Get plane type
        uint64_t plane_type = 0;
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(d->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyPtr prop = drmModeGetProperty(d->fd, props->props[j]);
                if (prop && strcmp(prop->name, "type") == 0) {
                    plane_type = props->prop_values[j];
                    drmModeFreeProperty(prop);
                    break;
                }
                if (prop) drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }
        
        LOG_DRM("Plane %u: type=%llu, crtc_id=%u, possible_crtcs=0x%x, compatible=%s", 
               plane->plane_id, (unsigned long long)plane_type, plane->crtc_id, plane->possible_crtcs,
               (plane->possible_crtcs & (1 << crtc_index)) ? "YES" : "NO");
        
        drmModeFreePlane(plane);
    }
    
    // Second pass: find a compatible primary plane first, then any compatible plane
    uint32_t fallback_plane = 0;
    
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(d->fd, planes->planes[i]);
        if (!plane) continue;
        
        // Check if this plane works with our CRTC
        if (plane->possible_crtcs & (1 << crtc_index)) {
            // Verify plane type by checking its type property
            drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(d->fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
            if (props) {
                for (uint32_t j = 0; j < props->count_props; j++) {
                    drmModePropertyPtr prop = drmModeGetProperty(d->fd, props->props[j]);
                    if (prop && strcmp(prop->name, "type") == 0) {
                        if (props->prop_values[j] == 1) {  // DRM_PLANE_TYPE_PRIMARY
                            d->plane = plane->plane_id;
                            LOG_DRM("Selected primary plane %u", d->plane);
                            drmModeFreeProperty(prop);
                            drmModeFreeObjectProperties(props);
                            drmModeFreePlane(plane);
                            goto found_plane;
                        } else if (props->prop_values[j] == 0 && !fallback_plane) {  // DRM_PLANE_TYPE_OVERLAY
                            fallback_plane = plane->plane_id;
                            LOG_DRM("Found compatible overlay plane %u as fallback", fallback_plane);
                        }
                    }
                    if (prop) drmModeFreeProperty(prop);
                }
                drmModeFreeObjectProperties(props);
            }
        }
        
        drmModeFreePlane(plane);
    }
    
    // If no primary plane found, use overlay plane as fallback
    if (!d->plane && fallback_plane) {
        d->plane = fallback_plane;
        LOG_DRM("Using overlay plane %u as fallback", d->plane);
    }
    
found_plane:
    
    drmModeFreePlaneResources(planes);
    
    if (!d->plane) {
        LOG_ERROR("Failed to find a suitable plane for display");
        return false;
    }
    
found_primary_plane:
    
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