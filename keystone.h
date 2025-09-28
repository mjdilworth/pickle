#ifndef PICKLE_KEYSTONE_H
#define PICKLE_KEYSTONE_H

#include <GLES3/gl31.h>
#include <stdbool.h>

// Keystone data structure (contains the projection parameters)
typedef struct {
    float points[4][2];      // 4 corner points (TL, TR, BL, BR) in normalized -1 to 1 space
    int selected_corner;     // Currently selected corner (0-3, -1 for none)
    bool enabled;            // Flag to enable/disable keystone correction
    float matrix[16];        // The transformation matrix for rendering
    bool mesh_enabled;       // Whether to use mesh-based warping instead of simple 4-point
    int mesh_size;           // Number of subdivisions for the keystone mesh
    float **mesh_points;     // 2D array of mesh vertices
    int active_mesh_point[2];// Active mesh point coordinates (x,y) or (-1,-1) for none
    bool initialized;        // Flag to indicate if keystone has been initialized
    int border_visible;      // Whether to display border around the projection
    int border_width;        // Width of the border (when visible)
    int corner_markers;      // Whether to display corner markers
    bool perspective_pins[4];// Whether each corner is pinned (fixed) during adjustments
    int active_corner;       // Which corner is being adjusted (-1 = none)
} keystone_t;

// External global variables
extern keystone_t g_keystone;
extern int g_keystone_adjust_step;
extern int g_show_border;
extern int g_border_width;
extern int g_show_corner_markers;
extern int g_show_stats_overlay;
extern GLuint g_keystone_shader_program;
extern GLuint g_keystone_vertex_shader;
extern GLuint g_keystone_fragment_shader;
extern GLuint g_keystone_vertex_buffer;
extern GLuint g_keystone_texcoord_buffer;
extern GLuint g_keystone_index_buffer;
extern GLuint g_keystone_fbo;
extern GLuint g_keystone_fbo_texture;
extern int g_keystone_fbo_w;
extern int g_keystone_fbo_h;
extern GLint g_keystone_a_position_loc;
extern GLint g_keystone_a_texcoord_loc;
extern GLint g_keystone_u_texture_loc;

// Main keystone functions
void keystone_init(void);
void keystone_cleanup(void);
bool keystone_handle_key(char key);
void keystone_adjust_corner(int corner, float x_delta, float y_delta);
void keystone_update_matrix(void);
bool keystone_save_config(const char* path);
bool keystone_load_config(const char* path);
void cleanup_mesh_resources(void);
void cleanup_keystone_resources(void);

// Rendering functions
bool init_keystone_shader(void);
void cleanup_keystone_shader(void);
void update_keystone_mesh(void);
void render_with_keystone(GLuint source_texture, int width, int height);

// Toggle functions
void keystone_toggle_enabled(void);
void keystone_toggle_border(void);
void keystone_toggle_corner_markers(void);
void keystone_reset(void);

// Border functions
void keystone_adjust_border_width(int delta);

// FBO management for keystone rendering
bool ensure_keystone_fbo(int width, int height);
void cleanup_keystone_fbo(void);

// Accessor functions
bool is_keystone_enabled(void);
bool is_keystone_border_visible(void);
bool is_keystone_corner_markers_visible(void);
int get_keystone_border_width(void);
int get_keystone_selected_corner(void);

// Accessor functions for adapter layer (transition)
keystone_t *get_keystone_data(void);
int *get_keystone_adjust_step(void);
int *get_keystone_border_visible_ptr(void);
int *get_keystone_border_width_ptr(void);
int *get_keystone_corner_markers_ptr(void);
GLuint *get_keystone_shader_program_ptr(void);
GLuint *get_keystone_vertex_shader_ptr(void);
GLuint *get_keystone_fragment_shader_ptr(void);
GLuint *get_keystone_vertex_buffer_ptr(void);
GLuint *get_keystone_texcoord_buffer_ptr(void);
GLuint *get_keystone_index_buffer_ptr(void);
GLuint *get_keystone_fbo_ptr(void);
GLuint *get_keystone_fbo_texture_ptr(void);
int *get_keystone_fbo_w_ptr(void);
int *get_keystone_fbo_h_ptr(void);
GLint *get_keystone_a_position_loc_ptr(void);
GLint *get_keystone_a_texcoord_loc_ptr(void);
GLint *get_keystone_u_texture_loc_ptr(void);
bool *get_keystone_mesh_enabled_ptr(void);
bool (*get_keystone_perspective_pins_ptr(void))[4];
int *get_keystone_active_corner_ptr(void);
int (*get_keystone_active_mesh_point_ptr(void))[2];
float (*get_keystone_matrix_ptr(void))[16];

// Additional functions
void keystone_adjust_mesh_point(int row, int col, float x_delta, float y_delta);
void keystone_toggle_pin(int corner);
void cleanup_mesh_resources(void);

// Get the current keystone configuration
keystone_t *keystone_get_config(void);

#endif // PICKLE_KEYSTONE_H