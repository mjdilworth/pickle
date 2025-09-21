#include "keystone.h"
#include "utils.h"
#include "shader.h"
#include "hvs_keystone.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <GLES2/gl2.h>

// Global keystone state
keystone_t g_keystone = {
    .points = {
        {0.0f, 0.0f},  // Top-left
        {1.0f, 0.0f},  // Top-right
        {0.0f, 1.0f},  // Bottom-left
        {1.0f, 1.0f}   // Bottom-right
    },
    .selected_corner = -1,
    .enabled = false,
    .matrix = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    },
    .mesh_enabled = false,
    .mesh_size = 4,    // 4x4 mesh by default
    .mesh_points = NULL,
    .active_mesh_point = {-1, -1},
    .initialized = false,
    .border_visible = false,
    .border_width = 5,
    .corner_markers = true,
    .perspective_pins = {false, false, false, false},
    .active_corner = -1
};

// Keystone adjustment step size (in 1/1000 units)
int g_keystone_adjust_step = 1;

// UI visibility flags
int g_show_border = 0;
int g_border_width = 5;
int g_show_corner_markers = 1;

// OpenGL shader resources
GLuint g_keystone_shader_program = 0;
GLuint g_keystone_vertex_shader = 0;
GLuint g_keystone_fragment_shader = 0;
GLuint g_keystone_vertex_buffer = 0;
GLuint g_keystone_texcoord_buffer = 0;
GLuint g_keystone_index_buffer = 0;
GLuint g_keystone_fbo = 0;
GLuint g_keystone_fbo_texture = 0;
int g_keystone_fbo_w = 0;
int g_keystone_fbo_h = 0;
GLint g_keystone_a_position_loc = -1;
GLint g_keystone_a_texcoord_loc = -1;
GLint g_keystone_u_texture_loc = -1;

// Border shader resources
static GLuint g_border_shader_program = 0;
static GLuint g_border_vertex_shader = 0;
static GLuint g_border_fragment_shader = 0;
static GLint g_border_a_position_loc = -1;
static GLint g_border_u_color_loc = -1;

// Accessor functions for the adapter layer
keystone_t *get_keystone_data(void) {
    return &g_keystone;
}

int *get_keystone_adjust_step(void) {
    return &g_keystone_adjust_step;
}

bool *get_keystone_border_visible_ptr(void) {
    return &g_keystone.border_visible;
}

int *get_keystone_border_width_ptr(void) {
    return &g_keystone.border_width;
}

bool *get_keystone_corner_markers_ptr(void) {
    return &g_keystone.corner_markers;
}

GLuint *get_keystone_shader_program_ptr(void) {
    return &g_keystone_shader_program;
}

GLuint *get_keystone_vertex_shader_ptr(void) {
    return &g_keystone_vertex_shader;
}

GLuint *get_keystone_fragment_shader_ptr(void) {
    return &g_keystone_fragment_shader;
}

GLuint *get_keystone_vertex_buffer_ptr(void) {
    return &g_keystone_vertex_buffer;
}

GLuint *get_keystone_texcoord_buffer_ptr(void) {
    return &g_keystone_texcoord_buffer;
}

GLuint *get_keystone_index_buffer_ptr(void) {
    return &g_keystone_index_buffer;
}

GLuint *get_keystone_fbo_ptr(void) {
    return &g_keystone_fbo;
}

GLuint *get_keystone_fbo_texture_ptr(void) {
    return &g_keystone_fbo_texture;
}

int *get_keystone_fbo_w_ptr(void) {
    return &g_keystone_fbo_w;
}

int *get_keystone_fbo_h_ptr(void) {
    return &g_keystone_fbo_h;
}

GLint *get_keystone_a_position_loc_ptr(void) {
    return &g_keystone_a_position_loc;
}

GLint *get_keystone_a_texcoord_loc_ptr(void) {
    return &g_keystone_a_texcoord_loc;
}

GLint *get_keystone_u_texture_loc_ptr(void) {
    return &g_keystone_u_texture_loc;
}

bool *get_keystone_mesh_enabled_ptr(void) {
    return &g_keystone.mesh_enabled;
}

bool (*get_keystone_perspective_pins_ptr(void))[4] {
    return &g_keystone.perspective_pins;
}

int *get_keystone_active_corner_ptr(void) {
    return &g_keystone.active_corner;
}

int (*get_keystone_active_mesh_point_ptr(void))[2] {
    return &g_keystone.active_mesh_point;
}

float (*get_keystone_matrix_ptr(void))[16] {
    return &g_keystone.matrix;
}

void keystone_init(void) {
    // Initialize with default values (rectangle at full screen)
    g_keystone.points[0][0] = 0.0f; g_keystone.points[0][1] = 0.0f; // Top-left
    g_keystone.points[1][0] = 1.0f; g_keystone.points[1][1] = 0.0f; // Top-right
    g_keystone.points[2][0] = 0.0f; g_keystone.points[2][1] = 1.0f; // Bottom-left
    g_keystone.points[3][0] = 1.0f; g_keystone.points[3][1] = 1.0f; // Bottom-right
    
    g_keystone.selected_corner = -1;
    g_keystone.enabled = false;
    g_keystone.mesh_enabled = false;
    g_keystone.mesh_size = 4;
    g_keystone.mesh_points = NULL;
    g_keystone.active_mesh_point[0] = -1;
    g_keystone.active_mesh_point[1] = -1;
    g_keystone.border_visible = false;
    g_keystone.border_width = 5;
    g_keystone.corner_markers = true;
    g_keystone.active_corner = -1;
    
    for (int i = 0; i < 4; i++) {
        g_keystone.perspective_pins[i] = false;
    }
    
    // Initialize identity matrix
    for (int i = 0; i < 16; i++) {
        g_keystone.matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    
    g_keystone.initialized = true;
    
    // Check environment variable for adjustment step size
    const char* step_str = getenv("PICKLE_KEYSTONE_STEP");
    if (step_str) {
        int step = atoi(step_str);
        if (step >= 1 && step <= 100) {
            g_keystone_adjust_step = step;
        }
    }
    
    // Allocate mesh points if needed
    if (g_keystone.mesh_points == NULL) {
        g_keystone.mesh_points = malloc((size_t)g_keystone.mesh_size * sizeof(float*));
        if (g_keystone.mesh_points) {
            for (int i = 0; i < g_keystone.mesh_size; i++) {
                g_keystone.mesh_points[i] = malloc((size_t)g_keystone.mesh_size * 2 * sizeof(float));
                if (g_keystone.mesh_points[i]) {
                    // Initialize with default grid positions
                    for (int j = 0; j < g_keystone.mesh_size; j++) {
                        float x = (float)j / (float)(g_keystone.mesh_size - 1);
                        float y = (float)i / (float)(g_keystone.mesh_size - 1);
                        g_keystone.mesh_points[i][j*2] = x;
                        g_keystone.mesh_points[i][j*2+1] = y;
                    }
                }
            }
        }
    }
}

void keystone_cleanup(void) {
    cleanup_keystone_shader();
    cleanup_keystone_fbo();
    cleanup_mesh_resources();
    cleanup_keystone_resources();
}

bool init_border_shader(void) {
    g_border_vertex_shader = compile_shader(GL_VERTEX_SHADER, g_border_vs_src);
    if (!g_border_vertex_shader) return false;
    g_border_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, g_border_fs_src);
    if (!g_border_fragment_shader) { glDeleteShader(g_border_vertex_shader); g_border_vertex_shader = 0; return false; }
    g_border_shader_program = glCreateProgram();
    if (!g_border_shader_program) { glDeleteShader(g_border_vertex_shader); glDeleteShader(g_border_fragment_shader); return false; }
    glAttachShader(g_border_shader_program, g_border_vertex_shader);
    glAttachShader(g_border_shader_program, g_border_fragment_shader);
    glLinkProgram(g_border_shader_program);
    GLint linked = 0; glGetProgramiv(g_border_shader_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len=0; glGetProgramiv(g_border_shader_program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len>1) { char* buf = malloc((size_t)info_len); glGetProgramInfoLog(g_border_shader_program, info_len, NULL, buf); LOG_ERROR("Border shader link: %s", buf); free(buf);}        
        glDeleteProgram(g_border_shader_program); g_border_shader_program=0;
        glDeleteShader(g_border_vertex_shader); g_border_vertex_shader=0;
        glDeleteShader(g_border_fragment_shader); g_border_fragment_shader=0;
        return false;
    }
    g_border_a_position_loc = glGetAttribLocation(g_border_shader_program, "a_position");
    g_border_u_color_loc = glGetUniformLocation(g_border_shader_program, "u_color");
    return true;
}

bool init_keystone_shader(void) {
    // Compile the shaders
    g_keystone_vertex_shader = compile_shader(GL_VERTEX_SHADER, g_vertex_shader_src);
    if (!g_keystone_vertex_shader) {
        LOG_ERROR("Failed to compile keystone vertex shader");
        return false;
    }
    
    g_keystone_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, g_fragment_shader_src);
    if (!g_keystone_fragment_shader) {
        LOG_ERROR("Failed to compile keystone fragment shader");
        glDeleteShader(g_keystone_vertex_shader);
        g_keystone_vertex_shader = 0;
        return false;
    }
    
    // Create the shader program
    g_keystone_shader_program = glCreateProgram();
    if (!g_keystone_shader_program) {
        LOG_ERROR("Failed to create keystone shader program");
        glDeleteShader(g_keystone_vertex_shader);
        glDeleteShader(g_keystone_fragment_shader);
        g_keystone_vertex_shader = 0;
        g_keystone_fragment_shader = 0;
        return false;
    }
    
    // Attach the shaders to the program
    glAttachShader(g_keystone_shader_program, g_keystone_vertex_shader);
    glAttachShader(g_keystone_shader_program, g_keystone_fragment_shader);
    
    // Link the program
    glLinkProgram(g_keystone_shader_program);
    
    // Check the link status
    GLint linked;
    glGetProgramiv(g_keystone_shader_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(g_keystone_shader_program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char* info_log = malloc(sizeof(char) * (size_t)info_len);
            glGetProgramInfoLog(g_keystone_shader_program, info_len, NULL, info_log);
            LOG_ERROR("Error linking keystone shader program: %s", info_log);
            free(info_log);
        }
        glDeleteProgram(g_keystone_shader_program);
        glDeleteShader(g_keystone_vertex_shader);
        glDeleteShader(g_keystone_fragment_shader);
        g_keystone_shader_program = 0;
        g_keystone_vertex_shader = 0;
        g_keystone_fragment_shader = 0;
        return false;
    }
    
    // Get the attribute and uniform locations
    g_keystone_a_position_loc = glGetAttribLocation(g_keystone_shader_program, "a_position");
    g_keystone_a_texcoord_loc = glGetAttribLocation(g_keystone_shader_program, "a_texCoord");
    g_keystone_u_texture_loc = glGetUniformLocation(g_keystone_shader_program, "u_texture");
    
    return true;
}

void cleanup_keystone_shader(void) {
    if (g_keystone_vertex_buffer) {
        glDeleteBuffers(1, &g_keystone_vertex_buffer);
        g_keystone_vertex_buffer = 0;
    }
    
    if (g_keystone_texcoord_buffer) {
        glDeleteBuffers(1, &g_keystone_texcoord_buffer);
        g_keystone_texcoord_buffer = 0;
    }
    
    if (g_keystone_shader_program) {
        glDeleteProgram(g_keystone_shader_program);
        g_keystone_shader_program = 0;
    }
    
    if (g_keystone_vertex_shader) {
        glDeleteShader(g_keystone_vertex_shader);
        g_keystone_vertex_shader = 0;
    }
    
    if (g_keystone_fragment_shader) {
        glDeleteShader(g_keystone_fragment_shader);
        g_keystone_fragment_shader = 0;
    }
}

void cleanup_keystone_fbo(void) {
    if (g_keystone_fbo_texture) {
        glDeleteTextures(1, &g_keystone_fbo_texture);
        g_keystone_fbo_texture = 0;
    }
    
    if (g_keystone_fbo) {
        glDeleteFramebuffers(1, &g_keystone_fbo);
        g_keystone_fbo = 0;
    }
    
    g_keystone_fbo_w = 0;
    g_keystone_fbo_h = 0;
}

void cleanup_mesh_resources(void) {
    if (g_keystone.mesh_points) {
        for (int i = 0; i < g_keystone.mesh_size; i++) {
            if (g_keystone.mesh_points[i]) {
                free(g_keystone.mesh_points[i]);
            }
        }
        free(g_keystone.mesh_points);
        g_keystone.mesh_points = NULL;
    }
}

void cleanup_keystone_resources(void) {
    // Clean up mesh resources
    cleanup_mesh_resources();
    
    // Clean up OpenGL resources
    if (g_keystone_vertex_buffer) {
        glDeleteBuffers(1, &g_keystone_vertex_buffer);
        g_keystone_vertex_buffer = 0;
    }
    
    if (g_keystone_shader_program) {
        glDeleteProgram(g_keystone_shader_program);
        g_keystone_shader_program = 0;
    }
    
    if (g_keystone_vertex_shader) {
        glDeleteShader(g_keystone_vertex_shader);
        g_keystone_vertex_shader = 0;
    }
    
    if (g_keystone_fragment_shader) {
        glDeleteShader(g_keystone_fragment_shader);
        g_keystone_fragment_shader = 0;
    }
    
    // Reset keystone state
    g_keystone.initialized = false;
}

void keystone_adjust_corner(int corner, float x_delta, float y_delta) {
    if (corner < 0 || corner > 3) return;
    
    // Apply adjustment scaled by step size
    float scale = (float)g_keystone_adjust_step / 1000.0f;
    g_keystone.points[corner][0] += x_delta * scale;
    g_keystone.points[corner][1] += y_delta * scale;
    
    // Update the transformation matrix
    keystone_update_matrix();
    
    LOG_INFO("Adjusted corner %d to (%.3f, %.3f)", 
              corner + 1, g_keystone.points[corner][0], g_keystone.points[corner][1]);
}

void keystone_update_matrix(void) {
    // This is a placeholder for the matrix update function
    // In the real implementation, this would update any matrices needed for rendering
    
    // Update HVS keystone if enabled and supported
    if (g_keystone.enabled && hvs_keystone_is_supported()) {
        hvs_keystone_update(&g_keystone);
    }
}

bool keystone_save_config(const char* path) {
    if (!path) return false;
    
    FILE* f = fopen(path, "w");
    if (!f) {
        LOG_ERROR("Failed to open file for writing: %s (%s)", path, strerror(errno));
        return false;
    }
    
    fprintf(f, "# Pickle keystone configuration\n");
    fprintf(f, "enabled=%d\n", g_keystone.enabled ? 1 : 0);
    fprintf(f, "mesh_size=%d\n", g_keystone.mesh_size);
    
    // Save the corner points
    for (int i = 0; i < 4; i++) {
        fprintf(f, "corner%d=%.6f,%.6f\n", i+1, g_keystone.points[i][0], g_keystone.points[i][1]);
    }
    
    // Save border and corner marker settings
    fprintf(f, "border=%d\n", g_keystone.border_visible ? 1 : 0);
    fprintf(f, "cornermarks=%d\n", g_keystone.corner_markers ? 1 : 0);
    fprintf(f, "border_width=%d\n", g_keystone.border_width);
    
    // Save mesh points if mesh warping is enabled
    if (g_keystone.mesh_points) {
        for (int i = 0; i < g_keystone.mesh_size; i++) {
            for (int j = 0; j < g_keystone.mesh_size; j++) {
                if (g_keystone.mesh_points[i]) {
                    fprintf(f, "mesh_%d_%d=%.6f,%.6f\n", i, j, 
                            g_keystone.mesh_points[i][j*2],
                            g_keystone.mesh_points[i][j*2+1]);
                }
            }
        }
    }
    
    fclose(f);
    LOG_INFO("Keystone configuration saved to %s", path);
    
    return true;
}

bool keystone_load_config(const char* path) {
    if (!path) return false;
    
    FILE* f = fopen(path, "r");
    if (!f) {
        LOG_INFO("No keystone config found at %s", path);
        return false;
    }
    
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;
        
        if (strncmp(line, "enabled=", 8) == 0) {
            g_keystone.enabled = (atoi(line + 8) != 0);
        }
        else if (strncmp(line, "mesh_size=", 10) == 0) {
            int new_size = atoi(line + 10);
            if (new_size >= 2 && new_size <= 10) { // Sanity check
                // Only change if different (requires reallocation)
                if (new_size != g_keystone.mesh_size) {
                    // Clean up old mesh if it exists
                    cleanup_mesh_resources();
                    
                    // Set new size and allocate
                    g_keystone.mesh_size = new_size;
                    g_keystone.mesh_points = malloc((size_t)new_size * sizeof(float*));
                    if (g_keystone.mesh_points) {
                        for (int i = 0; i < new_size; i++) {
                            g_keystone.mesh_points[i] = malloc((size_t)new_size * 2 * sizeof(float));
                            if (g_keystone.mesh_points[i]) {
                                // Initialize with default grid positions
                                for (int j = 0; j < new_size; j++) {
                                    float x = (float)j / (float)(new_size - 1);
                                    float y = (float)i / (float)(new_size - 1);
                                    g_keystone.mesh_points[i][j*2] = x;
                                    g_keystone.mesh_points[i][j*2+1] = y;
                                }
                            }
                        }
                    }
                }
            }
        }
        else if (strncmp(line, "corner1=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &g_keystone.points[0][0], &g_keystone.points[0][1]);
        }
        else if (strncmp(line, "corner2=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &g_keystone.points[1][0], &g_keystone.points[1][1]);
        }
        else if (strncmp(line, "corner3=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &g_keystone.points[2][0], &g_keystone.points[2][1]);
        }
        else if (strncmp(line, "corner4=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &g_keystone.points[3][0], &g_keystone.points[3][1]);
        }
        else if (strncmp(line, "border=", 7) == 0) {
            g_keystone.border_visible = (atoi(line + 7) != 0);
        }
        else if (strncmp(line, "cornermarks=", 12) == 0) {
            g_keystone.corner_markers = (atoi(line + 12) != 0);
        }
        else if (strncmp(line, "border_width=", 13) == 0) {
            int width = atoi(line + 13);
            if (width >= 1 && width <= 50) { // Sanity check
                g_keystone.border_width = width;
            }
        }
        else if (strncmp(line, "mesh_", 5) == 0) {
            // Parse mesh point coordinates: mesh_i_j=x,y
            int i, j;
            float x, y;
            if (sscanf(line + 5, "%d_%d=%f,%f", &i, &j, &x, &y) == 4) {
                if (i >= 0 && i < g_keystone.mesh_size && 
                    j >= 0 && j < g_keystone.mesh_size &&
                    g_keystone.mesh_points && g_keystone.mesh_points[i]) {
                    g_keystone.mesh_points[i][j*2] = x;
                    g_keystone.mesh_points[i][j*2+1] = y;
                }
            }
        }
    }
    fclose(f);
    
    // Update matrix based on loaded settings
    if (g_keystone.enabled) {
        keystone_update_matrix();
    }
    
    LOG_INFO("Loaded keystone configuration from %s", path);
    return true;
}

// Toggle functions
void keystone_toggle_enabled(void) {
    g_keystone.enabled = !g_keystone.enabled;
    
    // If keystone is enabled and HVS is supported, apply HVS keystone
    if (g_keystone.enabled && hvs_keystone_is_supported()) {
        if (!hvs_keystone_update(&g_keystone)) {
            LOG_WARN("Failed to apply HVS keystone, falling back to software implementation");
        } else {
            LOG_INFO("Applied HVS keystone transformation");
        }
    } else if (!g_keystone.enabled && hvs_keystone_is_supported()) {
        // Clean up HVS keystone when disabled
        hvs_keystone_cleanup();
    }
    
    LOG_INFO("Keystone %s", g_keystone.enabled ? "enabled" : "disabled");
}

void keystone_toggle_border(void) {
    g_keystone.border_visible = !g_keystone.border_visible;
    LOG_INFO("Border %s", g_keystone.border_visible ? "visible" : "hidden");
}

void keystone_toggle_corner_markers(void) {
    g_keystone.corner_markers = !g_keystone.corner_markers;
    LOG_INFO("Corner markers %s", g_keystone.corner_markers ? "visible" : "hidden");
}

void keystone_reset(void) {
    g_keystone.points[0][0] = 0.0f; g_keystone.points[0][1] = 0.0f; // Top-left
    g_keystone.points[1][0] = 1.0f; g_keystone.points[1][1] = 0.0f; // Top-right
    g_keystone.points[2][0] = 0.0f; g_keystone.points[2][1] = 1.0f; // Bottom-left
    g_keystone.points[3][0] = 1.0f; g_keystone.points[3][1] = 1.0f; // Bottom-right
    
    keystone_update_matrix();
    
    // Update HVS keystone if active
    if (g_keystone.enabled && hvs_keystone_is_supported()) {
        hvs_keystone_update(&g_keystone);
    }
    
    LOG_INFO("Keystone reset to default");
}

// Border functions
void keystone_adjust_border_width(int delta) {
    g_keystone.border_width += delta;
    
    // Clamp to reasonable range
    if (g_keystone.border_width < 1) g_keystone.border_width = 1;
    if (g_keystone.border_width > 50) g_keystone.border_width = 50;
    
    LOG_INFO("Border width: %d", g_keystone.border_width);
}

// Accessor functions
bool is_keystone_enabled(void) {
    return g_keystone.enabled;
}

bool is_keystone_border_visible(void) {
    return g_keystone.border_visible;
}

bool is_keystone_corner_markers_visible(void) {
    return g_keystone.corner_markers;
}

int get_keystone_border_width(void) {
    return g_keystone.border_width;
}

int get_keystone_selected_corner(void) {
    return g_keystone.selected_corner;
}

// Simple key handler to be called from the main program
bool keystone_handle_key(char key) {
    // This is a placeholder for the key handler function
    // In the real implementation, this would handle keystone-related key events
    return false; // Return true if the key was handled
}