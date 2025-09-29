#include "event_callbacks.h"
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <xf86drm.h>
#include <drm/drm_fourcc.h>
#include <linux/joystick.h>

// External globals declared in pickle_globals.h
extern int g_debug;
extern volatile int g_stop;
extern volatile int g_mpv_wakeup;
extern volatile uint64_t g_mpv_update_flags;
extern int g_joystick_enabled;
extern int g_help_visible;
extern int g_help_toggle_request;

// Define logging macros similar to other Pickle components
#define LOG_EVENT(fmt, ...) fprintf(stderr, "[EVENT] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)

// DRM event callback
void drm_event_callback(int fd, uint32_t events, void *user_data) {
    (void)events; // Unused
    
    kms_ctx_t *drm = (kms_ctx_t *)user_data;
    if (!drm) {
        LOG_ERROR("Invalid DRM context in callback");
        return;
    }
    
    // Handle DRM events
    drmEventContext ev = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = page_flip_handler
    };
    
    if (drmHandleEvent(fd, &ev) < 0) {
        LOG_ERROR("drmHandleEvent failed: %s", strerror(errno));
    }
}

// MPV event callback
void mpv_event_callback(int fd, uint32_t events, void *user_data) {
    (void)events; // Unused
    
    mpv_player_t *player = (mpv_player_t *)user_data;
    if (!player) {
        LOG_ERROR("Invalid MPV player in callback");
        return;
    }
    
    // Drain the pipe
    unsigned char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {
        /* drain */
    }
    
    // Set the wakeup flag
    g_mpv_wakeup = 1;
    
    // Process MPV events
    drain_mpv_events(player->handle);
    if (player->render_ctx) {
        uint64_t flags = mpv_render_context_update(player->render_ctx);
        g_mpv_update_flags |= flags;
    }
}

// Keyboard input event callback
void keyboard_event_callback(int fd, uint32_t events, void *user_data) {
    (void)events; // Unused
    (void)user_data; // Unused
    
    char c;
    if (read(fd, &c, 1) > 0) {

        
        // Debug numeric keys specifically
        if (c >= '1' && c <= '4') {
            LOG_INFO("Numeric key %c pressed for keystone corner selection", c);
        }
        
        // Special case for arrow keys (they come as escape sequences)
        static char seq[5] = {0};
        static int seq_pos = 0;
        
        if (c == 27) { // ESC character starts a sequence
            seq[0] = c;
            seq_pos = 1;
            return; // Wait for more characters
        } else if (seq_pos == 1 && c == '[') { // Second char in sequence
            seq[seq_pos++] = c;
            return; // Wait for final character
        } else if (seq_pos == 2) { // Third and final char in most sequences
            seq[seq_pos++] = c;
            seq[seq_pos] = '\0';
            LOG_INFO("Complete sequence received: ESC[%c (code: %d)", c, (int)c);
            
            // Handle arrow keys
            char arrow_key = 0;
            if (c == 'A') {        // Up arrow
                LOG_INFO("Up arrow detected");
                arrow_key = 65;
            } else if (c == 'B') { // Down arrow
                LOG_INFO("Down arrow detected");
                arrow_key = 66;
            } else if (c == 'C') { // Right arrow
                LOG_INFO("Right arrow detected");
                arrow_key = 67;
            } else if (c == 'D') { // Left arrow
                LOG_INFO("Left arrow detected");
                arrow_key = 68;
            }
            
            if (arrow_key != 0) {
                LOG_INFO("Sending arrow key code %d to keystone handler", (int)arrow_key);
                bool keystone_handled = keystone_handle_key(arrow_key);
                LOG_INFO("Arrow key handled by keystone: %s", keystone_handled ? "YES" : "NO");
                if (keystone_handled) {
                    g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
                }
                seq_pos = 0; // Reset sequence
                return;
            }
            
            seq_pos = 0; // Reset sequence for unknown keys
        }
        else if (seq_pos > 0) {
            // Part of a control sequence
            if (seq_pos < 4) {
                seq[seq_pos++] = c;
                seq[seq_pos] = '\0';
            }
            
            // Check for special keys
            if (seq_pos == 3 && seq[0] == 27 && seq[1] == '[') {
                // Check for numeric keypad with xterm
                if (seq[2] >= '1' && seq[2] <= '4' && c == '~') {
                    // Convert keypad number to regular number
                    char keypad_num = seq[2];
                    LOG_INFO("Numeric keypad key %c pressed", keypad_num);
                    bool keystone_handled = keystone_handle_key(keypad_num);
                    if (keystone_handled) {
                        g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
                    }
                    seq_pos = 0;
                    return;
                }
            }
            
            // Reset sequence if not handled
            if (seq_pos >= 3 || c != '[') {
                seq_pos = 0;
            }
        }
        
        // Special case: Force keystone mode with 'K'
        if (c == 'K') {
            LOG_INFO("Force enabling keystone mode with capital K");
            g_keystone.enabled = true;
            g_keystone.active_corner = 0;
            // Border remains hidden by default
            keystone_update_matrix();
            LOG_INFO("Keystone correction FORCE enabled, adjusting corner %d", 
                    g_keystone.active_corner + 1);
            fprintf(stderr, "\rKeystone correction FORCE enabled, use arrow keys to adjust corner %d", 
                   g_keystone.active_corner + 1);
            g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
            return;
        }
        
        // Help overlay
        if (c == 'h') {
            if (!g_help_visible) {
                show_help_overlay(((mpv_player_t *)user_data)->handle);
                g_help_visible = 1;
            } else {
                hide_help_overlay(((mpv_player_t *)user_data)->handle);
                g_help_visible = 0;
            }
            g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
            return;
        }
        
        // Handle keystone adjustment keys
        bool keystone_handled = keystone_handle_key(c);
        if (keystone_handled) {
            // Force a redraw when keystone parameters change
            g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
            return;
        }
        
        // If not handled by keystone, allow 'q' to quit
        if (c == 'q') {
            LOG_INFO("Quit requested by user");
            g_stop = 1;
        }
    }
}

// Joystick event callback
void joystick_event_callback(int fd, uint32_t events, void *user_data) {
    (void)events; // Unused
    (void)user_data; // Unused
    
    if (!is_joystick_enabled()) {
        return;
    }
    
    struct js_event event;
    while (read(fd, &event, sizeof(event)) > 0) {
        if (handle_joystick_event(&event)) {
            // Force a redraw when keystone parameters change
            g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
        }
    }
}

// V4L2 timer callback
void v4l2_timer_callback(int fd, uint32_t events, void *user_data) {
    (void)events; // Unused
    (void)user_data; // Unused
    
    // Read from the timer fd to reset it
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) < 0) {
        if (errno != EAGAIN) {
            LOG_ERROR("Error reading from timer fd: %s", strerror(errno));
        }
    }
    
    // Force a frame update for V4L2 decoder
    g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
}

// Signal event callback
void signal_event_callback(int fd, uint32_t events, void *user_data) {
    (void)events; // Unused
    (void)user_data; // Unused
    
    struct signalfd_siginfo si;
    if (read(fd, &si, sizeof(si)) != sizeof(si)) {
        LOG_ERROR("Error reading signal info: %s", strerror(errno));
        return;
    }
    
    LOG_INFO("Received signal %d", si.ssi_signo);
    
    // Handle signals
    switch (si.ssi_signo) {
        case SIGINT:
        case SIGTERM:
            LOG_INFO("Quit requested by signal %d", si.ssi_signo);
            g_stop = 1;
            break;
        case SIGUSR1:
            // Toggle help overlay
            g_help_toggle_request = 1;
            break;
        default:
            LOG_INFO("Unhandled signal %d", si.ssi_signo);
            break;
    }
}