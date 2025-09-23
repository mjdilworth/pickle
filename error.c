#include "error.h"
#include <stdarg.h>

// Maximum error message length
#define MAX_ERROR_MSG_LEN 1024

// Error context structure
struct pickle_error_context {
    char last_error_msg[MAX_ERROR_MSG_LEN];
    pickle_result_t last_error_code;
};

// Global error context
static struct pickle_error_context g_error_ctx = {{0}, PICKLE_SUCCESS};

void pickle_log_error(const char *file, int line, const char *func, pickle_result_t code, const char *fmt, ...) {
    // Format the base message with file, line, function info
    int offset = snprintf(g_error_ctx.last_error_msg, MAX_ERROR_MSG_LEN,
                         "[ERROR] %s:%d in %s: ", file, line, func);
                         
    // Add the specific error message
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error_ctx.last_error_msg + offset, (size_t)(MAX_ERROR_MSG_LEN - offset), fmt, args);
    va_end(args);
    
    // Store the error code
    g_error_ctx.last_error_code = code;
    
    // Print the error to stderr
    fprintf(stderr, "%s\n", g_error_ctx.last_error_msg);
}

const char *pickle_error_string(pickle_result_t code) {
    switch (code) {
        case PICKLE_SUCCESS:
            return "Success";
        case PICKLE_ERROR_GENERIC:
            return "Generic error";
        case PICKLE_ERROR_MEMORY:
            return "Memory allocation error";
        case PICKLE_ERROR_IO:
            return "I/O error";
        case PICKLE_ERROR_GL:
            return "OpenGL error";
        case PICKLE_ERROR_MPV:
            return "MPV error";
        case PICKLE_ERROR_DRM:
            return "DRM error";
        case PICKLE_ERROR_EGL:
            return "EGL error";
        case PICKLE_ERROR_INIT:
            return "Initialization error";
        case PICKLE_ERROR_INVALID_PARAM:
            return "Invalid parameter";
        case PICKLE_ERROR_NOT_IMPLEMENTED:
            return "Not implemented";
        default:
            return "Unknown error";
    }
}

const char *pickle_get_last_error(void) {
    return g_error_ctx.last_error_msg;
}