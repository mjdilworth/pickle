#include "pickle_globals.h"
#include "mpv.h"
#include "drm.h"
#include "egl.h"
#include "keystone.h"  // For keystone integration
#include <stdio.h>
#include <GLES3/gl31.h>

// Video texture for MPV rendering
static GLuint g_mpv_texture = 0;
static GLuint g_mpv_fbo = 0;
static int g_mpv_texture_width = 0;
static int g_mpv_texture_height = 0;

/**
 * Create or resize MPV render texture
 */
static bool ensure_mpv_texture(int width, int height) {
    if (g_mpv_texture != 0 && g_mpv_texture_width == width && g_mpv_texture_height == height) {
        return true; // Already correct size
    }
    
    // Clean up existing resources
    if (g_mpv_fbo != 0) {
        glDeleteFramebuffers(1, &g_mpv_fbo);
        g_mpv_fbo = 0;
    }
    if (g_mpv_texture != 0) {
        glDeleteTextures(1, &g_mpv_texture);
        g_mpv_texture = 0;
    }
    
    // Create texture with optimized parameters
    glGenTextures(1, &g_mpv_texture);
    glBindTexture(GL_TEXTURE_2D, g_mpv_texture);
    
    // Use GL_RGBA8 internal format for better performance on V3D GPU
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    
    // Set texture parameters (optimized for video)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Enable automatic mipmap generation for better quality at distance
    // glGenerateMipmap(GL_TEXTURE_2D); // Commented out as video typically fills screen
    
    // Create framebuffer
    glGenFramebuffers(1, &g_mpv_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_mpv_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_mpv_texture, 0);
    
    // Check framebuffer completeness
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "MPV framebuffer not complete\n");
        return false;
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    g_mpv_texture_width = width;
    g_mpv_texture_height = height;
    
    return true;
}

// Simple wrapper for MPV rendering (used by pickle_events.c)
bool render_frame_mpv(mpv_handle *mpv, mpv_render_context *mpv_gl, kms_ctx_t *drm, egl_ctx_t *eglc) {
    if (!mpv || !mpv_gl || !drm || !eglc) {
        return false;
    }
    
    // Get video dimensions (use display size for now)
    int width = (int)drm->mode.hdisplay;
    int height = (int)drm->mode.vdisplay;
    
    // Ensure we have a texture to render to
    if (!ensure_mpv_texture(width, height)) {
        return false;
    }
    
    // Set up MPV rendering parameters
    mpv_opengl_fbo mpv_fbo = {
        .fbo = (int)g_mpv_fbo,
        .w = g_mpv_texture_width,
        .h = g_mpv_texture_height,
        .internal_format = 0
    };
    
    int flip_y = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {0}
    };
    
    // Clear the FBO
    glBindFramebuffer(GL_FRAMEBUFFER, g_mpv_fbo);
    glViewport(0, 0, g_mpv_texture_width, g_mpv_texture_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Render MPV frame to texture
    int result = mpv_render_context_render(mpv_gl, params);
    
    // Switch back to default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    
    if (result >= 0) {
        // Check if keystone correction is enabled
        if (g_keystone.enabled) {
            // When keystone is enabled, provide the MPV texture to the keystone pipeline
            // instead of rendering directly to screen
            g_keystone_fbo_texture = g_mpv_texture;
            
            // The keystone rendering will happen later in the main render loop
            // Just clear the screen here and let keystone handle the transformed rendering
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            
            return true; // Success - keystone pipeline will handle the actual rendering
        } else {
            // No keystone - render directly to screen using our video pipeline
            float src_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // Full texture
            float dst_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // Full screen
            
            // Clear the screen
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            
            // Render the video texture directly
            return render_video_frame(eglc, g_mpv_texture, src_rect, dst_rect);
        }
    }
    
    return false;
}