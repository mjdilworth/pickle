#include "utils.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl31.h>
#include <mpv/client.h>

// Global debug flag (defined here)
// g_debug is now defined in pickle_globals.c
static volatile sig_atomic_t g_interrupted = 0;

double tv_diff(const struct timeval *tv1, const struct timeval *tv2) {
    return (double)(tv2->tv_sec - tv1->tv_sec) + 
           (double)(tv2->tv_usec - tv1->tv_usec) / 1000000.0;
}

static void signal_handler(int sig) {
    g_interrupted = 1;
    LOG_INFO("Signal %d received, shutting down...", sig);
}

void setup_signal_handlers(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler);
}

bool is_interrupted(void) {
    return g_interrupted != 0;
}

void check_gl_error(const char *file, int line, const char *msg) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "[GL_ERROR] %s:%d: %s (0x%x)\n", file, line, msg, err);
    }
}

void log_opt_result(const char *option, int result) {
    if (result < 0) {
        fprintf(stderr, "Warning: setting %s returned error %d: %s\n", 
                option, result, mpv_error_string(result));
    }
}