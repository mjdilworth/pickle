#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include "egl.h"
#include "shader.h"

// Basic vertex shader for texture rendering
static const char *g_video_vertex_shader =
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n"
    "}\n";

// Basic fragment shader for texture rendering
static const char *g_video_fragment_shader =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(tex, v_texcoord);\n"
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
static bool init_video_shader(void) __attribute__((unused));

static bool init_video_shader(void) {
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
    g_video_position_attrib = glGetAttribLocation(g_video_shader_program, "position");
    g_video_texcoord_attrib = glGetAttribLocation(g_video_shader_program, "texcoord");
    g_video_tex_uniform = glGetUniformLocation(g_video_shader_program, "tex");
    
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
    // This is a simplified placeholder implementation
    (void)e; // Unused parameter
    (void)video_texture; // Unused parameter
    (void)src_rect; // Unused parameter
    (void)dst_rect; // Unused parameter
    
    // In a real implementation, this would render the video texture to the current framebuffer
    // using the source and destination rectangles
    
    return true;
}