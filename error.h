#ifndef PICKLE_ERROR_H
#define PICKLE_ERROR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Error codes for pickle
typedef enum {
    PICKLE_OK = 0,
    PICKLE_SUCCESS = 0,
    PICKLE_ERROR_GENERIC = -1,
    PICKLE_ERROR_MEMORY = -2,
    PICKLE_ERROR_IO = -3,
    PICKLE_ERROR_GL = -4,
    PICKLE_ERROR_MPV = -5,
    PICKLE_ERROR_DRM = -6,
    PICKLE_ERROR_EGL = -7,
    PICKLE_ERROR_INIT = -8,
    PICKLE_ERROR_INVALID_PARAM = -9,
    PICKLE_ERROR_INVALID_PARAMETER = -9, // Alias for INVALID_PARAM
    PICKLE_ERROR_NOT_IMPLEMENTED = -10,
    PICKLE_ERROR_GL_FRAMEBUFFER = -11,
    PICKLE_ERROR_UNSUPPORTED = -12,
    // Vulkan specific error codes
    PICKLE_ERROR_VULKAN_INSTANCE = -100,
    PICKLE_ERROR_VULKAN_DEVICE = -101,
    PICKLE_ERROR_VULKAN_SURFACE = -102,
    PICKLE_ERROR_VULKAN_SWAPCHAIN = -103,
    PICKLE_ERROR_VULKAN_COMMAND_POOL = -104,
    PICKLE_ERROR_VULKAN_COMMAND_BUFFERS = -105,
    PICKLE_ERROR_VULKAN_SYNC_OBJECTS = -106,
    PICKLE_ERROR_VULKAN_SHADER = -107,
    PICKLE_ERROR_VULKAN_PIPELINE = -108,
    PICKLE_ERROR_VULKAN_FRAMEBUFFER = -109,
    PICKLE_ERROR_VULKAN_MEMORY = -110,
    PICKLE_ERROR_VULKAN_IMAGE = -111,
    PICKLE_ERROR_VULKAN_VALIDATION_LAYERS = -112,
    PICKLE_ERROR_VULKAN_DEBUG_MESSENGER = -113,
    PICKLE_ERROR_VULKAN_NO_DEVICE = -114,
    PICKLE_ERROR_VULKAN_NO_SUITABLE_DEVICE = -115,
    PICKLE_ERROR_OUT_OF_MEMORY = -116,
    PICKLE_ERROR_GBM_INIT = -117,
    PICKLE_ERROR_GBM_SURFACE = -118,
    PICKLE_ERROR_VULKAN_NO_DISPLAY = -119,
    PICKLE_ERROR_VULKAN_NO_DISPLAY_MODE = -120,
    PICKLE_ERROR_VULKAN_IMAGE_VIEW = -121,
    PICKLE_ERROR_VULKAN_RENDER_PASS = -122
} pickle_result_t;

// Forward declaration for error context
typedef struct pickle_error_context pickle_error_context_t;

// Macro for error handling
#define PICKLE_RETURN_IF_ERROR(expr) do { \
    pickle_result_t __result = (expr); \
    if (__result != PICKLE_SUCCESS) { \
        return __result; \
    } \
} while(0)

// Log error and return code
#define PICKLE_ERROR_RETURN(code, fmt, ...) do { \
    pickle_log_error(__FILE__, __LINE__, __func__, code, fmt, ##__VA_ARGS__); \
    return code; \
} while(0)

// Log error, cleanup, and return code
#define PICKLE_ERROR_CLEANUP_RETURN(code, cleanup_expr, fmt, ...) do { \
    pickle_log_error(__FILE__, __LINE__, __func__, code, fmt, ##__VA_ARGS__); \
    { cleanup_expr; } \
    return code; \
} while(0)

// Check if expression evaluates to true
#define PICKLE_CHECK(expr, code, fmt, ...) do { \
    if (!(expr)) { \
        pickle_log_error(__FILE__, __LINE__, __func__, code, fmt, ##__VA_ARGS__); \
        return code; \
    } \
} while(0)

// Check memory allocation
#define PICKLE_CHECK_ALLOC(ptr) \
    PICKLE_CHECK(ptr != NULL, PICKLE_ERROR_MEMORY, "Memory allocation failed")

// Error reporting function
void pickle_log_error(const char *file, int line, const char *func, pickle_result_t code, const char *fmt, ...);

// Convert pickle error code to string
const char *pickle_error_string(pickle_result_t code);

// Get last error message
const char *pickle_get_last_error(void);

#endif // PICKLE_ERROR_H