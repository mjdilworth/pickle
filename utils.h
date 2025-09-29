#ifndef PICKLE_UTILS_H
#define PICKLE_UTILS_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/time.h>

// Logging macros
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
// Provide empty LOG_DEBUG for remaining references (debug disabled in release)
#define LOG_DEBUG(fmt, ...) do { } while(0)

// Global debug flag (extern declaration)
extern int g_debug;

// Timing utilities
double tv_diff(const struct timeval *tv1, const struct timeval *tv2);

// Signal handling
void setup_signal_handlers(void);
bool is_interrupted(void);

// OpenGL error checking
void check_gl_error(const char *file, int line, const char *msg);
#define CHECK_GL_ERROR(msg) check_gl_error(__FILE__, __LINE__, msg)

// Utility for logging option setting results
void log_opt_result(const char *option, int result);

#endif // PICKLE_UTILS_H