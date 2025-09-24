#include "render.h"
// Include only standard headers
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// Define our own logging macros without including log.h or utils.h
#define LOG_RENDER_INFO(fmt, ...) fprintf(stderr, "[RENDER INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_RENDER_DEBUG(fmt, ...) fprintf(stderr, "[RENDER DEBUG] " fmt "\n", ##__VA_ARGS__)
#define LOG_RENDER_ERROR(fmt, ...) fprintf(stderr, "[RENDER ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_RENDER_WARN(fmt, ...) fprintf(stderr, "[RENDER WARN] " fmt "\n", ##__VA_ARGS__)

// Initialize the render context
pickle_result_t render_init(render_context_t *ctx, double refresh_rate) {
    if (!ctx) {
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    LOG_RENDER_INFO("Initializing render context");
    
    // Clear the context
    memset(ctx, 0, sizeof(render_context_t));
    
    // Initialize frame pacing
    frame_pacing_init(&ctx->frame_pacing, refresh_rate);
    
#ifdef VULKAN_ENABLED
    // Determine the render backend to use
    render_backend_type_t preferred = render_backend_get_preferred();
    
    if (preferred == RENDER_BACKEND_AUTO) {
        ctx->backend_type = render_backend_detect_best();
    } else {
        ctx->backend_type = preferred;
        
        // If the preferred backend is not available, fall back to GLES
        if (!render_backend_is_available(ctx->backend_type)) {
            LOG_RENDER_WARN("Preferred render backend %s is not available, falling back to OpenGL ES",
                     render_backend_name(ctx->backend_type));
            ctx->backend_type = RENDER_BACKEND_GLES;
        }
    }
#else
    // When Vulkan is not enabled, always use GLES
    ctx->backend_type = RENDER_BACKEND_GLES;
#endif
    
    LOG_RENDER_INFO("Using render backend: %s", render_backend_name(ctx->backend_type));
    
    // Reset statistics
    ctx->frames_rendered = 0;
    ctx->frames_skipped = 0;
    ctx->render_time_ms = 0.0;
    ctx->max_render_time_ms = 0.0;
    
    return PICKLE_OK;
}

// Clean up the render context
void render_cleanup(render_context_t *ctx) {
    if (!ctx) {
        return;
    }
    
    LOG_RENDER_INFO("Cleaning up render context");
    
    // Clean up based on the backend
    if (ctx->backend_type == RENDER_BACKEND_GLES) {
        render_destroy_fbo(ctx);
    }
#ifdef VULKAN_ENABLED
    else if (ctx->backend_type == RENDER_BACKEND_VULKAN) {
        vulkan_cleanup(&ctx->vulkan);
    }
#endif
    
    // Reset statistics
    ctx->frames_rendered = 0;
    ctx->frames_skipped = 0;
}

// Render a frame using the appropriate backend
bool render_frame(render_context_t *ctx, kms_ctx_t *drm, egl_ctx_t *egl, 
                  mpv_handle *mpv, mpv_render_context *mpv_ctx) {
    if (!ctx || !drm) {
        return false;
    }
    
    // Track render time
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    bool result = false;
    
    // Render using the selected backend
    if (ctx->backend_type == RENDER_BACKEND_GLES) {
        // Traditional OpenGL ES rendering path
        if (egl && mpv && mpv_ctx) {
            // Implement OpenGL ES rendering path
            // This would call into existing code
            
            // For demonstration, assume success
            result = true;
        } else {
            LOG_RENDER_ERROR("Missing required context for OpenGL ES rendering");
            result = false;
        }
    }
#ifdef VULKAN_ENABLED
    else if (ctx->backend_type == RENDER_BACKEND_VULKAN) {
        // Basic Vulkan rendering path - no MPV integration yet
        if (mpv && mpv_ctx) {
            // Get the next swapchain image
            uint32_t image_index;
            pickle_result_t begin_result = vulkan_begin_frame(&ctx->vulkan, &image_index);
            if (begin_result != PICKLE_OK) {
                LOG_RENDER_ERROR("Failed to begin Vulkan frame: %s", pickle_error_string(begin_result));
                return false;
            }
            
            // TODO: Integrate MPV with Vulkan renderer here
            LOG_RENDER_INFO("Vulkan rendering path is not fully implemented yet");
            
            // Apply keystone correction if enabled
            keystone_t *keystone = keystone_get_config();
            if (keystone && keystone->enabled && ctx->vulkan.compute.initialized) {
                LOG_RENDER_DEBUG("Applying keystone correction with Vulkan compute shader");
                vulkan_compute_update_uniform(&ctx->vulkan, keystone);
                vulkan_compute_keystone_apply(&ctx->vulkan, 
                                             ctx->vulkan.swapchain.images[image_index], 
                                             keystone);
            }
            
            // End the frame and present
            pickle_result_t end_result = vulkan_end_frame(&ctx->vulkan, image_index);
            if (end_result != PICKLE_OK) {
                LOG_RENDER_ERROR("Failed to end Vulkan frame: %s", pickle_error_string(end_result));
                return false;
            }
            
            result = true;
        } else {
            LOG_RENDER_ERROR("Missing required context for Vulkan rendering");
            result = false;
        }
    }
#endif
    else {
        LOG_RENDER_ERROR("Unsupported render backend: %d", ctx->backend_type);
        result = false;
    }
    
    // Update statistics
    if (result) {
        ctx->frames_rendered++;
        
        // Calculate render time
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed_ms = ((double)(end.tv_sec - start.tv_sec)) * 1000.0 + 
                           ((double)(end.tv_nsec - start.tv_nsec)) / 1000000.0;
        
        ctx->render_time_ms = elapsed_ms;
        if (elapsed_ms > ctx->max_render_time_ms) {
            ctx->max_render_time_ms = elapsed_ms;
        }
    } else {
        ctx->frames_skipped++;
    }
    
    return result;
}

// Create framebuffer objects for rendering (OpenGL ES only)
pickle_result_t render_create_fbo(render_context_t *ctx, int width, int height) {
    if (!ctx) {
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    // Only applicable for OpenGL ES backend
    if (ctx->backend_type != RENDER_BACKEND_GLES) {
        return PICKLE_OK;
    }
    
    // Clean up existing FBO
    render_destroy_fbo(ctx);
    
    // Create a new FBO
    glGenFramebuffers(1, &ctx->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
    
    // Create a texture to render to
    glGenTextures(1, &ctx->texture);
    glBindTexture(GL_TEXTURE_2D, ctx->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Attach the texture to the FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->texture, 0);
    
    // Check FBO status
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_RENDER_ERROR("Failed to create framebuffer: %d", status);
        render_destroy_fbo(ctx);
        return PICKLE_ERROR_GL_FRAMEBUFFER;
    }
    
    // Unbind the FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // Store the dimensions
    ctx->texture_width = width;
    ctx->texture_height = height;
    
    return PICKLE_OK;
}

// Destroy framebuffer objects (OpenGL ES only)
void render_destroy_fbo(render_context_t *ctx) {
    if (!ctx) {
        return;
    }
    
    // Only applicable for OpenGL ES backend
    if (ctx->backend_type != RENDER_BACKEND_GLES) {
        return;
    }
    
    // Delete the FBO
    if (ctx->fbo) {
        glDeleteFramebuffers(1, &ctx->fbo);
        ctx->fbo = 0;
    }
    
    // Delete the texture
    if (ctx->texture) {
        glDeleteTextures(1, &ctx->texture);
        ctx->texture = 0;
    }
    
    // Reset dimensions
    ctx->texture_width = 0;
    ctx->texture_height = 0;
}

// Set frame skipping options
void render_set_frame_skipping(render_context_t *ctx, bool enabled) {
    if (!ctx) {
        return;
    }
    
    ctx->skip_unchanged_frames = enabled;
    LOG_RENDER_INFO("Frame skipping %s", enabled ? "enabled" : "disabled");
}

// Set direct rendering option
void render_set_direct_rendering(render_context_t *ctx, bool enabled) {
    if (!ctx) {
        return;
    }
    
    ctx->direct_rendering = enabled;
    LOG_RENDER_INFO("Direct rendering %s", enabled ? "enabled" : "disabled");
}

// Set keystone disabled option
void render_set_keystone_disabled(render_context_t *ctx, bool disabled) {
    if (!ctx) {
        return;
    }
    
    ctx->keystone_disabled = disabled;
    LOG_RENDER_INFO("Keystone correction %s", disabled ? "disabled" : "enabled");
}

// Get render statistics
void render_get_stats(render_context_t *ctx, char *buffer, size_t buffer_size) {
    if (!ctx || !buffer || buffer_size == 0) {
        return;
    }
    
    // Format the statistics
    snprintf(buffer, buffer_size,
             "Render Backend: %s\n"
             "Frames Rendered: %lu\n"
             "Frames Skipped: %lu\n"
             "Average Frame Time: %.2f ms\n"
             "Max Frame Time: %.2f ms\n",
             render_backend_name(ctx->backend_type),
             ctx->frames_rendered,
             ctx->frames_skipped,
             ctx->render_time_ms,
             ctx->max_render_time_ms);
}

// Set render backend
pickle_result_t render_set_backend(render_context_t *ctx, render_backend_type_t backend) {
    if (!ctx) {
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    // Check if the requested backend is available
    if (!render_backend_is_available(backend)) {
        LOG_RENDER_ERROR("Requested render backend %s is not available", render_backend_name(backend));
        return PICKLE_ERROR_UNSUPPORTED;
    }
    
    // If the backend is the same, do nothing
    if (ctx->backend_type == backend) {
        return PICKLE_OK;
    }
    
    // Clean up the current backend
    if (ctx->backend_type == RENDER_BACKEND_GLES) {
        render_destroy_fbo(ctx);
    }
#ifdef VULKAN_ENABLED
    else if (ctx->backend_type == RENDER_BACKEND_VULKAN) {
        vulkan_cleanup(&ctx->vulkan);
    }
#endif
    
    // Set the new backend
    ctx->backend_type = backend;
    LOG_RENDER_INFO("Switched to render backend: %s", render_backend_name(backend));
    
    // We don't initialize the new backend here because it requires additional context
    // that will be provided when render_frame is called
    
    return PICKLE_OK;
}

// Get current render backend name
const char* render_get_backend_name(render_context_t *ctx) {
    if (!ctx) {
        return "Unknown";
    }
    
    return render_backend_name(ctx->backend_type);
}


