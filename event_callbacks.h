#ifndef EVENT_CALLBACKS_H
#define EVENT_CALLBACKS_H

#include "event.h"
#include "mpv.h"
#include "keystone.h"
#include "drm.h"
#include "v4l2_player.h"
#include "pickle_globals.h"
#include <linux/input.h>

/**
 * @file event_callbacks.h
 * @brief Callback functions for the event-driven architecture
 * 
 * This file defines the callback functions for various event sources
 * registered with the event system.
 */

/**
 * DRM event callback function
 * 
 * @param fd File descriptor
 * @param events Epoll events
 * @param user_data User data (kms_ctx_t pointer)
 */
void drm_event_callback(int fd, uint32_t events, void *user_data);

/**
 * MPV event callback function
 * 
 * @param fd File descriptor
 * @param events Epoll events
 * @param user_data User data (mpv_player_t pointer)
 */
void mpv_event_callback(int fd, uint32_t events, void *user_data);

/**
 * Keyboard input event callback function
 * 
 * @param fd File descriptor
 * @param events Epoll events
 * @param user_data User data (typically NULL)
 */
void keyboard_event_callback(int fd, uint32_t events, void *user_data);

/**
 * Joystick event callback function
 * 
 * @param fd File descriptor
 * @param events Epoll events
 * @param user_data User data (typically NULL)
 */
void joystick_event_callback(int fd, uint32_t events, void *user_data);

/**
 * V4L2 timer callback function
 * 
 * @param fd File descriptor
 * @param events Epoll events
 * @param user_data User data (v4l2_player_t pointer)
 */
void v4l2_timer_callback(int fd, uint32_t events, void *user_data);

/**
 * Signal event callback function
 * 
 * @param fd File descriptor
 * @param events Epoll events
 * @param user_data User data (typically NULL)
 */
void signal_event_callback(int fd, uint32_t events, void *user_data);

#endif /* EVENT_CALLBACKS_H */