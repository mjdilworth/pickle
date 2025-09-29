#ifndef PICKLE_V4L2_DEMUX_BRIDGE_H
#define PICKLE_V4L2_DEMUX_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include "v4l2_decoder.h"
#include "v4l2_demuxer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Packet callback function type (matches demuxer callback signature)
typedef v4l2_demuxed_packet_cb v4l2_packet_callback_t;

// Forward declaration
typedef struct v4l2_demux_bridge v4l2_demux_bridge_t;

// Stream configuration structure
typedef struct {
    v4l2_codec_t codec;          // V4L2 codec type
    uint32_t width;              // Video width
    uint32_t height;             // Video height  
    double fps;                  // Frame rate
    bool is_supported;           // Whether codec is supported by V4L2
} v4l2_stream_config_t;

// Statistics structure
typedef struct {
    uint64_t packets_received;   // Total packets received from demuxer
    uint64_t packets_decoded;    // Total packets sent to V4L2 decoder
    uint64_t packets_dropped;    // Total packets dropped due to queue full
    uint64_t decode_errors;      // Number of decode errors
    size_t queue_size;           // Current queue size
    size_t max_queue_size;       // Maximum queue size reached
} v4l2_bridge_stats_t;

/**
 * Create a V4L2 demuxer bridge
 * 
 * @param decoder V4L2 decoder instance
 * @param max_queue_size Maximum number of packets to queue
 * @return Bridge instance or NULL on failure
 */
v4l2_demux_bridge_t* v4l2_demux_bridge_create(v4l2_decoder_t *decoder, size_t max_queue_size);

/**
 * Destroy the V4L2 demuxer bridge
 * 
 * @param bridge Bridge instance
 */
void v4l2_demux_bridge_destroy(v4l2_demux_bridge_t *bridge);

/**
 * Configure stream parameters from demuxer info
 * 
 * @param bridge Bridge instance
 * @param stream_info Stream information from demuxer
 * @return Stream configuration or NULL on failure
 */
v4l2_stream_config_t* v4l2_demux_bridge_configure_stream(v4l2_demux_bridge_t *bridge, 
                                                          const v4l2_stream_info_t *stream_info);

/**
 * Get packet callback function for demuxer
 * 
 * @param bridge Bridge instance
 * @return Callback function pointer
 */
v4l2_packet_callback_t v4l2_demux_bridge_get_callback(v4l2_demux_bridge_t *bridge);

/**
 * Get user data pointer for demuxer callback
 * 
 * @param bridge Bridge instance  
 * @return User data pointer (the bridge instance)
 */
void* v4l2_demux_bridge_get_user_data(v4l2_demux_bridge_t *bridge);

/**
 * Process queued packets (call from main thread)
 * 
 * @param bridge Bridge instance
 * @param max_packets Maximum packets to process per call (0 = unlimited)
 * @return Number of packets processed
 */
int v4l2_demux_bridge_process_packets(v4l2_demux_bridge_t *bridge, int max_packets);

/**
 * Check if bridge has packets ready to process
 * 
 * @param bridge Bridge instance
 * @return true if packets are queued
 */
bool v4l2_demux_bridge_has_packets(v4l2_demux_bridge_t *bridge);

/**
 * Flush all queued packets
 * 
 * @param bridge Bridge instance
 */
void v4l2_demux_bridge_flush(v4l2_demux_bridge_t *bridge);

/**
 * Get bridge statistics
 * 
 * @param bridge Bridge instance
 * @param stats Output statistics structure
 */
void v4l2_demux_bridge_get_stats(v4l2_demux_bridge_t *bridge, v4l2_bridge_stats_t *stats);

/**
 * Set error callback for bridge errors
 * 
 * @param bridge Bridge instance
 * @param callback Error callback function (NULL to disable)
 * @param user_data User data for error callback
 */
void v4l2_demux_bridge_set_error_callback(v4l2_demux_bridge_t *bridge,
                                           void (*callback)(const char *error, void *user_data),
                                           void *user_data);

/**
 * Map FFmpeg codec ID to V4L2 codec type
 * 
 * @param codec_id FFmpeg codec ID (from AVCodecID)
 * @return V4L2 codec type or V4L2_CODEC_UNKNOWN if not supported
 */
v4l2_codec_t v4l2_demux_bridge_map_codec(int codec_id);

/**
 * Check if codec is supported by V4L2 hardware
 * 
 * @param codec_id FFmpeg codec ID
 * @return true if supported
 */
bool v4l2_demux_bridge_is_codec_supported(int codec_id);

#ifdef __cplusplus
}
#endif

#endif // PICKLE_V4L2_DEMUX_BRIDGE_H