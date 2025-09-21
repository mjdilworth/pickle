#include "render.h"
#include "keystone.h"
#include "hvs_keystone.h"
#include "compute_keystone.h"
#include "utils.h"
#include "mpv.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

// Initialize the render context
pickle_result_t render_init(render_context_t *ctx, double refresh_rate) {
    if (!ctx) {
        return PICKLE_ERROR_INVALID_PARAM;
    }
    
    // Clear the context
    memset(ctx, 0, sizeof(render_context_t));
    
    // Initialize frame pacing
    pickle_result_t result = frame_pacing_init(&ctx->frame_pacing, refresh_rate);
    if (result != PICKLE_SUCCESS) {
        return result;
    }
    
    // Set default values
    ctx->skip_unchanged_frames = true;
    ctx->direct_rendering = true;
    ctx->keystone_disabled = false;
    
    return PICKLE_SUCCESS;
}

// Clean up the render context
void render_cleanup(render_context_t *ctx) {
    if (!ctx) {
        return;
    }
    
    // Destroy FBO if it exists
    render_destroy_fbo(ctx);
}

// Create framebuffer objects for rendering
pickle_result_t render_create_fbo(render_context_t *ctx, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) {
        return PICKLE_ERROR_INVALID_PARAM;
    }
    
    // Clean up existing FBO if any
    render_destroy_fbo(ctx);
    
    // Create texture
    glGenTextures(1, &ctx->texture);
    glBindTexture(GL_TEXTURE_2D, ctx->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Create FBO
    glGenFramebuffers(1, &ctx->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->texture, 0);
    
    // Check if FBO is complete
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("FBO creation failed: 0x%x", status);
        render_destroy_fbo(ctx);
        return PICKLE_ERROR_GL;
    }
    
    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // Store dimensions
    ctx->texture_width = width;
    ctx->texture_height = height;
    
    return PICKLE_SUCCESS;
}

// Destroy framebuffer objects
void render_destroy_fbo(render_context_t *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->fbo) {
        glDeleteFramebuffers(1, &ctx->fbo);
        ctx->fbo = 0;
    }
    
    if (ctx->texture) {
        glDeleteTextures(1, &ctx->texture);
        ctx->texture = 0;
    }
    
    ctx->texture_width = 0;
    ctx->texture_height = 0;
}

// Set frame skipping options
void render_set_frame_skipping(render_context_t *ctx, bool enabled) {
    if (ctx) {
        ctx->skip_unchanged_frames = enabled;
    }
}

// Set direct rendering option
void render_set_direct_rendering(render_context_t *ctx, bool enabled) {
    if (ctx) {
        ctx->direct_rendering = enabled;
    }
}

// Set keystone disabled option
void render_set_keystone_disabled(render_context_t *ctx, bool disabled) {
    if (ctx) {
        ctx->keystone_disabled = disabled;
    }
}

// Render a frame
bool render_frame(render_context_t *ctx, kms_ctx_t *drm, egl_ctx_t *egl, 
                  mpv_handle *mpv, mpv_render_context *mpv_ctx) {
    if (!ctx || !drm || !egl || !mpv || !mpv_ctx) {
        LOG_ERROR("Invalid parameters for render_frame");
        return false;
    }

    // Begin timing
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);

    // Wait for the next frame if frame pacing is enabled
    if (!frame_pacing_wait_next_frame(&ctx->frame_pacing)) {
        ctx->frames_skipped++;
        return true;  // Skip rendering but return true to continue
    }

    // Check if we have a new frame
    bool has_frame = mpv_render_context_update(mpv_ctx) & MPV_RENDER_UPDATE_FRAME;

    // Skip rendering if the frame hasn't changed and skip_unchanged_frames is enabled
    if (!has_frame && ctx->skip_unchanged_frames) {
        ctx->last_frame_unchanged = true;
        ctx->unchanged_frames_count++;
        LOG_DEBUG("Skipping unchanged frame (#%d)", ctx->unchanged_frames_count);
        return true;
    }

    // Reset unchanged counter if we have a new frame
    if (has_frame) {
        ctx->last_frame_unchanged = false;
        ctx->unchanged_frames_count = 0;
    }

    // Detect if we need to resize the render target
    int width = drm->mode.hdisplay;
    int height = drm->mode.vdisplay;
    
    if (ctx->current_width != width || ctx->current_height != height) {
        LOG_INFO("Resize render target: %dx%d", width, height);
        ctx->current_width = width;
        ctx->current_height = height;
    }

    // Determine rendering path based on keystone state
    bool use_direct_rendering = ctx->direct_rendering && 
                              (ctx->keystone_disabled || !g_keystone.enabled);

    if (use_direct_rendering) {
        // Direct rendering path - render directly to the screen
        mpv_opengl_fbo mpv_fbo = {
            .fbo = 0,  // Default framebuffer
            .w = width,
            .h = height,
            .internal_format = 0  // Not needed when rendering to screen
        };

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &(int){1}},
            {0}
        };

        // Render MPV directly to screen
        mpv_render_context_render(mpv_ctx, params);
    } else {
        // Keystone path - render to FBO, then apply keystone

        // Make sure our FBO is the right size
        if (ctx->texture_width != width || ctx->texture_height != height) {
            render_create_fbo(ctx, width, height);
        }

        // First render MPV to our FBO
        mpv_opengl_fbo mpv_fbo = {
            .fbo = ctx->fbo,
            .w = width,
            .h = height,
            .internal_format = GL_RGBA
        };

        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &(int){1}},
            {0}
        };

        // Bind our FBO and render MPV to it
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
        mpv_render_context_render(mpv_ctx, params);

        // Now render the FBO texture to the screen with keystone correction
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        render_with_keystone(ctx->texture, width, height);
    }

    // End timing and update stats
    gettimeofday(&end_time, NULL);
    double frame_time_ms = tv_diff(&start_time, &end_time) * 1000.0;
    
    ctx->render_time_ms = frame_time_ms;
    if (frame_time_ms > ctx->max_render_time_ms) {
        ctx->max_render_time_ms = frame_time_ms;
    }
    
    ctx->frames_rendered++;
    
    // Update frame pacing
    frame_pacing_frame_presented(&ctx->frame_pacing);
    
    return true;
}

// Get render statistics
void render_get_stats(render_context_t *ctx, char *buffer, size_t buffer_size) {
    if (!ctx || !buffer || buffer_size == 0) {
        return;
    }
    
    char frame_pacing_stats[512] = {0};
    frame_pacing_get_stats(&ctx->frame_pacing, frame_pacing_stats, sizeof(frame_pacing_stats));
    
    snprintf(buffer, buffer_size,
            "Render Stats:\n"
            "  Frames rendered: %lu\n"
            "  Frames skipped: %lu\n"
            "  Unchanged frames: %d\n"
            "  Current render time: %.2f ms\n"
            "  Max render time: %.2f ms\n"
            "  Direct rendering: %s\n"
            "  Keystone disabled: %s\n"
            "%s",
            ctx->frames_rendered,
            ctx->frames_skipped,
            ctx->unchanged_frames_count,
            ctx->render_time_ms,
            ctx->max_render_time_ms,
            ctx->direct_rendering ? "yes" : "no",
            ctx->keystone_disabled ? "yes" : "no",
            frame_pacing_stats);
}

// Render a texture with keystone correction
void render_with_keystone(GLuint texture, int width, int height) {
    // Check if hardware HVS keystone is supported and enabled
    if (hvs_keystone_is_supported() && g_keystone.enabled) {
        // Apply hardware HVS keystone transformation
        keystone_t *keystone = get_keystone_data();
        if (hvs_keystone_apply(keystone, width, height)) {
            // When using HVS keystone, we still need to render the texture to the screen,
            // but without OpenGL keystone correction since HVS will handle it
            LOG_DEBUG("Using HVS keystone transformation");
            
            // Simple full-screen texture rendering
            glDisable(GL_BLEND);
            glViewport(0, 0, width, height);
            
            // Render the texture to the screen
            glClear(GL_COLOR_BUFFER_BIT);
            
            // Set up basic texture drawing
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture);
            
            // Use default shader program for simple texture rendering
            // (This assumes a basic shader is available in the shader module)
            GLuint prog = get_basic_shader_program();
            glUseProgram(prog);
            
            // Draw a full-screen quad
            GLfloat vertices[] = {
                -1.0f, -1.0f,  // Bottom left
                 1.0f, -1.0f,  // Bottom right
                 1.0f,  1.0f,  // Top right
                -1.0f,  1.0f   // Top left
            };
            
            GLfloat texcoords[] = {
                0.0f, 0.0f,  // Bottom left
                1.0f, 0.0f,  // Bottom right
                1.0f, 1.0f,  // Top right
                0.0f, 1.0f   // Top left
            };
            
            GLuint indices[] = {
                0, 1, 2,  // First triangle
                0, 2, 3   // Second triangle
            };
            
            GLint pos_attrib = glGetAttribLocation(prog, "position");
            GLint tex_attrib = glGetAttribLocation(prog, "texcoord");
            GLint tex_uniform = glGetUniformLocation(prog, "texture");
            
            glUniform1i(tex_uniform, 0);  // Texture unit 0
            
            // Set up vertex attributes
            glEnableVertexAttribArray(pos_attrib);
            glVertexAttribPointer(pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, vertices);
            
            glEnableVertexAttribArray(tex_attrib);
            glVertexAttribPointer(tex_attrib, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
            
            // Draw the quad
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, indices);
            
            // Clean up
            glDisableVertexAttribArray(pos_attrib);
            glDisableVertexAttribArray(tex_attrib);
            
            return;
        }
    }

    // Try compute shader-based keystone if available and enabled
    if (compute_keystone_is_supported() && g_keystone.enabled) {
        // Apply compute shader-based keystone transformation
        keystone_t *keystone = get_keystone_data();
        if (compute_keystone_apply(keystone, texture, width, height)) {
            LOG_DEBUG("Using compute shader keystone transformation");
            return;
        }
    }
    
    // Fallback to software keystone if hardware HVS or compute shader is not available or failed
    LOG_DEBUG("Using software keystone transformation");
    
    // Ensure keystone shader is initialized
    if (!g_keystone_shader_program && !init_keystone_shader()) {
        LOG_ERROR("Failed to initialize keystone shader");
        return;
    }
    
    // Set up viewport and clear screen
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Bind the texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    // Use keystone shader program
    glUseProgram(g_keystone_shader_program);
    
    // Set texture uniform
    glUniform1i(g_keystone_u_texture_loc, 0);  // Texture unit 0
    
    // Create quad vertices and texture coordinates
    GLfloat vertices[] = {
        -1.0f, -1.0f,  // Bottom left
         1.0f, -1.0f,  // Bottom right
         1.0f,  1.0f,  // Top right
        -1.0f,  1.0f   // Top left
    };
    
    GLfloat texcoords[] = {
        0.0f, 1.0f,  // Bottom left
        1.0f, 1.0f,  // Bottom right
        1.0f, 0.0f,  // Top right
        0.0f, 0.0f   // Top left
    };
    
    // Apply keystone matrix to vertex positions
    for (int i = 0; i < 4; i++) {
        float x = vertices[i*2];
        float y = vertices[i*2+1];
        
        // Map from GL coordinates (-1,1) to keystone coordinates (0,1)
        float kx = (x + 1.0f) * 0.5f;
        float ky = (y + 1.0f) * 0.5f;
        
        // Apply keystone transformation
        float nx = g_keystone.points[i][0];
        float ny = g_keystone.points[i][1];
        
        // Map back to GL coordinates
        vertices[i*2] = nx * 2.0f - 1.0f;
        vertices[i*2+1] = ny * 2.0f - 1.0f;
    }
    
    // Set up vertex attributes
    glEnableVertexAttribArray(g_keystone_a_position_loc);
    glVertexAttribPointer(g_keystone_a_position_loc, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    
    glEnableVertexAttribArray(g_keystone_a_texcoord_loc);
    glVertexAttribPointer(g_keystone_a_texcoord_loc, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    
    // Draw the quad
    GLuint indices[] = {
        0, 1, 2,  // First triangle
        0, 2, 3   // Second triangle
    };
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, indices);
    
    // Clean up
    glDisableVertexAttribArray(g_keystone_a_position_loc);
    glDisableVertexAttribArray(g_keystone_a_texcoord_loc);
    
    // Draw border and corner markers if enabled
    if (g_keystone.border_visible) {
        draw_keystone_border();
    }
    
    if (g_keystone.corner_markers) {
        draw_keystone_corner_markers();
    }
}