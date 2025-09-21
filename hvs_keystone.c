#include "hvs_keystone.h"
#include "dispmanx.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(DISPMANX_ENABLED)
#include <bcm_host.h>

// Internal state for HVS keystone
typedef struct {
    DISPMANX_DISPLAY_HANDLE_T display;
    DISPMANX_RESOURCE_HANDLE_T resource;
    DISPMANX_ELEMENT_HANDLE_T element;
    bool initialized;
    bool active;
    int display_width;
    int display_height;
    // HVS transformation parameters
    VC_IMAGE_TRANSFORM_T transform;
    // Vertices for the transformation
    int32_t src_rect[4]; // x0, y0, x1, y1
    int32_t dst_rect[8]; // x0, y0, x1, y1, x2, y2, x3, y3 (clockwise: TL, TR, BR, BL)
} hvs_keystone_state_t;

static hvs_keystone_state_t g_hvs_state = {0};

/**
 * Convert from normalized keystone coordinates to screen coordinates
 */
static void keystone_to_screen_coords(keystone_t *keystone, int width, int height, int32_t *dst_rect) {
    // Keystone points are normalized from 0.0 to 1.0
    // Map them to screen coordinates
    
    // Top-left
    dst_rect[0] = (int32_t)(keystone->points[0][0] * width);
    dst_rect[1] = (int32_t)(keystone->points[0][1] * height);
    
    // Top-right
    dst_rect[2] = (int32_t)(keystone->points[1][0] * width);
    dst_rect[3] = (int32_t)(keystone->points[1][1] * height);
    
    // Bottom-right
    dst_rect[4] = (int32_t)(keystone->points[3][0] * width);
    dst_rect[5] = (int32_t)(keystone->points[3][1] * height);
    
    // Bottom-left
    dst_rect[6] = (int32_t)(keystone->points[2][0] * width);
    dst_rect[7] = (int32_t)(keystone->points[2][1] * height);
}

bool hvs_keystone_is_supported(void) {
    // Check if we're running on RPi with DispmanX
    return is_dispmanx_supported();
}

bool hvs_keystone_init(void) {
    if (!hvs_keystone_is_supported()) {
        LOG_WARN("HVS keystone not supported on this platform");
        return false;
    }
    
    if (g_hvs_state.initialized) {
        // Already initialized
        return true;
    }
    
    // Initialize BCM host if not already done
    bcm_host_init();
    
    // Open the default display
    g_hvs_state.display = vc_dispmanx_display_open(0);
    if (g_hvs_state.display == DISPMANX_NO_HANDLE) {
        LOG_ERROR("Failed to open DispmanX display");
        return false;
    }
    
    // Get display information
    DISPMANX_MODEINFO_T mode_info;
    if (vc_dispmanx_display_get_info(g_hvs_state.display, &mode_info) != 0) {
        LOG_ERROR("Failed to get display information");
        vc_dispmanx_display_close(g_hvs_state.display);
        g_hvs_state.display = DISPMANX_NO_HANDLE;
        return false;
    }
    
    g_hvs_state.display_width = mode_info.width;
    g_hvs_state.display_height = mode_info.height;
    
    LOG_INFO("HVS: Display size: %dx%d", g_hvs_state.display_width, g_hvs_state.display_height);
    
    g_hvs_state.initialized = true;
    return true;
}

bool hvs_keystone_apply(keystone_t *keystone, int display_width, int display_height) {
    if (!g_hvs_state.initialized && !hvs_keystone_init()) {
        return false;
    }
    
    if (!keystone->enabled) {
        // Keystone correction is disabled, clean up if active
        if (g_hvs_state.active) {
            hvs_keystone_cleanup();
            g_hvs_state.active = false;
        }
        return true;
    }
    
    // Store display dimensions
    g_hvs_state.display_width = display_width;
    g_hvs_state.display_height = display_height;
    
    // Set up source rectangle (the full display)
    g_hvs_state.src_rect[0] = 0;
    g_hvs_state.src_rect[1] = 0;
    g_hvs_state.src_rect[2] = display_width << 16;  // Fixed-point 16.16 format
    g_hvs_state.src_rect[3] = display_height << 16; // Fixed-point 16.16 format
    
    // Set up destination rectangle (the keystone-corrected quad)
    keystone_to_screen_coords(keystone, display_width, display_height, g_hvs_state.dst_rect);
    
    // Start a new DispmanX update
    DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
    if (update == DISPMANX_NO_HANDLE) {
        LOG_ERROR("Failed to start DispmanX update");
        return false;
    }
    
    // If we already have an active element, remove it
    if (g_hvs_state.element != DISPMANX_NO_HANDLE) {
        vc_dispmanx_element_remove(update, g_hvs_state.element);
        g_hvs_state.element = DISPMANX_NO_HANDLE;
    }
    
    // Set up the VC_DISPMANX_ALPHA_T structure for transparency
    VC_DISPMANX_ALPHA_T alpha = {
        DISPMANX_FLAGS_ALPHA_FROM_SOURCE | DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS,
        255, // Alpha value
        0    // Mask
    };
    
    // Create a new element with the keystone transformation
    g_hvs_state.element = vc_dispmanx_element_add(
        update,                       // Update handle
        g_hvs_state.display,          // Display handle
        1,                            // Layer (higher values are on top)
        NULL,                         // Source rectangle
        DISPMANX_NO_HANDLE,           // Destination display (same as source)
        NULL,                         // Destination rectangle
        DISPMANX_PROTECTION_NONE,     // Protection
        &alpha,                       // Alpha blending
        NULL,                         // Clamp
        DISPMANX_NO_ROTATE            // Transform
    );
    
    if (g_hvs_state.element == DISPMANX_NO_HANDLE) {
        LOG_ERROR("Failed to add DispmanX element");
        vc_dispmanx_update_submit_sync(update);
        return false;
    }
    
    // Apply the keystone transformation using HVS
    if (vc_dispmanx_element_modified_src_rect(
            update,                   // Update handle
            g_hvs_state.element,      // Element handle
            (VC_RECT_T*)g_hvs_state.src_rect, // Source rectangle
            DISPMANX_NO_ROTATE        // Transform
        ) != 0) {
        LOG_ERROR("Failed to set source rectangle");
        vc_dispmanx_update_submit_sync(update);
        return false;
    }
    
    // Apply the keystone transformation using HVS
    if (vc_dispmanx_element_modified_dst_quad(
            update,                   // Update handle
            g_hvs_state.element,      // Element handle
            (VC_RECT_T*)g_hvs_state.dst_rect  // Destination quad
        ) != 0) {
        LOG_ERROR("Failed to set destination quad");
        vc_dispmanx_update_submit_sync(update);
        return false;
    }
    
    // Submit the update
    if (vc_dispmanx_update_submit_sync(update) != 0) {
        LOG_ERROR("Failed to submit DispmanX update");
        return false;
    }
    
    g_hvs_state.active = true;
    LOG_INFO("HVS keystone transformation applied");
    
    return true;
}

bool hvs_keystone_update(keystone_t *keystone) {
    if (!g_hvs_state.initialized || !g_hvs_state.active) {
        // Not initialized or not active, initialize and apply
        return hvs_keystone_apply(keystone, g_hvs_state.display_width, g_hvs_state.display_height);
    }
    
    // Just update the destination quad
    keystone_to_screen_coords(keystone, g_hvs_state.display_width, g_hvs_state.display_height, g_hvs_state.dst_rect);
    
    // Start a new DispmanX update
    DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
    if (update == DISPMANX_NO_HANDLE) {
        LOG_ERROR("Failed to start DispmanX update");
        return false;
    }
    
    // Apply the updated keystone transformation
    if (vc_dispmanx_element_modified_dst_quad(
            update,                   // Update handle
            g_hvs_state.element,      // Element handle
            (VC_RECT_T*)g_hvs_state.dst_rect  // Destination quad
        ) != 0) {
        LOG_ERROR("Failed to update destination quad");
        vc_dispmanx_update_submit_sync(update);
        return false;
    }
    
    // Submit the update
    if (vc_dispmanx_update_submit_sync(update) != 0) {
        LOG_ERROR("Failed to submit DispmanX update");
        return false;
    }
    
    return true;
}

void hvs_keystone_cleanup(void) {
    if (g_hvs_state.initialized) {
        // Clean up DispmanX resources
        if (g_hvs_state.element != DISPMANX_NO_HANDLE) {
            DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
            if (update != DISPMANX_NO_HANDLE) {
                vc_dispmanx_element_remove(update, g_hvs_state.element);
                vc_dispmanx_update_submit_sync(update);
            }
            g_hvs_state.element = DISPMANX_NO_HANDLE;
        }
        
        if (g_hvs_state.display != DISPMANX_NO_HANDLE) {
            vc_dispmanx_display_close(g_hvs_state.display);
            g_hvs_state.display = DISPMANX_NO_HANDLE;
        }
        
        g_hvs_state.initialized = false;
        g_hvs_state.active = false;
    }
}

#else // !defined(DISPMANX_ENABLED)

// Stub implementations for platforms without DispmanX support

bool hvs_keystone_is_supported(void) {
    return false;
}

bool hvs_keystone_init(void) {
    LOG_WARN("HVS keystone not supported on this platform");
    return false;
}

bool hvs_keystone_apply(keystone_t *keystone, int display_width, int display_height) {
    (void)keystone;
    (void)display_width;
    (void)display_height;
    return false;
}

bool hvs_keystone_update(keystone_t *keystone) {
    (void)keystone;
    return false;
}

void hvs_keystone_cleanup(void) {
    // Nothing to do
}

#endif // defined(DISPMANX_ENABLED)