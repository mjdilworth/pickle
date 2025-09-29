#include "pickle_events.h"
#include "event_callbacks.h"
#include "input.h"
#include "v4l2_player.h"
#include "pickle_globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
extern int g_pending_flip;
extern int g_use_v4l2_decoder;
extern int g_scanout_disabled;
extern uint64_t g_frames;
extern double g_video_fps;

// Define logging macros similar to other Pickle components
#define LOG_EVENT(fmt, ...) fprintf(stderr, "[EVENT] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) do { } while(0)
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)

// External render function declarations
// External render function declarations
extern bool render_frame_mpv(mpv_handle *mpv, mpv_render_context *mpv_gl, kms_ctx_t *drm, egl_ctx_t *eglc);
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
    if (get_joystick_fd() >= 0) {
        if (event_register(ctx, get_joystick_fd(), EVENT_TYPE_JOYSTICK, EPOLLIN, joystick_event_callback, NULL) < 0) {
            LOG_ERROR("Failed to register joystick events");
            event_cleanup(ctx);
            return NULL;
        }
        LOG_EVENT("Registered joystick events");
    }
    
    // If using V4L2 decoder, create a timer for frame updates
    if (g_use_v4l2_decoder && v4l2_player) {
        // Adaptive frame interval based on detected video FPS (default 60fps if not yet detected)
        double fps = (g_video_fps > 0) ? g_video_fps : 60.0;
        int frame_interval_ms = (int)(1000.0 / fps);
        if (frame_interval_ms < 1) frame_interval_ms = 1; // Minimum 1ms (max 1000fps)
        LOG_EVENT("Creating V4L2 timer with %dms interval (%.1f fps)", frame_interval_ms, fps);
        int timer_fd = event_create_timer(ctx, frame_interval_ms, v4l2_timer_callback, v4l2_player);
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
        g_mpv_update_flags &= ~((uint64_t)MPV_RENDER_UPDATE_FRAME);
    }
    
    // Render a frame if needed
    if (need_frame) {
        bool render_ok = false;
        
        // Use the appropriate render function based on decoder type
        if (g_use_v4l2_decoder && v4l2_player) {
            render_ok = render_v4l2_frame(drm, egl, v4l2_player);
        } else if (player && player->handle) {
            render_ok = render_frame_mpv(player->handle, player->render_ctx, drm, egl);
        }
        
        if (!render_ok) {
            LOG_ERROR("Frame rendering failed");
            return false;
        }
    }
    
    return true;
}