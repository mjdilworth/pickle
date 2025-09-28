#include "gl_optimize.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

// Global GL state cache
gl_state_cache_t g_gl_state = {0};

// Performance mode flags
bool g_performance_mode = false;
bool g_disable_stats_overlay_in_perf = false;
bool g_disable_keystone_in_perf = false;

void gl_state_init(void) {
    g_gl_state.current_program = 0;
    g_gl_state.current_texture = 0;
    g_gl_state.current_blend_src = GL_NONE;
    g_gl_state.current_blend_dst = GL_NONE;
    g_gl_state.blend_enabled = false;
    g_gl_state.depth_test_enabled = false;
    g_gl_state.cull_face_enabled = false;
    g_gl_state.scissor_test_enabled = false;
    g_gl_state.viewport[0] = g_gl_state.viewport[1] = 0;
    g_gl_state.viewport[2] = g_gl_state.viewport[3] = 0;
    g_gl_state.initialized = true;
}

void gl_use_program_cached(GLuint program) {
    if (!g_gl_state.initialized) gl_state_init();
    
    if (g_gl_state.current_program != program) {
        glUseProgram(program);
        g_gl_state.current_program = program;
    }
}

void gl_bind_texture_cached(GLenum target, GLuint texture) {
    if (!g_gl_state.initialized) gl_state_init();
    
    if (g_gl_state.current_texture != texture) {
        glBindTexture(target, texture);
        g_gl_state.current_texture = texture;
    }
}

void gl_enable_blend_cached(GLenum src, GLenum dst) {
    if (!g_gl_state.initialized) gl_state_init();
    
    if (!g_gl_state.blend_enabled) {
        glEnable(GL_BLEND);
        g_gl_state.blend_enabled = true;
    }
    
    if (g_gl_state.current_blend_src != src || g_gl_state.current_blend_dst != dst) {
        glBlendFunc(src, dst);
        g_gl_state.current_blend_src = src;
        g_gl_state.current_blend_dst = dst;
    }
}

void gl_disable_blend_cached(void) {
    if (!g_gl_state.initialized) gl_state_init();
    
    if (g_gl_state.blend_enabled) {
        glDisable(GL_BLEND);
        g_gl_state.blend_enabled = false;
    }
}

void gl_viewport_cached(int x, int y, int width, int height) {
    if (!g_gl_state.initialized) gl_state_init();
    
    if (g_gl_state.viewport[0] != x || g_gl_state.viewport[1] != y ||
        g_gl_state.viewport[2] != width || g_gl_state.viewport[3] != height) {
        glViewport(x, y, width, height);
        g_gl_state.viewport[0] = x;
        g_gl_state.viewport[1] = y;
        g_gl_state.viewport[2] = width;
        g_gl_state.viewport[3] = height;
    }
}

void gl_reset_state_cache(void) {
    // Force reset of cached state to handle external GL state changes
    g_gl_state.initialized = false;
    gl_state_init();
}

void gl_optimize_for_performance(void) {
    g_performance_mode = true;
    
    // Configure performance-specific optimizations
    g_disable_stats_overlay_in_perf = true;
    g_disable_keystone_in_perf = false; // Keep keystone but optimize it
    
    // Disable expensive GL features
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DITHER);
    
    // Optimize for 2D rendering
    glHint(GL_GENERATE_MIPMAP_HINT, GL_FASTEST);
    
    LOG_INFO("GL optimizations enabled for performance mode");
}

bool should_skip_feature_for_performance(const char* feature_name) {
    if (!g_performance_mode) return false;
    
    // Skip expensive features in performance mode, but respect user's explicit requests
    if (g_disable_stats_overlay_in_perf && 
        (strstr(feature_name, "stats") || strstr(feature_name, "overlay"))) {
        // Don't skip stats overlay if user has explicitly enabled it
        // This allows users to see performance stats even in performance mode
        extern int g_show_stats_overlay;
        if (strstr(feature_name, "stats") && g_show_stats_overlay) {
            return false; // User explicitly wants to see stats
        }
        return true;
    }
    
    return false;
}