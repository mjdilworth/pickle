#include "keystone.h"
#include "utils.h"
#include "shader.h"

#include "drm_keystone.h"  // New DRM/KMS implementation
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <GLES3/gl31.h>
#include <linux/limits.h>  // For PATH_MAX

// Global state for keystone correction
keystone_t g_keystone = {0};

// Keystone adjustment step size (in 1/100 units - increased from 1/1000)
int g_keystone_adjust_step = 10;  // Increased from 1 to 10

// UI visibility flags
int g_show_border = 0;  // Initialize to 0 (disabled) so border is not visible by default
int g_border_width = 5;
int g_show_corner_markers = 1;
int g_show_stats_overlay = 0;  // Stats overlay disabled by default

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

int *get_keystone_border_visible_ptr(void) {
    return &g_keystone.border_visible;
}

int *get_keystone_border_width_ptr(void) {
    return &g_keystone.border_width;
}

int *get_keystone_corner_markers_ptr(void) {
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
    g_keystone.border_visible = false;  // Default to hiding border
    g_keystone.border_width = 5;
    g_keystone.corner_markers = true;
    g_keystone.active_corner = -1;
    
    // Initialize border to disabled by default
    g_show_border = 0;
    
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
    
    // Initialize hardware keystone (DRM only - DispmanX has been removed)
    if (drm_keystone_is_supported()) {
        if (!drm_keystone_init()) {
            LOG_WARN("Failed to initialize DRM keystone, will fall back to software");
        } else {
            LOG_INFO("Initialized DRM keystone");
        }
    } else {
        LOG_INFO("Hardware keystone not supported, using software implementation");
    }
}

void keystone_cleanup(void) {
    cleanup_keystone_shader();
    cleanup_keystone_fbo();
    cleanup_mesh_resources();
    cleanup_keystone_resources();
    
    // Clean up hardware keystone resources
    if (drm_keystone_is_active()) {
        drm_keystone_cleanup();
    }
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
    
    // Validate that all required attributes were found
    if (g_keystone_a_position_loc < 0 || g_keystone_a_texcoord_loc < 0 || g_keystone_u_texture_loc < 0) {
        LOG_ERROR("Failed to get keystone shader attributes: pos=%d tex=%d u_tex=%d", 
                  g_keystone_a_position_loc, g_keystone_a_texcoord_loc, g_keystone_u_texture_loc);
        
        // Clean up on failure
        glDeleteProgram(g_keystone_shader_program);
        glDeleteShader(g_keystone_vertex_shader);
        glDeleteShader(g_keystone_fragment_shader);
        g_keystone_shader_program = 0;
        g_keystone_vertex_shader = 0;
        g_keystone_fragment_shader = 0;
        return false;
    }
    
    LOG_INFO("Keystone shader initialized: program=%u, pos=%d, tex=%d, u_tex=%d", 
             g_keystone_shader_program, g_keystone_a_position_loc, g_keystone_a_texcoord_loc, g_keystone_u_texture_loc);
    
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
    
    // Apply adjustment scaled by step size - use larger scale factor
    float scale = (float)g_keystone_adjust_step / 100.0f; // Changed from 1000.0f to 100.0f
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
    
    // Update hardware keystone if enabled and supported (DRM only - DispmanX removed)
    if (g_keystone.enabled) {
        if (drm_keystone_is_supported()) {
            drm_keystone_update(&g_keystone);
        }
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
            // Sync with global border flag to ensure visibility
            g_show_border = g_keystone.border_visible ? 1 : 0;
        }
        else if (strncmp(line, "cornermarks=", 12) == 0) {
            g_keystone.corner_markers = (atoi(line + 12) != 0);
            // Sync with global corner markers flag
            g_show_corner_markers = g_keystone.corner_markers ? 1 : 0;
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
    
    // Apply hardware keystone if enabled (DRM only - DispmanX removed)
    if (g_keystone.enabled) {
        if (drm_keystone_is_supported()) {
            if (!drm_keystone_update(&g_keystone)) {
                LOG_WARN("Failed to apply DRM keystone, falling back to software implementation");
            } else {
                LOG_INFO("Applied DRM keystone transformation");
            }
        } else {
            LOG_INFO("Hardware keystone not supported, using software implementation");
        }
    } else {
        // Clean up hardware keystone when disabled
        if (drm_keystone_is_active()) {
            drm_keystone_cleanup();
        }
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
    
    // Hardware keystone handled by DRM/KMS
    
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
    LOG_INFO("Keystone handler received key: %d (0x%02x) '%c'", 
             (int)key, (int)key, (key >= 32 && key < 127) ? key : '?');
    
    // Handle keys that work regardless of keystone mode
    switch (key) {
        case 'v': // Toggle stats overlay (available always)
            g_show_stats_overlay = !g_show_stats_overlay;
            fprintf(stderr, "\rStats overlay %s\n", g_show_stats_overlay ? "enabled" : "disabled");
            return true;
        case 'k': // Toggle keystone mode
            g_keystone.enabled = !g_keystone.enabled;
            if (g_keystone.enabled) {
                g_keystone.active_corner = 0;
                keystone_update_matrix();
                LOG_INFO("Keystone correction enabled, adjusting corner %d", 
                        g_keystone.active_corner + 1);
                fprintf(stderr, "\rKeystone correction enabled, use arrow keys to adjust corner %d", 
                       g_keystone.active_corner + 1);
            } else {
                fprintf(stderr, "\rKeystone correction disabled\n");
            }
            return true;
    }
    
    // Handle keystone-specific keys only when keystone mode is enabled
    if (!g_keystone.enabled) {
        return false;
    }
    
    // Process keystone keys only when keystone mode is enabled
    switch (key) {
        // Corner selection
        case '1': // Top-left (ASCII 49)
        case 1:   // Alternative keycode that might be sent
            g_keystone.active_corner = 0;
            LOG_INFO("Selected corner 1 (top-left)");
            fprintf(stderr, "\rAdjusting corner %d (top-left)\n", g_keystone.active_corner + 1);
            return true;
        case '2': // Top-right (ASCII 50)
        case 2:   // Alternative keycode that might be sent
            g_keystone.active_corner = 1;
            LOG_INFO("Selected corner 2 (top-right)");
            fprintf(stderr, "\rAdjusting corner %d (top-right)\n", g_keystone.active_corner + 1);
            return true;
        case '3': // Bottom-left (ASCII 51)
        case 3:   // Alternative keycode that might be sent
            g_keystone.active_corner = 2;
            LOG_INFO("Selected corner 3 (bottom-left)");
            fprintf(stderr, "\rAdjusting corner %d (bottom-left)\n", g_keystone.active_corner + 1);
            return true;
        case '4': // Bottom-right (ASCII 52)
        case 4:   // Alternative keycode that might be sent
            g_keystone.active_corner = 3;
            LOG_INFO("Selected corner 4 (bottom-right)");
            fprintf(stderr, "\rAdjusting corner %d (bottom-right)\n", g_keystone.active_corner + 1);
            return true;
            
        // Movement keys - arrow keys only
        case 65: // Up arrow
            LOG_INFO("Keystone: Processing Up arrow key for corner %d", g_keystone.active_corner + 1);
            keystone_adjust_corner(g_keystone.active_corner, 0, -0.05f);  // Increased from -0.01f
            return true;
        case 66: // Down arrow
            LOG_INFO("Keystone: Processing Down arrow key for corner %d", g_keystone.active_corner + 1);
            keystone_adjust_corner(g_keystone.active_corner, 0, 0.05f);  // Increased from 0.01f
            return true;
        case 68: // Left arrow
            LOG_INFO("Keystone: Processing Left arrow key for corner %d", g_keystone.active_corner + 1);
            keystone_adjust_corner(g_keystone.active_corner, -0.05f, 0);  // Increased from -0.01f
            return true;
        case 67: // Right arrow
            LOG_INFO("Keystone: Processing Right arrow key for corner %d", g_keystone.active_corner + 1);
            keystone_adjust_corner(g_keystone.active_corner, 0.05f, 0);  // Increased from 0.01f
            return true;
            
        // Toggle features
        case 'b': // Toggle border
            g_show_border = !g_show_border;
            // Sync keystone border visibility with the global setting
            g_keystone.border_visible = (g_show_border != 0);
            fprintf(stderr, "\rBorder %s\n", g_show_border ? "enabled" : "disabled");
            return true;
        case 'c': // Toggle corner markers
            g_show_corner_markers = !g_show_corner_markers;
            // Sync keystone corner markers visibility with the global setting
            g_keystone.corner_markers = (g_show_corner_markers != 0);
            fprintf(stderr, "\rCorner markers %s\n", g_show_corner_markers ? "enabled" : "disabled");
            return true;
        case 'r': // Reset keystone
            keystone_init(); // Re-initialize to defaults
            fprintf(stderr, "\rKeystone settings reset\n");
            return true;
        case 'S': // Save keystone (uppercase S)
        case 's': // Save keystone (lowercase s)
            // Save to both the current directory and user config
            if (keystone_save_config("./keystone.conf")) {
                fprintf(stderr, "\rKeystone configuration saved to ./keystone.conf\n");
            }
            // Also try to save to user config directory
            char config_path[PATH_MAX];
            const char* home = getenv("HOME");
            if (home) {
                snprintf(config_path, sizeof(config_path), "%s/.config/pickle_keystone.conf", home);
                if (keystone_save_config(config_path)) {
                    fprintf(stderr, "\rKeystone configuration also saved to %s\n", config_path);
                }
            }
            return true;
        // Remove handling of raw ESC key here since it's part of arrow key sequences
        // and is already properly handled in event_callbacks.c
    }
    
    return false; // Key not handled
}