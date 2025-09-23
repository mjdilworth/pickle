#ifndef PICKLE_GLOBALS_H
#define PICKLE_GLOBALS_H

#include <stdbool.h>
#include <stdint.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <linux/joystick.h>
#include "drm.h"
#include "egl.h"
#include "v4l2_player.h"

// Global variables that need to be shared across modules
extern int g_debug;
extern volatile int g_stop;
extern volatile int g_mpv_wakeup;
extern volatile uint64_t g_mpv_update_flags;
extern int g_joystick_enabled;
extern int g_joystick_fd;
extern int g_help_visible;
extern int g_help_toggle_request;
extern int g_pending_flip;
extern uint64_t g_frames;
extern int g_use_v4l2_decoder;
extern int g_scanout_disabled;
extern int g_mpv_pipe[2];

// Functions that need to be shared across modules
void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data);
void drain_mpv_events(mpv_handle *h);
bool render_frame_mpv(mpv_handle *mpv, mpv_render_context *mpv_gl, kms_ctx_t *drm, egl_ctx_t *eglc);
bool render_v4l2_frame(kms_ctx_t *d, egl_ctx_t *e, v4l2_player_t *p);
void show_help_overlay(mpv_handle *mpv);
void hide_help_overlay(mpv_handle *mpv);
bool handle_joystick_event(struct js_event *event);

#endif /* PICKLE_GLOBALS_H */