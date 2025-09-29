#ifndef V4L2_INTEGRATION_H
#define V4L2_INTEGRATION_H

#include "v4l2_demuxer.h"
#include "v4l2_demux_bridge.h"
#include "v4l2_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

// V4L2 integration context - combines demuxer, bridge, and decoder
typedef struct {
    v4l2_demuxer_t *demuxer;          // FFmpeg-based demuxer
    v4l2_demux_bridge_t *bridge;      // Packet bridge
    v4l2_decoder_t *decoder;          // V4L2 hardware decoder
    
    // Integration state
    bool is_initialized;
    bool is_playing;
    
    // Stream information
    v4l2_stream_config_t stream_config;
} v4l2_integration_t;

// Minimal integration API - for use in main application
v4l2_integration_t* v4l2_integration_create(void);
void v4l2_integration_destroy(v4l2_integration_t *integration);

bool v4l2_integration_open_file(v4l2_integration_t *integration, const char *file_path);
bool v4l2_integration_is_container_format(const char *file_path);

bool v4l2_integration_start_playback(v4l2_integration_t *integration);
void v4l2_integration_stop_playback(v4l2_integration_t *integration);

// Process packets (call regularly in main loop)
int v4l2_integration_process(v4l2_integration_t *integration);

// Check if integration is available and working
bool v4l2_integration_is_available(void);
bool v4l2_integration_has_packets(v4l2_integration_t *integration);

// Get stream information
const v4l2_stream_config_t* v4l2_integration_get_stream_config(v4l2_integration_t *integration);

// Error handling
void v4l2_integration_set_error_callback(v4l2_integration_t *integration,
                                          void (*callback)(const char *error, void *user_data),
                                          void *user_data);

#ifdef __cplusplus
}
#endif

#endif // V4L2_INTEGRATION_H