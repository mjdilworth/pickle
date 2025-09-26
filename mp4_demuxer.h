#ifndef PICKLE_MP4_DEMUXER_H
#define PICKLE_MP4_DEMUXER_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @file mp4_demuxer.h 
 * @brief MP4 container demuxer for V4L2 decoder integration
 */

// MP4 packet structure
typedef struct {
    uint8_t *data;           // Packet data
    size_t size;             // Packet size
    int64_t pts;             // Presentation timestamp
    int64_t dts;             // Decode timestamp
    bool is_keyframe;        // Is this packet a keyframe
} mp4_packet_t;

// MP4 demuxer context
typedef struct mp4_demuxer {
    FILE *file;              // Input file handle
    uint32_t codec_id;       // Codec identifier (FFmpeg style)
    double fps;              // Frame rate
    bool eof_reached;        // End of file reached
    
    // Private implementation details
    void *priv;              // Private context (could be libavformat context)
} mp4_demuxer_t;

/**
 * Initialize MP4 demuxer
 * @param demuxer Demuxer context to initialize
 * @param filename Input MP4 file path
 * @return true on success, false on failure
 */
bool mp4_demuxer_init(mp4_demuxer_t *demuxer, const char *filename);

/**
 * Get stream information from MP4
 * @param demuxer Demuxer context
 * @param width Output video width
 * @param height Output video height
 * @param fps Output frame rate
 * @param codec_name Output codec name
 * @return true on success, false on failure
 */
bool mp4_demuxer_get_stream_info(mp4_demuxer_t *demuxer, uint32_t *width, uint32_t *height, 
                                  double *fps, const char **codec_name);

/**
 * Check if codec is supported by V4L2 decoder
 * @param demuxer Demuxer context
 * @return true if codec is supported, false otherwise
 */
bool mp4_demuxer_is_codec_supported(mp4_demuxer_t *demuxer);

/**
 * Get next packet from MP4 stream
 * @param demuxer Demuxer context
 * @param packet Output packet structure
 * @return true if packet retrieved, false on EOF or error
 */
bool mp4_demuxer_get_packet(mp4_demuxer_t *demuxer, mp4_packet_t *packet);

/**
 * Free packet data
 * @param packet Packet to free
 */
void mp4_demuxer_free_packet(mp4_packet_t *packet);

/**
 * Clean up demuxer resources
 * @param demuxer Demuxer context to clean up
 */
void mp4_demuxer_cleanup(mp4_demuxer_t *demuxer);

#endif /* PICKLE_MP4_DEMUXER_H */