#ifndef PICKLE_ERROR_H
#define PICKLE_ERROR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Error codes for pickle
typedef enum {
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
    PICKLE_ERROR_NOT_IMPLEMENTED = -10
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