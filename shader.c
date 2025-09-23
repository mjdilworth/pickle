#include "shader.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

// Shader source code for keystone correction
const char* g_vertex_shader_src = 
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texCoord;\n"
    "varying vec2 v_texCoord;\n"
    "void main() {\n"
    "    // Position is already in clip space coordinates (-1 to 1)\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    \n"
    "    // Use the provided texture coordinates directly\n"
    "    v_texCoord = a_texCoord;\n"
    "}\n";

const char* g_fragment_shader_src = 
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    vec4 color = texture2D(u_texture, v_texCoord);\n"
    "    // We always want to see the video content, regardless of alpha\n"
    "    gl_FragColor = vec4(color.rgb, 1.0);\n"
    "}\n";

// Border shader: positions only, uniform color
const char* g_border_vs_src =
    "attribute vec2 a_position;\n"
    "void main(){\n"
    "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

const char* g_border_fs_src =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() {\n"
    "    gl_FragColor = u_color;\n"
    "}\n";

GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        LOG_ERROR("Failed to create shader");
        return 0;
    }
    
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info_log = malloc((size_t)info_len);
            glGetShaderInfoLog(shader, info_len, NULL, info_log);
            LOG_ERROR("Shader compilation failed: %s", info_log);
            free(info_log);
        }
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

GLuint link_program(GLuint vertex_shader, GLuint fragment_shader) {
    GLuint program = glCreateProgram();
    if (!program) {
        LOG_ERROR("Failed to create program");
        return 0;
    }
    
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char *info_log = malloc((size_t)info_len);
            glGetProgramInfoLog(program, info_len, NULL, info_log);
            LOG_ERROR("Program linking failed: %s", info_log);
            free(info_log);
        }
        glDeleteProgram(program);
        return 0;
    }
    
    return program;
}

void cleanup_shader_resources(GLuint program, GLuint vertex_shader, GLuint fragment_shader) {
    // Delete program if it exists
    if (program) {
        // Detach shaders before deleting program
        if (vertex_shader) {
            glDetachShader(program, vertex_shader);
        }
        if (fragment_shader) {
            glDetachShader(program, fragment_shader);
        }
        glDeleteProgram(program);
    }
    
    // Delete shaders if they exist
    if (vertex_shader) {
        glDeleteShader(vertex_shader);
    }
    if (fragment_shader) {
        glDeleteShader(fragment_shader);
    }
}

// Function to get a basic shader program for rendering textures
GLuint get_basic_shader_program(void) {
    static GLuint program = 0;
    
    // Create the program only once
    if (program == 0) {
        GLuint vert_shader = compile_shader(GL_VERTEX_SHADER, g_vertex_shader_src);
        GLuint frag_shader = compile_shader(GL_FRAGMENT_SHADER, g_fragment_shader_src);
        
        if (!vert_shader || !frag_shader) {
            LOG_ERROR("Failed to compile shaders for basic program");
            return 0;
        }
        
        program = link_program(vert_shader, frag_shader);
        if (!program) {
            LOG_ERROR("Failed to link basic shader program");
            return 0;
        }
        
        // Shaders can be deleted after linking
        glDeleteShader(vert_shader);
        glDeleteShader(frag_shader);
    }
    
    return program;
}

// Draw corner markers for the keystone correction UI
void draw_keystone_corner_markers(float corners[8], int selected_corner) {
    GLuint border_prog = 0;
    GLuint vert_shader = compile_shader(GL_VERTEX_SHADER, g_border_vs_src);
    GLuint frag_shader = compile_shader(GL_FRAGMENT_SHADER, g_border_fs_src);
    
    if (!vert_shader || !frag_shader) {
        LOG_ERROR("Failed to compile shaders for keystone border");
        return;
    }
    
    border_prog = link_program(vert_shader, frag_shader);
    if (!border_prog) {
        LOG_ERROR("Failed to link keystone border shader program");
        return;
    }
    
    // Clean up shaders after linking
    glDeleteShader(vert_shader);
    glDeleteShader(frag_shader);
    
    glUseProgram(border_prog);
    
    // Draw each corner as a small marker
    for (int i = 0; i < 4; i++) {
        // Size of the marker square (in clip space)
        const float marker_size = 0.03f;
        
        // Get corner coordinates from array (x,y pairs)
        float x = corners[i*2];
        float y = corners[i*2+1];
        
        // Set color based on whether this corner is selected
        GLint color_loc = glGetUniformLocation(border_prog, "u_color");
        if (i == selected_corner) {
            // Selected corner: bright yellow
            glUniform4f(color_loc, 1.0f, 1.0f, 0.0f, 1.0f);
        } else {
            // Normal corner: white
            glUniform4f(color_loc, 1.0f, 1.0f, 1.0f, 0.8f);
        }
        
        // Define vertices for a small square at the corner
        float marker_vertices[] = {
            x - marker_size, y - marker_size,
            x + marker_size, y - marker_size,
            x + marker_size, y + marker_size,
            x - marker_size, y + marker_size
        };
        
        // Draw the marker
        GLint pos_attrib = glGetAttribLocation(border_prog, "a_position");
        glEnableVertexAttribArray((GLuint)pos_attrib);
        glVertexAttribPointer((GLuint)pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, marker_vertices);
        glDrawArrays(GL_LINE_LOOP, 0, 4);
        glDisableVertexAttribArray((GLuint)pos_attrib);
    }
    
    glDeleteProgram(border_prog);
}

// Draw a border around the keystone quadrilateral
void draw_keystone_border(float corners[8]) {
    GLuint border_prog = 0;
    GLuint vert_shader = compile_shader(GL_VERTEX_SHADER, g_border_vs_src);
    GLuint frag_shader = compile_shader(GL_FRAGMENT_SHADER, g_border_fs_src);
    
    if (!vert_shader || !frag_shader) {
        LOG_ERROR("Failed to compile shaders for keystone border");
        return;
    }
    
    border_prog = link_program(vert_shader, frag_shader);
    if (!border_prog) {
        LOG_ERROR("Failed to link keystone border shader program");
        return;
    }
    
    // Clean up shaders after linking
    glDeleteShader(vert_shader);
    glDeleteShader(frag_shader);
    
    glUseProgram(border_prog);
    
    // Set color for the border (white)
    GLint color_loc = glGetUniformLocation(border_prog, "u_color");
    glUniform4f(color_loc, 1.0f, 1.0f, 1.0f, 0.6f);
    
    // Connect corners to form a quadrilateral
    float border_vertices[] = {
        corners[0], corners[1],  // Top-left
        corners[2], corners[3],  // Top-right
        corners[4], corners[5],  // Bottom-right
        corners[6], corners[7]   // Bottom-left
    };
    
    // Draw the border as a line loop
    GLint pos_attrib = glGetAttribLocation(border_prog, "a_position");
    glEnableVertexAttribArray((GLuint)pos_attrib);
    glVertexAttribPointer((GLuint)pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, border_vertices);
    glDrawArrays(GL_LINE_LOOP, 0, 4);
    glDisableVertexAttribArray((GLuint)pos_attrib);
    
    glDeleteProgram(border_prog);
}