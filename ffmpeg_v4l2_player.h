#ifndef FFMPEG_V4L2_PLAYER_H
#define FFMPEG_V4L2_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/rational.h>
#include <GLES3/gl31.h>
#include "drm.h"
#include "egl.h"

/**
 * FFmpeg V4L2 M2M Player
 * 
 * Uses FFmpeg's h264_v4l2m2m (and other V4L2 M2M codecs) for hardware-accelerated
 * video decoding on Raspberry Pi and similar platforms.
 */

// Player state structure
typedef struct {
    // FFmpeg contexts
    AVFormatContext *format_ctx;
    AVCodecContext *codec_ctx;
    const AVCodec *codec;
    AVBSFContext *bsf_ctx;
    AVBSFContext *bsf_ctx_filter_units;
    AVBSFContext *bsf_ctx_aud; // optional: h264_metadata with aud=insert
    AVCodecParserContext *parser_ctx;
    AVPacket *au_packet; // temporary packet used for parser output
    AVPacket *packet;
    AVFrame *frame;
    int video_stream_index;
    
    // Video properties
    uint32_t width;
    uint32_t height;
    double fps;
    int64_t duration;
    AVRational stream_time_base;
    int64_t frame_duration; // duration in stream_time_base units
    int64_t last_valid_pts;
    
    // OpenGL texture for rendering
    GLuint y_texture;
    GLuint uv_texture;
    bool texture_valid;
    
    // OpenGL rendering optimization
    GLuint vbo;              // Vertex buffer object
    GLuint vao;              // Vertex array object
    // Note: Uniform locations are global (g_nv12_u_texture_y_loc, g_nv12_u_texture_uv_loc)
    
    // NV12 conversion buffer
    uint8_t *nv12_buffer;
    size_t nv12_buffer_size;
    
    // Performance tracking
    uint64_t frames_decoded;
    uint64_t frames_rendered;
    uint64_t frames_dropped;
    double decode_time_avg;
    
    // State flags
    bool initialized;
    bool eof_reached;
    bool loop_enabled;
    bool avcc_extradata_converted;
    bool fatal_error;
    bool extradata_injected;
    bool use_annexb_bsf;
    bool use_filter_units_bsf;
    bool use_aud_bsf;
    int avcc_length_size;
    bool seen_keyframe; // true once we've encountered first keyframe packet
    bool seen_idr;      // true once we've observed IDR NAL (type 5) from BSF output
    
    // File path
    char *file_path;
    
    // Threading support
    bool use_threaded_decoding;
    void *decode_thread;
    bool thread_running;
    bool thread_stop_requested;
    
    // Frame queue for threaded decoding
    struct {
        AVFrame *frames[3];  // 3-frame queue buffer
        int write_idx;       // Producer index
        int read_idx;        // Consumer index
        int count;           // Number of frames in queue
        void *mutex;         // Mutex for thread safety
        void *cond;          // Condition variable for signaling
    } frame_queue;
} ffmpeg_v4l2_player_t;

/**
 * Check if FFmpeg V4L2 M2M decoder is available
 * 
 * @return true if h264_v4l2m2m codec is available
 */
bool ffmpeg_v4l2_is_supported(void);

/**
 * Initialize FFmpeg V4L2 player
 * 
 * @param player Pointer to player structure
 * @param file Path to video file
 * @return true on success, false on failure
 */
bool init_ffmpeg_v4l2_player(ffmpeg_v4l2_player_t *player, const char *file);

/**
 * Get next frame from decoder
 * 
 * @param player Pointer to player structure
 * @return true if frame is ready, false on error or EOF
 */
bool ffmpeg_v4l2_get_frame(ffmpeg_v4l2_player_t *player);

/**
 * Upload frame data to OpenGL textures (NV12 format)
 * 
 * @param player Pointer to player structure
 * @return true on success, false on failure
 */
bool ffmpeg_v4l2_upload_to_gl(ffmpeg_v4l2_player_t *player);

/**
 * Render frame with keystone correction
 * 
 * @param d DRM context
 * @param e EGL context
 * @param player FFmpeg V4L2 player
 * @return true on success, false on failure
 */
bool render_ffmpeg_v4l2_frame(kms_ctx_t *d, egl_ctx_t *e, ffmpeg_v4l2_player_t *player);

/**
 * Seek to position in video
 * 
 * @param player Pointer to player structure
 * @param timestamp Timestamp in seconds
 * @return true on success, false on failure
 */
bool ffmpeg_v4l2_seek(ffmpeg_v4l2_player_t *player, double timestamp);

/**
 * Reset decoder for loop playback
 * 
 * @param player Pointer to player structure
 * @return true on success, false on failure
 */
bool ffmpeg_v4l2_reset(ffmpeg_v4l2_player_t *player);

/**
 * Get player statistics
 * 
 * @param player Pointer to player structure
 * @param frames_decoded Output: total frames decoded
 * @param frames_dropped Output: total frames dropped
 * @param avg_decode_time Output: average decode time in ms
 */
void ffmpeg_v4l2_get_stats(ffmpeg_v4l2_player_t *player, 
                           uint64_t *frames_decoded,
                           uint64_t *frames_dropped,
                           double *avg_decode_time);

/**
 * Cleanup and destroy player
 * 
 * @param player Pointer to player structure
 */
void cleanup_ffmpeg_v4l2_player(ffmpeg_v4l2_player_t *player);

/**
 * Enable threaded decoding mode (must be called before init_ffmpeg_v4l2_player)
 *
 * @param player Pointer to player structure
 * @return true on success, false on failure
 */
bool ffmpeg_v4l2_enable_threaded_decoding(ffmpeg_v4l2_player_t *player);

/**
 * Start the decoding thread (automatically called by init_ffmpeg_v4l2_player if threaded mode is enabled)
 *
 * @param player Pointer to player structure
 * @return true on success, false on failure
 */
bool ffmpeg_v4l2_start_decode_thread(ffmpeg_v4l2_player_t *player);

/**
 * Stop the decoding thread (automatically called by cleanup_ffmpeg_v4l2_player)
 *
 * @param player Pointer to player structure
 * @return true on success, false on failure
 */
bool ffmpeg_v4l2_stop_decode_thread(ffmpeg_v4l2_player_t *player);

#endif // FFMPEG_V4L2_PLAYER_H
