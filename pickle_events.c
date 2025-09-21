#include "pickle_events.h"
#include "event_callbacks.h"
#include "v4l2_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

// External globals from pickle.c
extern int g_debug;
extern int g_stop;
extern volatile int g_mpv_wakeup;
extern volatile uint64_t g_mpv_update_flags;
extern int g_joystick_fd;
extern int g_mpv_pipe[2];
extern int g_pending_flip;
extern int g_use_v4l2_decoder;
extern int g_scanout_disabled;
extern uint64_t g_frames;

// Define logging macros similar to other Pickle components
#define LOG_EVENT(fmt, ...) fprintf(stderr, "[EVENT] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) do { if (g_debug) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)

// External render function declarations
extern bool render_frame_mpv(kms_ctx_t *d, egl_ctx_t *e, mpv_player_t *p);
extern bool render_v4l2_frame(kms_ctx_t *d, egl_ctx_t *e, v4l2_player_t *p);

// Initialize the event system for Pickle
event_ctx_t *pickle_event_init(kms_ctx_t *drm, mpv_player_t *player, v4l2_player_t *v4l2_player) {
    // Initialize the event system with a reasonable number of sources
    event_ctx_t *ctx = event_init(16);
    if (!ctx) {
        LOG_ERROR("Failed to initialize event system");
        return NULL;
    }
    
    // Register DRM events
    if (drm && drm->fd >= 0 && !g_scanout_disabled) {
        if (event_register(ctx, drm->fd, EVENT_TYPE_DRM, EPOLLIN, drm_event_callback, drm) < 0) {
            LOG_ERROR("Failed to register DRM events");
            event_cleanup(ctx);
            return NULL;
        }
        LOG_EVENT("Registered DRM events");
    }
    
    // Register MPV events
    if (player && g_mpv_pipe[0] >= 0) {
        if (event_register(ctx, g_mpv_pipe[0], EVENT_TYPE_MPV, EPOLLIN, mpv_event_callback, player) < 0) {
            LOG_ERROR("Failed to register MPV events");
            event_cleanup(ctx);
            return NULL;
        }
        LOG_EVENT("Registered MPV events");
    }
    
    // Register keyboard input events
    if (event_register(ctx, STDIN_FILENO, EVENT_TYPE_INPUT, EPOLLIN, keyboard_event_callback, player) < 0) {
        LOG_ERROR("Failed to register keyboard events");
        event_cleanup(ctx);
        return NULL;
    }
    LOG_EVENT("Registered keyboard events");
    
    // Register joystick events
    if (g_joystick_fd >= 0) {
        if (event_register(ctx, g_joystick_fd, EVENT_TYPE_JOYSTICK, EPOLLIN, joystick_event_callback, NULL) < 0) {
            LOG_ERROR("Failed to register joystick events");
            event_cleanup(ctx);
            return NULL;
        }
        LOG_EVENT("Registered joystick events");
    }
    
    // If using V4L2 decoder, create a timer for frame updates
    if (g_use_v4l2_decoder && v4l2_player) {
        // Update at ~25fps (40ms interval)
        int timer_fd = event_create_timer(ctx, 40, v4l2_timer_callback, v4l2_player);
        if (timer_fd < 0) {
            LOG_ERROR("Failed to create V4L2 timer");
            event_cleanup(ctx);
            return NULL;
        }
        LOG_EVENT("Created V4L2 timer");
    }
    
    // Register signal handlers for clean shutdown
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    
    // Block the signals
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_ERROR("Failed to block signals: %s", strerror(errno));
        event_cleanup(ctx);
        return NULL;
    }
    
    // Create a signalfd for the signals
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        LOG_ERROR("Failed to create signalfd: %s", strerror(errno));
        event_cleanup(ctx);
        return NULL;
    }
    
    // Register the signalfd
    if (event_register(ctx, sfd, EVENT_TYPE_SIGNAL, EPOLLIN, signal_event_callback, NULL) < 0) {
        LOG_ERROR("Failed to register signal events");
        close(sfd);
        event_cleanup(ctx);
        return NULL;
    }
    LOG_EVENT("Registered signal events");
    
    LOG_INFO("Event system initialized successfully");
    return ctx;
}

// Clean up the event system
void pickle_event_cleanup(event_ctx_t *ctx) {
    if (ctx) {
        event_cleanup(ctx);
        LOG_INFO("Event system cleaned up");
    }
}

// Process events and render frames
bool pickle_event_process_and_render(event_ctx_t *ctx, kms_ctx_t *drm, egl_ctx_t *egl,
                                   mpv_player_t *player, v4l2_player_t *v4l2_player,
                                   int timeout_ms) {
    if (!ctx || !drm || !egl) {
        LOG_ERROR("Invalid parameters for pickle_event_process_and_render");
        return false;
    }
    
    // Process events
    int events_processed = event_process(ctx, timeout_ms);
    if (events_processed < 0) {
        LOG_ERROR("Error processing events");
        return false;
    }
    
    // Check if we should stop
    if (g_stop) {
        return false;
    }
    
    // Check if we need to render a frame
    int need_frame = 0;
    
    // Always render the first frame
    if (g_frames == 0 && !g_pending_flip) {
        need_frame = 1;
    }
    
    // Check if MPV wants to render a frame
    if (g_mpv_update_flags & MPV_RENDER_UPDATE_FRAME) {
        need_frame = 1;
        g_mpv_update_flags &= ~MPV_RENDER_UPDATE_FRAME;
    }
    
    // Render a frame if needed
    if (need_frame) {
        bool render_ok = false;
        
        // Use the appropriate render function based on decoder type
        if (g_use_v4l2_decoder && v4l2_player) {
            render_ok = render_v4l2_frame(drm, egl, v4l2_player);
        } else if (player && player->handle) {
            render_ok = render_frame_mpv(drm, egl, player);
        }
        
        if (!render_ok) {
            LOG_ERROR("Frame rendering failed");
            return false;
        }
    }
    
    return true;
}