#ifndef PICKLE_V4L2_DEMUXER_H
#define PICKLE_V4L2_DEMUXER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Forward declarations (always available for stubs)
typedef struct v4l2_demuxer v4l2_demuxer_t;

// Conditional compilation - only build demuxer when enabled
#if defined(USE_V4L2_DECODER) && defined(ENABLE_V4L2_DEMUXER)

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

// Demuxed packet structure
typedef struct {
    uint8_t *data;          // Packet data
    size_t size;            // Packet size in bytes
    int64_t pts;            // Presentation timestamp
    int64_t dts;            // Decode timestamp
    bool keyframe;          // Whether this is a keyframe
    int stream_index;       // Stream index in container
} v4l2_demuxed_packet_t;

// Callback for demuxed packets
typedef void (*v4l2_demuxed_packet_cb)(v4l2_demuxed_packet_t *packet, void *user_data);

// Stream information
typedef struct {
    int codec_id;           // AVCodecID from FFmpeg
    int width;              // Video width
    int height;             // Video height
    int64_t duration;       // Stream duration in microseconds
    double fps;             // Frames per second
    const char *codec_name; // Human readable codec name
} v4l2_stream_info_t;

// V4L2 demuxer context
struct v4l2_demuxer {
    // FFmpeg context
    AVFormatContext *fmt_ctx;
    AVCodecContext *codec_ctx;
    AVStream *video_stream;
    int video_stream_index;
    
    // State
    bool initialized;
    bool eof_reached;
    char *filename;
    
    // Stream info
    v4l2_stream_info_t stream_info;
    
    // Callback
    v4l2_demuxed_packet_cb packet_callback;
    void *user_data;
    
    // Threading support (optional)
    bool use_threading;
    void *thread_handle;
    bool thread_running;
    bool thread_stop_requested;
};

// Public API functions
#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check if V4L2 demuxer is available and supported
 * @return true if demuxer can be used, false otherwise
 */
bool v4l2_demuxer_is_available(void);

/**
 * Create and initialize a V4L2 demuxer
 * @param filename Path to video file
 * @param callback Callback for demuxed packets
 * @param user_data User data passed to callback
 * @return Demuxer instance or NULL on failure
 */
v4l2_demuxer_t* v4l2_demuxer_create(const char *filename, 
                                    v4l2_demuxed_packet_cb callback, 
                                    void *user_data);

/**
 * Get stream information
 * @param demuxer Demuxer instance
 * @return Stream info structure or NULL on failure
 */
const v4l2_stream_info_t* v4l2_demuxer_get_stream_info(v4l2_demuxer_t *demuxer);

/**
 * Start demuxing (blocking mode)
 * @param demuxer Demuxer instance
 * @return true on success, false on failure
 */
bool v4l2_demuxer_start_blocking(v4l2_demuxer_t *demuxer);

/**
 * Start demuxing (threaded mode)
 * @param demuxer Demuxer instance
 * @return true on success, false on failure
 */
bool v4l2_demuxer_start_threaded(v4l2_demuxer_t *demuxer);

/**
 * Stop demuxing (for threaded mode)
 * @param demuxer Demuxer instance
 */
void v4l2_demuxer_stop(v4l2_demuxer_t *demuxer);

/**
 * Process a single packet (for polling mode)
 * @param demuxer Demuxer instance
 * @return true if packet processed, false if EOF or error
 */
bool v4l2_demuxer_process_packet(v4l2_demuxer_t *demuxer);

/**
 * Seek to timestamp
 * @param demuxer Demuxer instance
 * @param timestamp_us Timestamp in microseconds
 * @return true on success, false on failure
 */
bool v4l2_demuxer_seek(v4l2_demuxer_t *demuxer, int64_t timestamp_us);

/**
 * Check if EOF reached
 * @param demuxer Demuxer instance
 * @return true if EOF reached
 */
bool v4l2_demuxer_is_eof(v4l2_demuxer_t *demuxer);

/**
 * Cleanup and destroy demuxer
 * @param demuxer Demuxer instance
 */
void v4l2_demuxer_destroy(v4l2_demuxer_t *demuxer);

#ifdef __cplusplus
}
#endif

#else
// Stub functions when demuxer is disabled
static inline bool v4l2_demuxer_is_available(void) { return false; }
static inline v4l2_demuxer_t* v4l2_demuxer_create(const char *filename, void *callback, void *user_data) { return NULL; }
static inline void v4l2_demuxer_destroy(v4l2_demuxer_t *demuxer) { (void)demuxer; }
#endif // USE_V4L2_DECODER && ENABLE_V4L2_DEMUXER

#endif // PICKLE_V4L2_DEMUXER_H