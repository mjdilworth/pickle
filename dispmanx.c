#include "dispmanx.h"
#include "log.h"
#include "egl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if defined(DISPMANX_ENABLED)
#include <bcm_host.h>

// Check if running on RPi with DispmanX support
bool is_dispmanx_supported(void) {
    // Simple check for BCM host library availability
    static bool checked = false;
    static bool supported = false;
    
    if (!checked) {
        checked = true;
        
        // Try to initialize BCM host
        bcm_host_init();
        
        // TODO: Add more specific checks for RPi hardware
        supported = true;
    }
    
    return supported;
}

// Initialize DispmanX context
bool dispmanx_init(dispmanx_ctx_t *ctx) {
    if (!ctx) {
        LOG_ERROR("Invalid context pointer");
        return false;
    }
    
    // Initialize BCM host library
    bcm_host_init();
    
    // Get display size
    if (graphics_get_display_size(0, &ctx->screen_width, &ctx->screen_height) < 0) {
        LOG_ERROR("Failed to get display size");
        return false;
    }
    
    LOG_INFO("Display size: %dx%d", ctx->screen_width, ctx->screen_height);
    
    // Open display
    ctx->display = vc_dispmanx_display_open(0);
    if (ctx->display == DISPMANX_NO_HANDLE) {
        LOG_ERROR("Failed to open display");
        return false;
    }
    
    // Set up source/destination rectangles
    vc_dispmanx_rect_set(&ctx->src_rect, 0, 0, ctx->screen_width << 16, ctx->screen_height << 16);
    vc_dispmanx_rect_set(&ctx->dst_rect, 0, 0, ctx->screen_width, ctx->screen_height);
    
    // Initialize with no keystone correction
    ctx->keystone_enabled = false;
    ctx->transform = 0;
    
    // Create initial resource (will be used in display_frame)
    ctx->resource = DISPMANX_NO_HANDLE;
    ctx->element = DISPMANX_NO_HANDLE;
    
    return true;
}

// Clean up DispmanX resources
void dispmanx_destroy(dispmanx_ctx_t *ctx) {
    if (!ctx) return;
    
    // Remove element if exists
    if (ctx->element != DISPMANX_NO_HANDLE) {
       DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
        vc_dispmanx_element_remove(update, ctx->element);
        vc_dispmanx_update_submit_sync(update);
        ctx->element = DISPMANX_NO_HANDLE;
    }
    
    // Delete resource if exists
    if (ctx->resource != DISPMANX_NO_HANDLE) {
        vc_dispmanx_resource_delete(ctx->resource);
        ctx->resource = DISPMANX_NO_HANDLE;
    }
    
    // Close display
    if (ctx->display != DISPMANX_NO_HANDLE) {
        vc_dispmanx_display_close(ctx->display);
        ctx->display = DISPMANX_NO_HANDLE;
    }
}

// Display a frame using DispmanX direct hardware path
bool dispmanx_display_frame(dispmanx_ctx_t *ctx, uint32_t *buffer, uint32_t width, uint32_t height, uint32_t stride) {
    if (!ctx || !buffer || width == 0 || height == 0 || stride == 0) {
        LOG_ERROR("Invalid parameters for direct display");
        return false;
    }
    
    // Create or recreate resource if needed
    if (ctx->resource == DISPMANX_NO_HANDLE || width != ctx->frame_width || height != ctx->frame_height) {
        // Delete old resource if it exists
        if (ctx->resource != DISPMANX_NO_HANDLE) {
            vc_dispmanx_resource_delete(ctx->resource);
        }
        
        ctx->resource = vc_dispmanx_resource_create(VC_IMAGE_RGBA32, width, height, 0);
        if (ctx->resource == DISPMANX_NO_HANDLE) {
            LOG_ERROR("Failed to create resource");
            return false;
        }
        
        ctx->frame_width = width;
        ctx->frame_height = height;
    }
    
    // Write buffer to resource
    VC_RECT_T rect;
    vc_dispmanx_rect_set(&rect, 0, 0, width, height);
    
    // Stride is in bytes, but vc_dispmanx_resource_write_data expects it in pixels
    uint32_t pitch = stride / 4;
    vc_dispmanx_resource_write_data(ctx->resource, VC_IMAGE_RGBA32, pitch, buffer, &rect);
    
    // Start update
   DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
    
    // Add element if it doesn't exist, or update it if it does
    if (ctx->element == DISPMANX_NO_HANDLE) {
        // Add new element
        VC_DISPMANX_ALPHA_T alpha = {
            DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS,
            255, // Fully opaque
            0
        };
        
        // Add element with appropriate transform
        ctx->element = vc_dispmanx_element_add(
            update,
            ctx->display,
            0, // layer
            &ctx->dst_rect,
            ctx->resource,
            &ctx->src_rect,
            DISPMANX_PROTECTION_NONE,
            &alpha,
            NULL, // clamp
            ctx->keystone_enabled ? ctx->transform : DISPMANX_NO_ROTATE
        );
        
        if (ctx->element == DISPMANX_NO_HANDLE) {
            LOG_ERROR("Failed to create element");
            vc_dispmanx_update_submit_sync(update);
            return false;
        }
    } else {
        // Update existing element
        vc_dispmanx_element_change_source(update, ctx->element, ctx->resource);
        
        if (ctx->keystone_enabled) {
            vc_dispmanx_element_change_attributes(
                update,
                ctx->element,
                ELEMENT_CHANGE_DEST_RECT | (ctx->keystone_enabled ? ELEMENT_CHANGE_TRANSFORM : 0),
                0, // layer
                0, // opacity
                &ctx->dst_rect,
                &ctx->src_rect,
                NULL, // mask
                ctx->transform
            );
        }
    }
    
    // Submit update
    vc_dispmanx_update_submit_sync(update);
    
    return true;
}

// Create an EGL window using DispmanX
bool dispmanx_create_egl_window(dispmanx_ctx_t *ctx, egl_ctx_t *egl) {
    if (!ctx || !egl) {
        LOG_ERROR("Invalid parameters for EGL window creation");
        return false;
    }
    
    // Start update
   DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
    
    // Add element if it doesn't exist
    if (ctx->element == DISPMANX_NO_HANDLE) {
        VC_DISPMANX_ALPHA_T alpha = {
            DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS,
            255, // Fully opaque
            0
        };
        
        ctx->element = vc_dispmanx_element_add(
            update,
            ctx->display,
            0, // layer
            &ctx->dst_rect,
            0, // resource (none for EGL window)
            &ctx->src_rect,
            DISPMANX_PROTECTION_NONE,
            &alpha,
            NULL, // clamp
            ctx->keystone_enabled ? ctx->transform : DISPMANX_NO_ROTATE
        );
        
        if (ctx->element == DISPMANX_NO_HANDLE) {
            LOG_ERROR("Failed to create element for EGL window");
            vc_dispmanx_update_submit_sync(update);
            return false;
        }
    }
    
    // Submit update
    vc_dispmanx_update_submit_sync(update);
    
    // Create EGL window structure
    ctx->egl_window.width = ctx->screen_width;
    ctx->egl_window.height = ctx->screen_height;
    ctx->egl_window.handle = ctx->element;
    
    // Set EGL native window
    egl->native_window = &ctx->egl_window;
    
    return true;
}

// Apply keystone transformation
bool dispmanx_apply_keystone(dispmanx_ctx_t *ctx, float *corners) {
    if (!ctx || !corners) {
        LOG_ERROR("Invalid parameters for keystone correction");
        return false;
    }
    
    // TODO: Implement keystone correction using DispmanX transform
    // This requires mapping the corners to a transform matrix
    
    // Enable keystone mode
    ctx->keystone_enabled = true;
    
    return true;
}

#else // !defined(DISPMANX_ENABLED)

// Stub implementations for platforms without DispmanX support

bool is_dispmanx_supported(void) {
    return false;
}

bool dispmanx_init(dispmanx_ctx_t *ctx) {
    (void)ctx; // unused
    return false;
}

void dispmanx_destroy(dispmanx_ctx_t *ctx) {
    (void)ctx; // unused
}

bool dispmanx_display_frame(dispmanx_ctx_t *ctx, uint32_t *buffer, uint32_t width, uint32_t height, uint32_t stride) {
    (void)ctx; // unused
    (void)buffer; // unused
    (void)width; // unused
    (void)height; // unused
    (void)stride; // unused
    return false;
}

bool dispmanx_create_egl_window(dispmanx_ctx_t *ctx, egl_ctx_t *egl) {
    (void)ctx; // unused
    (void)egl; // unused
    return false;
}

bool dispmanx_apply_keystone(dispmanx_ctx_t *ctx, float *corners) {
    (void)ctx; // unused
    (void)corners; // unused
    return false;
}

#endif // defined(DISPMANX_ENABLED)
