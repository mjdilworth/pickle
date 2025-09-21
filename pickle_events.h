#ifndef PICKLE_EVENTS_H
#define PICKLE_EVENTS_H

#include "event.h"
#include "mpv.h"
#include "drm.h"
#include "v4l2_decoder.h"
#include "v4l2_player.h"

/**
 * @file pickle_events.h
 * @brief Integration of the event-driven architecture with Pickle
 * 
 * This file defines the functions to set up the event system for Pickle
 * and integrate it with the existing components.
 */

/**
 * Initialize the event system for Pickle
 * 
 * @param drm DRM context
 * @param player MPV player context
 * @param v4l2_player V4L2 player context (can be NULL if not using V4L2)
 * @return Event context, or NULL on failure
 */
event_ctx_t *pickle_event_init(kms_ctx_t *drm, mpv_player_t *player, v4l2_player_t *v4l2_player);

/**
 * Clean up the event system
 * 
 * @param ctx Event context
 */
void pickle_event_cleanup(event_ctx_t *ctx);

/**
 * Process events and render frames
 * 
 * @param ctx Event context
 * @param drm DRM context
 * @param egl EGL context
 * @param player MPV player context
 * @param v4l2_player V4L2 player context (can be NULL if not using V4L2)
 * @param timeout_ms Timeout in milliseconds, -1 for no timeout
 * @return true if successful, false on error
 */
bool pickle_event_process_and_render(event_ctx_t *ctx, kms_ctx_t *drm, egl_ctx_t *egl,
                                   mpv_player_t *player, v4l2_player_t *v4l2_player,
                                   int timeout_ms);

#endif /* PICKLE_EVENTS_H */