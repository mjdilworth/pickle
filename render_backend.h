#ifndef PICKLE_RENDER_BACKEND_H
#define PICKLE_RENDER_BACKEND_H

#include <stdbool.h>
#include "error.h"

// Render backend type enumeration
typedef enum {
    RENDER_BACKEND_GLES,   // OpenGL ES
    RENDER_BACKEND_VULKAN, // Vulkan
    RENDER_BACKEND_AUTO    // Automatically select best available
} render_backend_type_t;

// Backend detection and selection
render_backend_type_t render_backend_detect_best(void);
const char* render_backend_name(render_backend_type_t type);
bool render_backend_is_available(render_backend_type_t type);

// Backend selection functions
void render_backend_set_preferred(render_backend_type_t type);
render_backend_type_t render_backend_get_preferred(void);
render_backend_type_t render_backend_get_active(void);

#endif // PICKLE_RENDER_BACKEND_H