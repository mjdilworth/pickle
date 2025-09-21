#ifndef PICKLE_MPV_H
#define PICKLE_MPV_H

#include <stdbool.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include "error.h"

// MPV context structure
typedef struct {
    mpv_handle *handle;
    mpv_render_context *render_ctx;
    bool initialized;
    bool loop_playback;
    char *hwdec_mode;
    char *video_file;
    void *proc_context;  // Context for get_proc_address function
} mpv_player_t;

// Initialize MPV player
pickle_result_t mpv_player_init(mpv_player_t *player, void *proc_ctx);

// Clean up MPV player
void mpv_player_cleanup(mpv_player_t *player);

// Set MPV option
pickle_result_t mpv_player_set_option_string(mpv_player_t *player, const char *name, const char *value);

// Set MPV flag option
pickle_result_t mpv_player_set_option_flag(mpv_player_t *player, const char *name, int value);

// Load and play a file
pickle_result_t mpv_player_load_file(mpv_player_t *player, const char *filename);

// Set hardware decoder mode
void mpv_player_set_hwdec(mpv_player_t *player, const char *hwdec);

// Set loop mode
void mpv_player_set_loop(mpv_player_t *player, bool loop);

// Process MPV events
bool mpv_player_process_events(mpv_player_t *player);

// Check if a new frame is available
bool mpv_player_has_frame(mpv_player_t *player);

// Get a readable string for MPV end-file reason
const char *mpv_player_end_reason_str(int reason);

// Check if MPV render context is valid
bool mpv_player_has_render_context(mpv_player_t *player);

#endif // PICKLE_MPV_H