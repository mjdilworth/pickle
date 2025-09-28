#ifndef GL_OPTIMIZE_H
#define GL_OPTIMIZE_H

#include <GLES2/gl2.h>
#include <stdbool.h>

// GL state cache to avoid redundant state changes
typedef struct {
    GLuint current_program;
    GLuint current_texture;
    GLenum current_blend_src;
    GLenum current_blend_dst;
    bool blend_enabled;
    bool depth_test_enabled;
    bool cull_face_enabled;
    bool scissor_test_enabled;
    int viewport[4];
    bool initialized;
} gl_state_cache_t;

// Global GL state cache
extern gl_state_cache_t g_gl_state;

// Optimized GL state management functions
void gl_state_init(void);
void gl_use_program_cached(GLuint program);
void gl_bind_texture_cached(GLenum target, GLuint texture);
void gl_enable_blend_cached(GLenum src, GLenum dst);
void gl_disable_blend_cached(void);
void gl_viewport_cached(int x, int y, int width, int height);
void gl_reset_state_cache(void);

// Performance mode flags
extern bool g_performance_mode;
extern bool g_disable_stats_overlay_in_perf;
extern bool g_disable_keystone_in_perf;

// High-performance rendering functions
void gl_optimize_for_performance(void);
bool should_skip_feature_for_performance(const char* feature_name);

#endif // GL_OPTIMIZE_H