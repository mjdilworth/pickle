#include "render_backend.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

#ifdef VULKAN_ENABLED
#include "vulkan.h"
#endif

// Global variables for render backend
static render_backend_type_t g_preferred_backend = RENDER_BACKEND_AUTO;
static render_backend_type_t g_active_backend = RENDER_BACKEND_GLES; // Default to GLES

// Detect the best available backend
render_backend_type_t render_backend_detect_best(void) {
#ifdef VULKAN_ENABLED
    if (vulkan_is_available()) {
        return RENDER_BACKEND_VULKAN;
    }
#endif
    return RENDER_BACKEND_GLES;
}

// Get the name of a backend
const char* render_backend_name(render_backend_type_t type) {
    switch (type) {
        case RENDER_BACKEND_GLES:
            return "OpenGL ES";
        case RENDER_BACKEND_VULKAN:
            return "Vulkan";
        case RENDER_BACKEND_AUTO:
            return "Auto";
        default:
            return "Unknown";
    }
}

// Check if a backend is available
bool render_backend_is_available(render_backend_type_t type) {
    switch (type) {
        case RENDER_BACKEND_GLES:
            return true;  // GLES is always available
        case RENDER_BACKEND_VULKAN:
#ifdef VULKAN_ENABLED
            if (vulkan_is_available()) {
                LOG_INFO("Vulkan backend is available");
                return true;
            } else {
                LOG_WARN("Vulkan support was compiled in but Vulkan runtime is not available");
                return false;
            }
#else
            LOG_WARN("Vulkan support is not enabled in this build");
            return false;
#endif
        case RENDER_BACKEND_AUTO:
            return true;  // Auto is always available
        default:
            return false;
    }
}

// Set the preferred backend
void render_backend_set_preferred(render_backend_type_t type) {
    g_preferred_backend = type;
    
    // Log the preferred backend
    LOG_INFO("Preferred render backend set to %s", render_backend_name(type));
    
    // If auto is selected, detect the best backend
    if (type == RENDER_BACKEND_AUTO) {
        render_backend_type_t best = render_backend_detect_best();
        LOG_INFO("Auto-selected render backend: %s", render_backend_name(best));
    }
    
    // Check if the selected backend is available
    if (!render_backend_is_available(type)) {
        LOG_WARN("Selected backend %s is not available, falling back to GLES", render_backend_name(type));
        g_active_backend = RENDER_BACKEND_GLES;
    } else {
        g_active_backend = type;
    }
}

// Get the preferred backend
render_backend_type_t render_backend_get_preferred(void) {
    return g_preferred_backend;
}

// Get the active backend
render_backend_type_t render_backend_get_active(void) {
    return g_active_backend;
}