#ifndef PICKLE_KEYSTONE_H
#define PICKLE_KEYSTONE_H

#include <GLES2/gl2.h>
#include <stdbool.h>

// Keystone data structure (contains the projection parameters)
typedef struct {
    float points[4][2];      // 4 corner points (TL, TR, BL, BR) in normalized -1 to 1 space
    int mesh_size;           // Number of subdivisions for the keystone mesh
    float **mesh_points;     // 2D array of mesh vertices
    bool initialized;        // Flag to indicate if keystone has been initialized
    bool enabled;            // Flag to enable/disable keystone correction
    int selected_corner;     // Currently selected corner (0-3, -1 for none)
    int border_visible;      // Whether to display border around the projection
    int border_width;        // Width of the border (when visible)
    int corner_markers;      // Whether to display corner markers
} keystone_t;

// Main keystone functions
void keystone_init(void);
void keystone_cleanup(void);
bool keystone_handle_key(char key);
void keystone_adjust_corner(int corner, float x_delta, float y_delta);
void keystone_update_matrix(void);
bool keystone_save_config(const char* path);
bool keystone_load_config(const char* path);

// Rendering functions
bool init_keystone_shader(void);
void cleanup_keystone_shader(void);
void update_keystone_mesh(void);
void render_with_keystone(GLuint source_texture, int width, int height);
void draw_keystone_border(void);
void draw_keystone_corner_markers(void);

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

#endif // PICKLE_KEYSTONE_H