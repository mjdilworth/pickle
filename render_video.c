#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include "egl.h"
#include "shader.h"

// GPU-specific optimizations for Raspberry Pi 4 V3D
#ifdef RPI4_OPTIMIZED
    #define VERTEX_CACHE_OPTIMIZATION 1
    #define TILE_BUFFER_OPTIMIZATION 1
    #define BANDWIDTH_OPTIMIZATION 1
#endif

// Shader cache for performance optimization
typedef struct {
    GLuint program;
    GLint position_attr;
    GLint texcoord_attr;
    GLint texture_uniform;
    bool cached;
} shader_cache_t;

// Render state cache for performance optimization
typedef struct {
    GLuint bound_program;
    GLuint bound_texture;
    GLuint bound_vao;
    GLenum blend_enabled;
    bool initialized;
    bool caching_enabled;  // Can be disabled if errors occur
    int error_count;       // Track consecutive errors
    bool permanently_disabled;  // Once disabled due to errors, never re-enable
} render_state_cache_t;

// Helper macro for checking if caching should be used
#define SHOULD_USE_CACHING() (g_render_state.caching_enabled && !g_render_state.permanently_disabled)

static shader_cache_t g_video_shader_cache = {0};
static render_state_cache_t g_render_state = {.caching_enabled = true};

// OpenGL ES 3.1 vertex shader for texture rendering
static const char *g_video_vertex_shader =
    "#version 310 es\n"
    "in vec2 position;\n"
    "in vec2 texcoord;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n"
    "}\n";

// OpenGL ES 3.1 fragment shader for texture rendering  
static const char *g_video_fragment_shader =
    "#version 310 es\n"
    "precision mediump float;\n"
    "in vec2 v_texcoord;\n"
    "uniform sampler2D tex;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = texture(tex, v_texcoord);\n"
    "}\n";

// Shader program for rendering video frames
static GLuint g_video_shader_program = 0;
static GLint g_video_position_attrib = -1;
static GLint g_video_texcoord_attrib = -1;
static GLint g_video_tex_uniform = -1;

/**
 * Initialize the video rendering shader
 *
 * @return true on success, false on failure
 */
static bool init_video_shaders(void) {
    // Use cached shader if available
    if (g_video_shader_cache.cached && g_video_shader_cache.program != 0) {
        g_video_shader_program = g_video_shader_cache.program;
        g_video_position_attrib = g_video_shader_cache.position_attr;
        g_video_texcoord_attrib = g_video_shader_cache.texcoord_attr;
        g_video_tex_uniform = g_video_shader_cache.texture_uniform;
        return true;
    }
    
    if (g_video_shader_program != 0) {
        return true; // Already initialized
    }
    
    // Create and compile shaders
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, g_video_vertex_shader);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, g_video_fragment_shader);
    
    if (!vertex_shader || !fragment_shader) {
        return false;
    }
    
    // Create shader program
    g_video_shader_program = glCreateProgram();
    if (!g_video_shader_program) {
        return false;
    }
    
    // Attach shaders and link program
    glAttachShader(g_video_shader_program, vertex_shader);
    glAttachShader(g_video_shader_program, fragment_shader);
    glLinkProgram(g_video_shader_program);
    
    // Check link status
    GLint link_status;
    glGetProgramiv(g_video_shader_program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        GLint log_length;
        glGetProgramiv(g_video_shader_program, GL_INFO_LOG_LENGTH, &log_length);
        if (log_length > 0) {
            char *log = malloc((size_t)log_length);
            if (log) {
                glGetProgramInfoLog(g_video_shader_program, log_length, NULL, log);
                fprintf(stderr, "Shader link error: %s\n", log);
                free(log);
            }
        }
        glDeleteProgram(g_video_shader_program);
        g_video_shader_program = 0;
        return false;
    }
    
    // Get attribute and uniform locations
    // Get attribute locations
    g_video_position_attrib = glGetAttribLocation(g_video_shader_program, "position");
    g_video_texcoord_attrib = glGetAttribLocation(g_video_shader_program, "texcoord");
    g_video_tex_uniform = glGetUniformLocation(g_video_shader_program, "tex");
    
    // Debug: Check if attribute locations are valid
    if (g_video_position_attrib == -1) {
        fprintf(stderr, "Failed to get position attribute location\n");
        return false;
    }
    if (g_video_texcoord_attrib == -1) {
        fprintf(stderr, "Failed to get texcoord attribute location\n");
        return false;
    }
    if (g_video_tex_uniform == -1) {
        fprintf(stderr, "Failed to get tex uniform location\n");
        return false;
    }    // Cache shader for future use (performance optimization)
    g_video_shader_cache.program = g_video_shader_program;
    g_video_shader_cache.position_attr = g_video_position_attrib;
    g_video_shader_cache.texcoord_attr = g_video_texcoord_attrib;
    g_video_shader_cache.texture_uniform = g_video_tex_uniform;
    g_video_shader_cache.cached = true;
    
    // Clean up shaders (they're linked to the program now)
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return true;
}

/**
 * Render a video frame to the current framebuffer
 *
 * @param e Pointer to EGL context
 * @param video_texture Video texture to render
 * @param src_rect Source rectangle (normalized 0-1, x, y, w, h)
 * @param dst_rect Destination rectangle (normalized 0-1, x, y, w, h)
 * @return true on success, false on failure
 */
bool render_video_frame(egl_ctx_t *e, GLuint video_texture, 
                        const float src_rect[4], const float dst_rect[4]) {
    (void)e; // Currently unused but available for future optimizations
    
    if (video_texture == 0) {
        return false;
    }
    
    // Initialize shader if not already done
    if (!init_video_shaders()) {
        fprintf(stderr, "Failed to initialize video shader\n");
        return false;
    }
    
    // Use the video shader program (with adaptive state caching)
    if (!SHOULD_USE_CACHING() || !g_render_state.initialized || g_render_state.bound_program != g_video_shader_program) {
        glUseProgram(g_video_shader_program);
        if (SHOULD_USE_CACHING()) {
            g_render_state.bound_program = g_video_shader_program;
        }
    }
    
    // Bind the video texture (with adaptive state caching)
    if (!SHOULD_USE_CACHING() || !g_render_state.initialized || g_render_state.bound_texture != video_texture) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, video_texture);
        if (SHOULD_USE_CACHING()) {
            g_render_state.bound_texture = video_texture;
        }
    }
    glUniform1i(g_video_tex_uniform, 0);
    
    // Calculate vertex positions from destination rectangle
    // dst_rect is [x, y, w, h] in normalized coordinates (0-1)
    float x1 = dst_rect[0] * 2.0f - 1.0f;  // Convert 0-1 to -1 to 1
    float y1 = dst_rect[1] * 2.0f - 1.0f;
    float x2 = (dst_rect[0] + dst_rect[2]) * 2.0f - 1.0f;
    float y2 = (dst_rect[1] + dst_rect[3]) * 2.0f - 1.0f;
    
    // Calculate texture coordinates from source rectangle
    // src_rect is [x, y, w, h] in normalized texture coordinates (0-1)
    float u1 = src_rect[0];
    float v1 = src_rect[1];
    float u2 = src_rect[0] + src_rect[2];
    float v2 = src_rect[1] + src_rect[3];
    
    // Define quad vertices (position + texture coordinates)
    float vertices[] = {
        // positions        // texture coords
        x1, y2,             u1, v2,  // bottom-left
        x2, y2,             u2, v2,  // bottom-right
        x2, y1,             u2, v1,  // top-right
        x1, y1,             u1, v1   // top-left
    };
    
    unsigned int indices[] = {
        0, 1, 2,  // first triangle
        2, 3, 0   // second triangle
    };
    
    // Use simple VBO approach without VAO for better compatibility 
    static GLuint VBO = 0, EBO = 0;
    if (VBO == 0) {
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);
    }
    
    // Bind buffers directly without VAO
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    
    // Use buffer orphaning for better memory bandwidth utilization
    // This prevents GPU stalls by creating a new buffer instead of syncing
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), NULL, GL_STREAM_DRAW);  // Orphan old buffer
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);      // Upload new data
    
    // Upload index data  
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    // Set up vertex attributes (no VAO - direct setup each frame for compatibility)
    glVertexAttribPointer((GLuint)g_video_position_attrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray((GLuint)g_video_position_attrib);
    
    glVertexAttribPointer((GLuint)g_video_texcoord_attrib, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray((GLuint)g_video_texcoord_attrib);
    
    // Enable blending for proper alpha handling
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // No state caching needed with simple approach
    
    // Render the quad
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
#ifdef RPI4_OPTIMIZED
    // On V3D GPU, flush command buffer to prevent stalls
    glFlush();
#endif
    
    // Note: We keep render state for next frame (performance optimization)
    // Only reset state when switching render modes
    
    // Check for OpenGL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error in render_video_frame: 0x%x\n", error);
        
        // Increment error count and disable caching after repeated errors
        g_render_state.error_count++;
        if (g_render_state.error_count >= 3 && !g_render_state.permanently_disabled) {
            g_render_state.caching_enabled = false;
            g_render_state.permanently_disabled = true;
            fprintf(stderr, "Disabling render state caching due to repeated OpenGL errors\n");
        }
        
        // Reset render state cache on error to recover
        g_render_state.initialized = false;
        g_render_state.bound_program = 0;
        g_render_state.bound_texture = 0;
        g_render_state.blend_enabled = GL_FALSE;
        
        // Continue rendering despite errors - don't return false!
    } else {
        // Only reset error count if permanently disabled flag is not set
        if (g_render_state.error_count > 0 && !g_render_state.permanently_disabled) {
            g_render_state.error_count--;
        }
    }
    
    return true;
}