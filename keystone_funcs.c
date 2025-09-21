#include "keystone.h"
#include "utils.h"
#include <stdlib.h>

// Get access to the internal keystone data structure
extern keystone_t *get_keystone_data(void);

void keystone_adjust_mesh_point(int row, int col, float x_delta, float y_delta) {
    keystone_t *ks = get_keystone_data();
    if (row < 0 || row >= ks->mesh_size || col < 0 || col >= ks->mesh_size) {
        return;
    }
    
    // Adjust the mesh point
    ks->mesh_points[row][col*2] += x_delta;
    ks->mesh_points[row][col*2+1] += y_delta;
    
    // Clamp to reasonable range (-1 to 2)
    if (ks->mesh_points[row][col*2] < -1.0f) ks->mesh_points[row][col*2] = -1.0f;
    if (ks->mesh_points[row][col*2] >  2.0f) ks->mesh_points[row][col*2] =  2.0f;
    if (ks->mesh_points[row][col*2+1] < -1.0f) ks->mesh_points[row][col*2+1] = -1.0f;
    if (ks->mesh_points[row][col*2+1] >  2.0f) ks->mesh_points[row][col*2+1] =  2.0f;
}

void keystone_toggle_pin(int corner) {
    keystone_t *ks = get_keystone_data();
    if (corner < 0 || corner > 3) return;
    
    ks->perspective_pins[corner] = !ks->perspective_pins[corner];
    
    LOG_INFO("Corner %d pin: %s", corner, ks->perspective_pins[corner] ? "enabled" : "disabled");
}

// cleanup_mesh_resources is defined in keystone.c