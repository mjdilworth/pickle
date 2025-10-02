#ifndef FFMPEG_V4L2_PLAYER_H
#define FFMPEG_V4L2_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
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
    AVPacket *packet;
    AVFrame *frame;
    int video_stream_index;
    
    // Video properties
    uint32_t width;
    uint32_t height;
    double fps;
    int64_t duration;
    
    // OpenGL texture for rendering
    GLuint y_texture;
    GLuint uv_texture;
    bool texture_valid;
    
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
    
    // File path
    char *file_path;
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

#endif // FFMPEG_V4L2_PLAYER_H
