#ifndef PICKLE_RENDER_H
#define PICKLE_RENDER_H

#include <stdbool.h>
#include <GLES2/gl2.h>
#include "drm.h"
#include "egl.h"
#include "error.h"
#include "frame_pacing.h"
#include "render_backend.h"

#ifdef VULKAN_ENABLED
#include "vulkan.h"
#endif

// Forward declarations
typedef struct mpv_handle mpv_handle;
typedef struct mpv_render_context mpv_render_context;

// Render context
typedef struct {
    // Render backend
    render_backend_type_t backend_type;
    
    // OpenGL ES resources
    GLuint fbo;
    GLuint texture;
    int texture_width;
    int texture_height;
    
    // Frame buffer info
    int current_width;
    int current_height;
    
    // Frame pacing
    frame_pacing_context_t frame_pacing;
    
    // Frame skipping
    bool skip_unchanged_frames;
    bool last_frame_unchanged;
    int unchanged_frames_count;
    
    // Rendering path
    bool direct_rendering;
    bool keystone_disabled;
    
    // Statistics
    unsigned long frames_rendered;
    unsigned long frames_skipped;
    double render_time_ms;
    double max_render_time_ms;
    
#ifdef VULKAN_ENABLED
    // Vulkan context
    vulkan_ctx_t vulkan;
#endif
} render_context_t;

// Initialize the render context
pickle_result_t render_init(render_context_t *ctx, double refresh_rate);

// Clean up the render context
void render_cleanup(render_context_t *ctx);

// Render a frame
bool render_frame(render_context_t *ctx, kms_ctx_t *drm, egl_ctx_t *egl, 
                  mpv_handle *mpv, mpv_render_context *mpv_ctx);

// Create framebuffer objects for rendering
pickle_result_t render_create_fbo(render_context_t *ctx, int width, int height);

// Destroy framebuffer objects
void render_destroy_fbo(render_context_t *ctx);

// Set frame skipping options
void render_set_frame_skipping(render_context_t *ctx, bool enabled);

// Set direct rendering option
void render_set_direct_rendering(render_context_t *ctx, bool enabled);

// Set keystone disabled option
void render_set_keystone_disabled(render_context_t *ctx, bool disabled);

// Get render statistics
void render_get_stats(render_context_t *ctx, char *buffer, size_t buffer_size);

// Set render backend
pickle_result_t render_set_backend(render_context_t *ctx, render_backend_type_t backend);

// Get current render backend name
const char* render_get_backend_name(render_context_t *ctx);

#endif // PICKLE_RENDER_H