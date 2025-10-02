#define _GNU_SOURCE
#include "ffmpeg_v4l2_player.h"
#include "pickle_globals.h"
#include "log.h"
#include "shader.h"
#include "keystone.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

// Declare external shader program (from pickle.c)
extern GLuint g_nv12_shader_program;

// Declare rendering functions
extern void render_keystone_quad(void);
extern void render_fullscreen_quad(void);

// Constants
#define MAX_PACKETS_PER_FRAME 100

/**
 * Get current time in microseconds
 */
static int64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Check if FFmpeg V4L2 M2M decoder is available
 */
bool ffmpeg_v4l2_is_supported(void) {
    // Check for h264_v4l2m2m codec
    const AVCodec *codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    if (codec) {
        LOG_INFO("FFmpeg h264_v4l2m2m codec is available");
        return true;
    }
    
    LOG_DEBUG("FFmpeg V4L2 M2M decoder not available");
    return false;
}

/**
 * Initialize FFmpeg V4L2 player
 */
bool init_ffmpeg_v4l2_player(ffmpeg_v4l2_player_t *player, const char *file) {
    if (!player || !file) {
        LOG_ERROR("Invalid player or file parameter");
        return false;
    }
    
    memset(player, 0, sizeof(ffmpeg_v4l2_player_t));
    
    // Store file path for potential reset/loop
    player->file_path = strdup(file);
    if (!player->file_path) {
        LOG_ERROR("Failed to allocate file path");
        return false;
    }
    
    // Open input file
    if (avformat_open_input(&player->format_ctx, file, NULL, NULL) < 0) {
        LOG_ERROR("Failed to open video file: %s", file);
        free(player->file_path);
        return false;
    }
    
    // Retrieve stream information
    if (avformat_find_stream_info(player->format_ctx, NULL) < 0) {
        LOG_ERROR("Failed to find stream information");
        avformat_close_input(&player->format_ctx);
        free(player->file_path);
        return false;
    }
    
    // Find video stream
    player->video_stream_index = -1;
    for (unsigned int i = 0; i < player->format_ctx->nb_streams; i++) {
        if (player->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            player->video_stream_index = (int)i;
            break;
        }
    }
    
    if (player->video_stream_index == -1) {
        LOG_ERROR("No video stream found in file");
        avformat_close_input(&player->format_ctx);
        free(player->file_path);
        return false;
    }
    
    AVStream *video_stream = player->format_ctx->streams[player->video_stream_index];
    AVCodecParameters *codecpar = video_stream->codecpar;
    
    // Get video properties
    player->width = (uint32_t)codecpar->width;
    player->height = (uint32_t)codecpar->height;
    player->duration = player->format_ctx->duration;
    
    // Calculate FPS
    if (video_stream->avg_frame_rate.den != 0) {
        player->fps = av_q2d(video_stream->avg_frame_rate);
    } else if (video_stream->r_frame_rate.den != 0) {
        player->fps = av_q2d(video_stream->r_frame_rate);
    } else {
        player->fps = 30.0; // Default fallback
    }
    
    LOG_INFO("Video: %ux%u @ %.2f fps", player->width, player->height, player->fps);
    
    // Find V4L2 M2M decoder based on codec
    const char *decoder_name = NULL;
    switch (codecpar->codec_id) {
        case AV_CODEC_ID_H264:
            decoder_name = "h264_v4l2m2m";
            break;
        case AV_CODEC_ID_HEVC:
            decoder_name = "hevc_v4l2m2m";
            break;
        case AV_CODEC_ID_MPEG2VIDEO:
            decoder_name = "mpeg2_v4l2m2m";
            break;
        case AV_CODEC_ID_VP8:
            decoder_name = "vp8_v4l2m2m";
            break;
        case AV_CODEC_ID_VP9:
            decoder_name = "vp9_v4l2m2m";
            break;
        default:
            LOG_ERROR("Unsupported codec ID: %d", codecpar->codec_id);
            avformat_close_input(&player->format_ctx);
            free(player->file_path);
            return false;
    }
    
    player->codec = avcodec_find_decoder_by_name(decoder_name);
    if (!player->codec) {
        LOG_ERROR("FFmpeg V4L2 M2M decoder '%s' not found", decoder_name);
        avformat_close_input(&player->format_ctx);
        free(player->file_path);
        return false;
    }
    
    LOG_INFO("Using FFmpeg decoder: %s", decoder_name);
    
    // Allocate codec context
    player->codec_ctx = avcodec_alloc_context3(player->codec);
    if (!player->codec_ctx) {
        LOG_ERROR("Failed to allocate codec context");
        avformat_close_input(&player->format_ctx);
        free(player->file_path);
        return false;
    }
    
    // Copy codec parameters to context
    if (avcodec_parameters_to_context(player->codec_ctx, codecpar) < 0) {
        LOG_ERROR("Failed to copy codec parameters");
        avcodec_free_context(&player->codec_ctx);
        avformat_close_input(&player->format_ctx);
        free(player->file_path);
        return false;
    }
    
    // Set thread count to 1 for V4L2 (hardware decoder)
    player->codec_ctx->thread_count = 1;
    
    // Try to use get_format callback for hardware acceleration
    // This might help with V4L2 M2M format negotiation
    player->codec_ctx->get_format = NULL;  // Let decoder choose
    
    LOG_INFO("Set codec context: threads=1, let V4L2 negotiate format");
    
    // Set some additional flags that might help
    player->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    player->codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    
    // Open codec (V4L2 M2M will negotiate pixel format during open)
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "num_capture_buffers", "16", 0);  // More buffers for V4L2
    
    int ret = avcodec_open2(player->codec_ctx, player->codec, &opts);
    av_dict_free(&opts);
    
    if (ret < 0) {
        LOG_ERROR("Failed to open codec: %s", av_err2str(ret));
        avcodec_free_context(&player->codec_ctx);
        avformat_close_input(&player->format_ctx);
        free(player->file_path);
        return false;
    }
    
    LOG_INFO("Codec opened successfully - pix_fmt=%d (%s), width=%d, height=%d",
             player->codec_ctx->pix_fmt, 
             av_get_pix_fmt_name(player->codec_ctx->pix_fmt),
             player->codec_ctx->width, player->codec_ctx->height);
    
    // Allocate packet and frame
    player->packet = av_packet_alloc();
    player->frame = av_frame_alloc();
    
    if (!player->packet || !player->frame) {
        LOG_ERROR("Failed to allocate packet or frame");
        av_packet_free(&player->packet);
        av_frame_free(&player->frame);
        avcodec_free_context(&player->codec_ctx);
        avformat_close_input(&player->format_ctx);
        free(player->file_path);
        return false;
    }
    
    // Allocate NV12 conversion buffer
    player->nv12_buffer_size = av_image_get_buffer_size(AV_PIX_FMT_NV12, 
                                                         (int)player->width, 
                                                         (int)player->height, 1);
    player->nv12_buffer = (uint8_t *)av_malloc(player->nv12_buffer_size);
    
    if (!player->nv12_buffer) {
        LOG_ERROR("Failed to allocate NV12 buffer");
        av_packet_free(&player->packet);
        av_frame_free(&player->frame);
        avcodec_free_context(&player->codec_ctx);
        avformat_close_input(&player->format_ctx);
        free(player->file_path);
        return false;
    }
    
    // Create OpenGL textures for NV12 format
    glGenTextures(1, &player->y_texture);
    glGenTextures(1, &player->uv_texture);
    
    // Setup Y texture (luminance)
    glBindTexture(GL_TEXTURE_2D, player->y_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (GLsizei)player->width, (GLsizei)player->height, 
                 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup UV texture (chrominance)
    glBindTexture(GL_TEXTURE_2D, player->uv_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, (GLsizei)player->width / 2, 
                 (GLsizei)player->height / 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Prime the V4L2 M2M decoder by sending some packets during init
    LOG_INFO("Priming V4L2 M2M decoder with initial packets...");
    int packets_sent = 0;
    for (int i = 0; i < 20; i++) {
        int ret = av_read_frame(player->format_ctx, player->packet);
        if (ret < 0) break;
        
        if (player->packet->stream_index == player->video_stream_index) {
            int send_ret = avcodec_send_packet(player->codec_ctx, player->packet);
            if (send_ret == 0) {
                packets_sent++;
            }
        }
        av_packet_unref(player->packet);
    }
    
    // Reset to beginning of file
    av_seek_frame(player->format_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
    
    LOG_INFO("V4L2 decoder primed with %d packets, ready for playback", packets_sent);
    
    player->initialized = true;
    LOG_INFO("FFmpeg V4L2 player initialized successfully");
    
    return true;
}

/**
 * Get next frame from decoder
 */
bool ffmpeg_v4l2_get_frame(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->initialized) {
        return false;
    }
    
    int64_t start_time = get_time_us();
    
    // Try to receive a frame first (might be buffered from previous calls)
    int ret = avcodec_receive_frame(player->codec_ctx, player->frame);
    if (ret == 0) {
        // Frame available
        int64_t decode_time = get_time_us() - start_time;
        player->decode_time_avg = (player->decode_time_avg * 0.9) + 
                                 ((double)decode_time / 1000.0 * 0.1);
        player->frames_decoded++;
        return true;
    }
    
    // If EAGAIN, we need to send more packets
    // For hardware decoders, send one packet and try again
    while (ret == AVERROR(EAGAIN)) {
        // Read next packet
        ret = av_read_frame(player->format_ctx, player->packet);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                player->eof_reached = true;
                
                // Send flush packet
                avcodec_send_packet(player->codec_ctx, NULL);
                
                // Try to get remaining frames
                ret = avcodec_receive_frame(player->codec_ctx, player->frame);
                if (ret == 0) {
                    int64_t decode_time = get_time_us() - start_time;
                    player->decode_time_avg = (player->decode_time_avg * 0.9) + 
                                             ((double)decode_time / 1000.0 * 0.1);
                    player->frames_decoded++;
                    return true;
                }
                return false;
            }
            LOG_ERROR("Error reading frame: %s", av_err2str(ret));
            return false;
        }
        
        // Skip non-video packets
        if (player->packet->stream_index != player->video_stream_index) {
            av_packet_unref(player->packet);
            continue;
        }
        
        // Send packet to decoder
        ret = avcodec_send_packet(player->codec_ctx, player->packet);
        av_packet_unref(player->packet);
        
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            LOG_ERROR("Error sending packet: %s", av_err2str(ret));
            return false;
        }
        
        // Try to receive frame again
        ret = avcodec_receive_frame(player->codec_ctx, player->frame);
        if (ret == 0) {
            int64_t decode_time = get_time_us() - start_time;
            player->decode_time_avg = (player->decode_time_avg * 0.9) + 
                                     ((double)decode_time / 1000.0 * 0.1);
            player->frames_decoded++;
            return true;
        }
    }
    
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        LOG_ERROR("Error receiving frame: %s", av_err2str(ret));
    }
    
    return false;
}

/**
 * Upload frame data to OpenGL textures (NV12 format)
 */
bool ffmpeg_v4l2_upload_to_gl(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->frame || !player->frame->data[0]) {
        return false;
    }
    
    AVFrame *frame = player->frame;
    
    // Upload Y plane
    glBindTexture(GL_TEXTURE_2D, player->y_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 
                    frame->width, frame->height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, 
                    frame->data[0]);
    
    // Upload UV plane (NV12 format has interleaved U and V)
    glBindTexture(GL_TEXTURE_2D, player->uv_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    frame->width / 2, frame->height / 2,
                    GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                    frame->data[1]);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    player->texture_valid = true;
    player->frames_rendered++;
    
    return true;
}

/**
 * Render frame with keystone correction
 */
bool render_ffmpeg_v4l2_frame(kms_ctx_t *d, egl_ctx_t *e, ffmpeg_v4l2_player_t *player) {
    if (!player || !player->texture_valid) {
        return false;
    }
    
    // Clear the framebuffer
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Use NV12 shader program
    glUseProgram(g_nv12_shader_program);
    
    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, player->y_texture);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, player->uv_texture);
    
    // Set texture uniforms
    GLint y_tex_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_y");
    GLint uv_tex_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_uv");
    
    if (y_tex_loc >= 0) glUniform1i(y_tex_loc, 0);
    if (uv_tex_loc >= 0) glUniform1i(uv_tex_loc, 1);
    
    // Apply keystone correction if enabled
    extern keystone_t g_keystone;
    if (g_keystone.enabled) {
        // TODO: Apply keystone transformation - for now render fullscreen
        float vertices[] = {
            -1.0f,  1.0f, 0.0f, 1.0f,  // Top-left
            -1.0f, -1.0f, 0.0f, 0.0f,  // Bottom-left
             1.0f,  1.0f, 1.0f, 1.0f,  // Top-right
             1.0f, -1.0f, 1.0f, 0.0f   // Bottom-right
        };
        
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices + 2);
        glEnableVertexAttribArray(1);
        
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        // Simple fullscreen quad
        float vertices[] = {
            -1.0f,  1.0f, 0.0f, 1.0f,  // Top-left
            -1.0f, -1.0f, 0.0f, 0.0f,  // Bottom-left
             1.0f,  1.0f, 1.0f, 1.0f,  // Top-right
             1.0f, -1.0f, 1.0f, 0.0f   // Bottom-right
        };
        
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices + 2);
        glEnableVertexAttribArray(1);
        
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    
    // Unbind
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    
    // Swap buffers to present the rendered frame
    eglSwapBuffers(e->dpy, e->surf);
    
    // Present the GBM buffer to screen with page flip
    if (!present_gbm_surface(d, e)) {
        LOG_ERROR("Failed to present GBM surface");
        return false;
    }
    
    return true;
}

/**
 * Seek to position in video
 */
bool ffmpeg_v4l2_seek(ffmpeg_v4l2_player_t *player, double timestamp) {
    if (!player || !player->initialized) {
        return false;
    }
    
    int64_t seek_target = (int64_t)(timestamp * AV_TIME_BASE);
    
    if (av_seek_frame(player->format_ctx, -1, seek_target, AVSEEK_FLAG_BACKWARD) < 0) {
        LOG_ERROR("Seek failed");
        return false;
    }
    
    avcodec_flush_buffers(player->codec_ctx);
    player->eof_reached = false;
    
    return true;
}

/**
 * Reset decoder for loop playback
 */
bool ffmpeg_v4l2_reset(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->initialized) {
        return false;
    }
    
    // Seek to beginning
    if (!ffmpeg_v4l2_seek(player, 0.0)) {
        LOG_ERROR("Failed to reset player");
        return false;
    }
    
    player->eof_reached = false;
    LOG_DEBUG("FFmpeg V4L2 player reset for loop");
    
    return true;
}

/**
 * Get player statistics
 */
void ffmpeg_v4l2_get_stats(ffmpeg_v4l2_player_t *player, 
                           uint64_t *frames_decoded,
                           uint64_t *frames_dropped,
                           double *avg_decode_time) {
    if (!player) {
        return;
    }
    
    if (frames_decoded) *frames_decoded = player->frames_decoded;
    if (frames_dropped) *frames_dropped = player->frames_dropped;
    if (avg_decode_time) *avg_decode_time = player->decode_time_avg;
}

/**
 * Cleanup and destroy player
 */
void cleanup_ffmpeg_v4l2_player(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->initialized) {
        return;
    }
    
    LOG_INFO("Cleaning up FFmpeg V4L2 player...");
    
    // Mark GL textures as invalid (EGL context destruction will clean them up)
    LOG_INFO("Invalidating GL textures...");
    player->y_texture = 0;
    player->uv_texture = 0;
    
    // Free FFmpeg resources first
    LOG_INFO("Freeing frame...");
    if (player->frame) {
        av_frame_free(&player->frame);
        // av_frame_free sets pointer to NULL
    }
    
    LOG_INFO("Freeing packet...");
    if (player->packet) {
        av_packet_free(&player->packet);
        // av_packet_free sets pointer to NULL
    }
    
    LOG_INFO("Freeing codec context...");
    if (player->codec_ctx) {
        avcodec_free_context(&player->codec_ctx);
        // avcodec_free_context sets pointer to NULL
    }

    LOG_INFO("Closing format context...");
    if (player->format_ctx) {
        avformat_close_input(&player->format_ctx);
        // avformat_close_input sets pointer to NULL
    }
    
    // Free NV12 buffer (allocated with av_malloc)
    LOG_INFO("Freeing NV12 buffer...");
    if (player->nv12_buffer) {
        av_free(player->nv12_buffer);
        player->nv12_buffer = NULL;
    }
    
    // Free file path (allocated with strdup)
    LOG_INFO("Freeing file path...");
    if (player->file_path) {
        free(player->file_path);
        player->file_path = NULL;
    }
    
    player->initialized = false;
    
    LOG_INFO("FFmpeg V4L2 player cleaned up");
}
