#ifndef PICKLE_LOG_H
#define PICKLE_LOG_H

#include <stdio.h>
#include <string.h>

// Simple logging macros
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)

// Only define these if they're not already defined (avoid conflict with utils.h)
#ifndef LOG_INFO
#define LOG_INFO(fmt, ...)  fprintf(stderr, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef LOG_DEBUG
#define LOG_DEBUG(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#endif

// Domain-specific logging
#define LOG_DRM(fmt, ...)   fprintf(stderr, "[DRM]   " fmt "\n", ##__VA_ARGS__)
#define LOG_EGL(fmt, ...)   fprintf(stderr, "[EGL]   " fmt "\n", ##__VA_ARGS__)
#define LOG_GL(fmt, ...)    fprintf(stderr, "[GL]    " fmt "\n", ##__VA_ARGS__)
#define LOG_MPV(fmt, ...)   fprintf(stderr, "[MPV]   " fmt "\n", ##__VA_ARGS__)

// Error with return helpers
#define RETURN_ERROR(msg) do { LOG_ERROR("%s", (msg)); return false; } while(0)
#define RETURN_ERROR_ERRNO(msg) do { LOG_ERROR("%s: %s", (msg), strerror(errno)); return false; } while(0)
#define RETURN_ERROR_EGL(msg) do { LOG_ERROR("%s: 0x%x", (msg), eglGetError()); return false; } while(0)

#endif // PICKLE_LOG_H