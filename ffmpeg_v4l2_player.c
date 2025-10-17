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
#include <limits.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/time.h>
#include <libavutil/common.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
#include <libavutil/hwcontext.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>

static void ffmpeg_log_callback(void *ptr, int level, const char *fmt, va_list vl) {
    if (!g_debug && level > AV_LOG_WARNING) {
        return;
    }

    char message[1024];
    vsnprintf(message, sizeof(message), fmt, vl);

    // Trim trailing newline for cleaner output
    size_t len = strlen(message);
    if (len > 0 && message[len - 1] == '\n') {
        message[len - 1] = '\0';
    }

    if (level <= AV_LOG_ERROR) {
        LOG_ERROR("[FFmpeg] %s", message);
    } else if (level <= AV_LOG_WARNING) {
        LOG_WARN("[FFmpeg] %s", message);
    } else if (level <= AV_LOG_INFO) {
        LOG_INFO("[FFmpeg] %s", message);
    } else {
        LOG_DEBUG("[FFmpeg] %s", message);
    }
}

static void ffmpeg_configure_logging(void) {
    static bool callback_registered = false;

    if (!callback_registered) {
        av_log_set_callback(ffmpeg_log_callback);
        callback_registered = true;
    }

    av_log_set_level(g_debug ? AV_LOG_TRACE : AV_LOG_WARNING);
}

// Preferred pixel formats (ordered)
static enum AVPixelFormat v4l2_choose_format(const enum AVPixelFormat *pix_fmts) {
    // Order of preference: NV12 (easy path) -> DRM_PRIME (hw frames) -> YUV420P -> first available
    bool has_first = false;
    enum AVPixelFormat first = AV_PIX_FMT_NONE;
    for (int i = 0; pix_fmts && pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        const enum AVPixelFormat f = pix_fmts[i];
        if (!has_first) { first = f; has_first = true; }
        if (f == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
        if (f == AV_PIX_FMT_DRM_PRIME) return AV_PIX_FMT_DRM_PRIME;
        if (f == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    }
    return has_first ? first : AV_PIX_FMT_NONE;
}

static enum AVPixelFormat v4l2_get_format(struct AVCodecContext *s, const enum AVPixelFormat *pix_fmts) {
    if (g_debug) {
        LOG_DEBUG("[V4L2] get_format candidates:");
        for (int i = 0; pix_fmts && pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
            const char *name = av_get_pix_fmt_name(pix_fmts[i]);
            LOG_DEBUG("[V4L2]   %s (%d)", name ? name : "unknown", pix_fmts[i]);
        }
    }
    enum AVPixelFormat chosen = v4l2_choose_format(pix_fmts);
    const char *chosen_name = av_get_pix_fmt_name(chosen);
    LOG_INFO("[V4L2] Chosen pixel format: %s (%d)", chosen_name ? chosen_name : "unknown", chosen);
    return chosen;
}

// --- Debug helpers ---
static void debug_dump_packet_prefix(const char *tag, const AVPacket *pkt, int limit) {
    if (!g_debug || !pkt || !pkt->data || pkt->size <= 0) return;
    const int bytes_to_dump = pkt->size < limit ? pkt->size : limit;
    // Attempt to identify first NAL after a 4-byte start code
    int start_code_offset = -1;
    for (int i = 0; i + 3 < bytes_to_dump; i++) {
        if (pkt->data[i] == 0x00 && pkt->data[i+1] == 0x00 && pkt->data[i+2] == 0x00 && pkt->data[i+3] == 0x01) {
            start_code_offset = i;
            break;
        }
    }
    int nal = -1;
    if (start_code_offset >= 0 && start_code_offset + 4 < pkt->size) {
        nal = pkt->data[start_code_offset + 4] & 0x1F; // H.264 NAL
    }
    LOG_DEBUG("[V4L2] %s: pkt size=%d, flags=0x%x, first N bytes:", tag, pkt->size, pkt->flags);
    for (int i = 0; i < bytes_to_dump; i += 16) {
        char hexbuf[3*16 + 1] = {0};
        char asciibuf[17] = {0};
        int off = 0;
        for (int j = 0; j < 16 && (i + j) < bytes_to_dump; j++) {
            uint8_t b = pkt->data[i + j];
            off += snprintf(hexbuf + off, sizeof(hexbuf) - (size_t)off, "%02X ", b);
            asciibuf[j] = (b >= 32 && b < 127) ? (char)b : '.';
        }
        LOG_DEBUG("[V4L2] %04X: %-48s  %s", i, hexbuf, asciibuf);
    }
    if (nal >= 0) {
        const char *t = "other";
        switch (nal) { case 1: t = "non-IDR"; break; case 5: t = "IDR"; break; case 6: t = "SEI"; break; case 7: t = "SPS"; break; case 8: t = "PPS"; break; case 9: t = "AUD"; break; }
        LOG_DEBUG("[V4L2] %s: first NAL=%d (%s)%s", tag, nal, t, (pkt->flags & AV_PKT_FLAG_KEY) ? ", keyframe" : "");
    }
}

// Scan Annex-B NAL unit types in a buffer. Returns count of types written (up to max_out).
static int scan_nal_types(const uint8_t *data, int size, int max_out, int *out_types) {
    if (!data || size <= 0 || max_out <= 0 || !out_types) return 0;
    int found = 0;
    for (int i = 0; i + 4 < size; ) {
        // Find 0x000001 or 0x00000001
        int sc = -1;
        if (i + 3 < size && data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01) {
            sc = 3;
        } else if (i + 4 < size && data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
            sc = 4;
        }
        if (sc < 0) { i++; continue; }
        int nal_index = i + sc;
        if (nal_index < size) {
            int nal_type = data[nal_index] & 0x1F; // H.264 NAL
            if (found < max_out) out_types[found] = nal_type;
            found++;
        }
        // Advance to next start code
        i = nal_index + 1;
    }
    return found > max_out ? max_out : found;
}

// (removed duplicate scan_nal_types definition)


// --- Memory tracking helpers ---

static void log_memory_usage(const char *context) {
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) return;
    
    char line[256];
    long vm_size = 0, vm_rss = 0, vm_peak = 0;
    
    while (fgets(line, sizeof(line), status)) {
        if (sscanf(line, "VmSize: %ld kB", &vm_size) == 1) continue;
        if (sscanf(line, "VmRSS: %ld kB", &vm_rss) == 1) continue;
        if (sscanf(line, "VmPeak: %ld kB", &vm_peak) == 1) continue;
    }
    fclose(status);
    
    LOG_INFO("[MEMORY] %s: VmSize=%ld MB, VmRSS=%ld MB, VmPeak=%ld MB",
             context, vm_size/1024, vm_rss/1024, vm_peak/1024);
}

// --- avcC to Annex-B conversion helpers ---

static int get_avcc_length_size(const uint8_t *extradata, size_t extradata_size) {
    if (!extradata || extradata_size < 5) {
        return 0;
    }
    return (extradata[4] & 0x03) + 1;
}

static int avcc_extradata_to_annexb(const uint8_t *extradata,
                                    size_t extradata_size,
                                    uint8_t **annexb_data,
                                    size_t *annexb_size,
                                    int *nal_length_size_out) {
    if (!extradata || !annexb_data || !annexb_size || extradata_size < 7) {
        return AVERROR(EINVAL);
    }

    int nal_length_size = get_avcc_length_size(extradata, extradata_size);
    if (nal_length_size < 1 || nal_length_size > 4) {
        return AVERROR_INVALIDDATA;
    }

    const uint8_t *ptr = extradata + 5;
    size_t remaining = extradata_size - 5;
    if (remaining < 1) {
        return AVERROR_INVALIDDATA;
    }

    int num_sps = (*ptr) & 0x1f;
    ptr++;
    remaining--;

    size_t total_size = 0;

    for (int i = 0; i < num_sps; ++i) {
        if (remaining < 2) {
            return AVERROR_INVALIDDATA;
        }
        uint16_t nal_size = (uint16_t)(ptr[0] << 8 | ptr[1]);
        ptr += 2;
        remaining -= 2;
        if (remaining < nal_size) {
            return AVERROR_INVALIDDATA;
        }
        total_size += 4 + nal_size;
        ptr += nal_size;
        remaining -= nal_size;
    }

    if (remaining < 1) {
        return AVERROR_INVALIDDATA;
    }

    int num_pps = *ptr;
    ptr++;
    remaining--;

    for (int i = 0; i < num_pps; ++i) {
        if (remaining < 2) {
            return AVERROR_INVALIDDATA;
        }
        uint16_t nal_size = (uint16_t)(ptr[0] << 8 | ptr[1]);
        ptr += 2;
        remaining -= 2;
        if (remaining < nal_size) {
            return AVERROR_INVALIDDATA;
        }
        total_size += 4 + nal_size;
        ptr += nal_size;
        remaining -= nal_size;
    }

    if (total_size == 0) {
        return AVERROR_INVALIDDATA;
    }

    uint8_t *out = av_malloc(total_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!out) {
        return AVERROR(ENOMEM);
    }

    ptr = extradata + 5;
    remaining = extradata_size - 5;
    uint8_t *dst = out;

    num_sps = (*ptr) & 0x1f;
    ptr++;
    remaining--;

    for (int i = 0; i < num_sps; ++i) {
        uint16_t nal_size = (uint16_t)(ptr[0] << 8 | ptr[1]);
        ptr += 2;
        remaining -= 2;
        memcpy(dst, "\x00\x00\x00\x01", 4);
        dst += 4;
        memcpy(dst, ptr, nal_size);
        dst += nal_size;
        ptr += nal_size;
        remaining -= nal_size;
    }

    int pps_count = (remaining > 0) ? *ptr : 0;
    if (remaining > 0) {
        ptr++;
        remaining--;
    }

    for (int i = 0; i < pps_count; ++i) {
        uint16_t nal_size = (uint16_t)(ptr[0] << 8 | ptr[1]);
        ptr += 2;
        remaining -= 2;
        memcpy(dst, "\x00\x00\x00\x01", 4);
        dst += 4;
        memcpy(dst, ptr, nal_size);
        dst += nal_size;
        ptr += nal_size;
        remaining -= nal_size;
    }

    memset(out + total_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    *annexb_data = out;
    *annexb_size = total_size;
    if (nal_length_size_out) {
        *nal_length_size_out = nal_length_size;
    }

    return 0;
}

/**
 * Convert avcC packet to Annex-B format in-place
 * Based on reference implementation for better reliability
 */
static int convert_avcc_to_annexb_inplace(uint8_t *buf, int size, int length_size) {
    if (!buf || size <= length_size || length_size < 1 || length_size > 4) {
        return -1;
    }
    
    uint8_t *p = buf;
    uint8_t *end = p + size;
    uint8_t *dst = p;
    
    while (p + length_size <= end) {
        uint32_t nal_size = 0;
        for (int i = 0; i < length_size; i++) {
            nal_size = (nal_size << 8) | p[i];
        }
        
        if (nal_size == 0 || p + length_size + nal_size > end) {
            // Invalid NAL size
            return -1;
        }
        
        // Replace length prefix with start code
        dst[0] = 0;
        dst[1] = 0;
        dst[2] = 0;
        dst[3] = 1;
        
        // If source and destination overlap, use memmove
        if (p + length_size != dst + 4) {
            memmove(dst + 4, p + length_size, nal_size);
        }
        
        p += length_size + nal_size;
        dst += 4 + nal_size;
    }
    
    // Return new size
    return (int)(dst - buf);
}

static int convert_sample_avcc_to_annexb(AVPacket *packet,
                                         int nal_length_size,
                                         const uint8_t *annexb_extradata,
                                         size_t annexb_extradata_size,
                                         bool is_keyframe) {
    if (!packet || !packet->data || packet->size <= 0 || nal_length_size <= 0) {
        return AVERROR(EINVAL);
    }

    // First we'll convert the packet data in place
    int new_size = convert_avcc_to_annexb_inplace(packet->data, packet->size, nal_length_size);
    if (new_size < 0) {
        return AVERROR_INVALIDDATA;
    }
    
    // Handle SPS/PPS prefixing for keyframes if needed
    if (is_keyframe && annexb_extradata && annexb_extradata_size > 0) {
        // For keyframes, we need to allocate a new buffer with extradata + converted data
        uint8_t *new_data = av_malloc(annexb_extradata_size + new_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!new_data) {
            return AVERROR(ENOMEM);
        }
        
        // Copy extradata and converted data
        memcpy(new_data, annexb_extradata, annexb_extradata_size);
        memcpy(new_data + annexb_extradata_size, packet->data, new_size);
        memset(new_data + annexb_extradata_size + new_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        
        // Save packet metadata
        int64_t pts = packet->pts;
        int64_t dts = packet->dts;
        int duration = (int)FFMIN(packet->duration, (int64_t)INT_MAX);
        int64_t pos = packet->pos;
        int flags = packet->flags;
        
        // Free old packet data
        av_packet_unref(packet);
        
        // Create new packet from the buffer
        int ret = av_packet_from_data(packet, new_data, (int)(annexb_extradata_size + new_size));
        if (ret < 0) {
            av_free(new_data);
            return ret;
        }
        
        // Restore packet metadata
        packet->pts = pts;
        packet->dts = dts;
        packet->duration = duration;
        packet->pos = pos;
        packet->flags = flags;
    } else {
        // Update packet size to reflect the in-place conversion
        packet->size = new_size;
    }
    
    return 0;
}

/**
 * Switch from hardware to software decoder
 * Based on reference implementation's fallback approach
 */
static bool switch_to_software_decoder(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->format_ctx || player->video_stream_index < 0) {
        LOG_ERROR("Invalid player state for software fallback");
        return false;
    }

    LOG_INFO("Attempting to switch to software decoder");
    log_memory_usage("Before switching to software decoder");

    // Get codec parameters from the video stream
    AVCodecParameters *codecpar = player->format_ctx->streams[player->video_stream_index]->codecpar;

    // Close the current (hardware) codec context
    if (player->codec_ctx) {
        avcodec_flush_buffers(player->codec_ctx);
        avcodec_free_context(&player->codec_ctx);
        player->codec_ctx = NULL;
    }

    // We won't use bitstream filters with the software decoder
    if (player->bsf_ctx) {
        av_bsf_flush(player->bsf_ctx);
        av_bsf_free(&player->bsf_ctx);
        player->bsf_ctx = NULL;
        player->use_annexb_bsf = false;
    }
    if (player->bsf_ctx_aud) {
        av_bsf_flush(player->bsf_ctx_aud);
        av_bsf_free(&player->bsf_ctx_aud);
        player->bsf_ctx_aud = NULL;
        player->use_aud_bsf = false;
    }

    // Find a software decoder for the same codec id
    const AVCodec *sw_codec = NULL;
    // Prefer explicit software decoder names for known codecs where applicable
    if (codecpar->codec_id == AV_CODEC_ID_H264) {
        sw_codec = avcodec_find_decoder_by_name("h264");
    }
    if (!sw_codec) {
        sw_codec = avcodec_find_decoder(codecpar->codec_id);
    }
    if (!sw_codec) {
        LOG_ERROR("No suitable software decoder found for codec_id=%d", codecpar->codec_id);
        player->fatal_error = true;
        return false;
    }

    LOG_INFO("Found software decoder: %s", sw_codec->name);

    // Allocate new codec context for software decoding
    player->codec_ctx = avcodec_alloc_context3(sw_codec);
    if (!player->codec_ctx) {
        LOG_ERROR("Failed to allocate software codec context");
        player->fatal_error = true;
        return false;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(player->codec_ctx, codecpar) < 0) {
        LOG_ERROR("Failed to copy codec parameters for software decoder");
        avcodec_free_context(&player->codec_ctx);
        player->fatal_error = true;
        return false;
    }

    // Configure software decoder
    player->codec_ctx->thread_count = 4; // Use multiple threads for software decoding
    player->codec_ctx->thread_type = FF_THREAD_FRAME; // Frame-level threading
    player->codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY; // Minimize latency

    // Open software codec
    if (avcodec_open2(player->codec_ctx, sw_codec, NULL) < 0) {
        LOG_ERROR("Failed to open software codec");
        avcodec_free_context(&player->codec_ctx);
        player->fatal_error = true;
        return false;
    }

    LOG_INFO("Successfully switched to software decoder: %s", sw_codec->name);

    // Try to rewind to the beginning to decode from a clean point
    if (av_seek_frame(player->format_ctx, player->video_stream_index, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        LOG_WARN("Failed to seek back to start of stream for software decoder");
        // Continue anyway
    }

    // Flush buffers to ensure clean state
    avcodec_flush_buffers(player->codec_ctx);

    // Reset flags and state appropriate for software decoding
    player->fatal_error = false;
    player->extradata_injected = true; // not needed for software

    return true;
}

// Declare external shader program (from pickle.c)
extern GLuint g_nv12_shader_program;

// Declare rendering functions
extern void render_keystone_quad(void);
extern void render_fullscreen_quad(void);

// Constants

/**
 * Get current time in microseconds
 */
static int64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/**
 * Check if FFmpeg V4L2 M2M decoder is available and suitable for a specific file
 */
bool ffmpeg_v4l2_is_supported(void) {
    // Check for h264_v4l2m2m codec
    const AVCodec *codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
    if (!codec) {
        LOG_DEBUG("FFmpeg V4L2 M2M decoder not available");
        return false;
    }
    
    LOG_INFO("FFmpeg h264_v4l2m2m codec is available");
    
    return true;
}

/**
 * Initialize FFmpeg V4L2 player
 */
bool init_ffmpeg_v4l2_player(ffmpeg_v4l2_player_t *player, const char *file) {
    if (!player || !file) {
        LOG_ERROR("Invalid player or file parameter");
        return false;
    }

    ffmpeg_configure_logging();
    
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
    
    LOG_DEBUG("[V4L2] Found video stream at index %d", player->video_stream_index);
    
    AVStream *video_stream = player->format_ctx->streams[player->video_stream_index];
    AVCodecParameters *codecpar = video_stream->codecpar;
    
    // Get video properties
    player->width = (uint32_t)codecpar->width;
    player->height = (uint32_t)codecpar->height;
    player->duration = player->format_ctx->duration;
    player->bsf_ctx = NULL;
    player->bsf_ctx_aud = NULL;
    player->use_annexb_bsf = false;
    player->use_aud_bsf = false;
    player->seen_idr = false;
    
    // Calculate FPS
    if (video_stream->avg_frame_rate.den != 0) {
        player->fps = av_q2d(video_stream->avg_frame_rate);
    } else if (video_stream->r_frame_rate.den != 0) {
        player->fps = av_q2d(video_stream->r_frame_rate);
    } else {
        player->fps = 30.0; // Default fallback
    }
    
    LOG_INFO("Video: %ux%u @ %.2f fps", player->width, player->height, player->fps);

    const bool needs_avcc_conversion =
        (codecpar->codec_id == AV_CODEC_ID_H264 &&
         codecpar->extradata && codecpar->extradata_size > 0 &&
         codecpar->extradata[0] == 1);

    int avcc_length_size_hint = 0;
    if (needs_avcc_conversion) {
        avcc_length_size_hint = get_avcc_length_size(codecpar->extradata,
                                                     (size_t)codecpar->extradata_size);
        if (avcc_length_size_hint < 1 || avcc_length_size_hint > 4) {
            avcc_length_size_hint = 0;
        }
    }

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

    // Clear codec_tag so V4L2 driver isn't forced to interpret non-native tags (e.g. 'avc1')
    player->codec_ctx->codec_tag = 0;

    // Prefer FFmpeg bitstream filter for Annex-B conversion when available
    if (needs_avcc_conversion) {
        const AVBitStreamFilter *annexb_bsf = av_bsf_get_by_name("h264_mp4toannexb");
        if (annexb_bsf) {
            int bsf_ret = av_bsf_alloc(annexb_bsf, &player->bsf_ctx);
            if (bsf_ret == 0 && player->bsf_ctx) {
                LOG_DEBUG("[BSF] Stream codecpar: codec_id=%d, extradata_size=%d, codec_tag=0x%x", codecpar->codec_id, codecpar->extradata_size, codecpar->codec_tag);
                bsf_ret = avcodec_parameters_copy(player->bsf_ctx->par_in, codecpar);
                if (bsf_ret == 0) {
                    player->bsf_ctx->time_base_in = video_stream->time_base;
                    LOG_DEBUG("[BSF] par_in set: codec_id=%d, extradata_size=%d, time_base=%d/%d", player->bsf_ctx->par_in->codec_id, player->bsf_ctx->par_in->extradata_size, player->bsf_ctx->time_base_in.num, player->bsf_ctx->time_base_in.den);
                    if (player->bsf_ctx->par_in->extradata && player->bsf_ctx->par_in->extradata_size > 0) {
                        LOG_DEBUG("[BSF] par_in extradata first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X", 
                            player->bsf_ctx->par_in->extradata[0], player->bsf_ctx->par_in->extradata[1], player->bsf_ctx->par_in->extradata[2], player->bsf_ctx->par_in->extradata[3],
                            player->bsf_ctx->par_in->extradata[4], player->bsf_ctx->par_in->extradata[5], player->bsf_ctx->par_in->extradata[6], player->bsf_ctx->par_in->extradata[7]);
                    } else {
                        LOG_DEBUG("[BSF] par_in has no extradata");
                    }
                    bsf_ret = av_bsf_init(player->bsf_ctx);
                    if (bsf_ret == 0) {
                        player->use_annexb_bsf = true;
                        player->extradata_injected = true; // bitstream filter injects SPS/PPS as needed
                        player->avcc_length_size = 0;
                        player->avcc_extradata_converted = false;
                        LOG_INFO("Using FFmpeg h264_mp4toannexb bitstream filter for Annex-B conversion");

                        // When using Annex-B BSF, clear codec extradata to avoid passing MP4 avcC
                        // to the hw decoder. SPS/PPS will be provided in-band by the BSF.
                        if (player->codec_ctx->extradata) {
                            av_freep(&player->codec_ctx->extradata);
                            player->codec_ctx->extradata_size = 0;
                        }

                        // Chain an AUD inserter after mp4->annexb: use h264_metadata with aud=insert
                        const AVBitStreamFilter *aud_bsf = av_bsf_get_by_name("h264_metadata");
                        if (aud_bsf) {
                            int aud_ret = av_bsf_alloc(aud_bsf, &player->bsf_ctx_aud);
                            if (aud_ret == 0 && player->bsf_ctx_aud) {
                                aud_ret = avcodec_parameters_copy(player->bsf_ctx_aud->par_in, player->bsf_ctx->par_out);
                                if (aud_ret == 0) {
                                    player->bsf_ctx_aud->time_base_in = player->bsf_ctx->time_base_out;
                                    // Set option aud=insert
                                    av_opt_set(player->bsf_ctx_aud->priv_data, "aud", "insert", 0);
                                    aud_ret = av_bsf_init(player->bsf_ctx_aud);
                                    if (aud_ret == 0) {
                                        player->use_aud_bsf = true;
                                        LOG_INFO("Chained h264_metadata bitstream filter with aud=insert");
                                    } else {
                                        LOG_WARN("Failed to initialize h264_metadata (aud insert): %s", av_err2str(aud_ret));
                                        av_bsf_free(&player->bsf_ctx_aud);
                                    }
                                } else {
                                    LOG_WARN("Failed to copy codec parameters to AUD BSF: %s", av_err2str(aud_ret));
                                    av_bsf_free(&player->bsf_ctx_aud);
                                }
                            } else {
                                LOG_WARN("Failed to allocate h264_metadata BSF for AUD insertion");
                            }
                        } else {
                            LOG_WARN("h264_metadata bitstream filter not available; skipping AUD insertion");
                        }
                    } else {
                        LOG_WARN("Failed to initialize h264_mp4toannexb bitstream filter: %s", av_err2str(bsf_ret));
                        av_bsf_free(&player->bsf_ctx);
                    }
                } else {
                    LOG_WARN("Failed to copy codec parameters into bitstream filter: %s", av_err2str(bsf_ret));
                    av_bsf_free(&player->bsf_ctx);
                }
            } else {
                LOG_WARN("Failed to allocate h264_mp4toannexb bitstream filter (ret=%d)", bsf_ret);
            }
        } else {
            LOG_INFO("h264_mp4toannexb bitstream filter not available; falling back to manual Annex-B conversion");
        }
    }

    // Convert avcC extradata to Annex-B when needed (common for MP4 files) if filter is unavailable
    if (!player->use_annexb_bsf) {
        if (needs_avcc_conversion &&
            player->codec_ctx->extradata && player->codec_ctx->extradata_size > 0) {
            uint8_t *annexb_data = NULL;
            size_t annexb_size = 0;
            int nal_length_size = avcc_length_size_hint;

            int conv_ret = avcc_extradata_to_annexb(player->codec_ctx->extradata,
                                                    (size_t)player->codec_ctx->extradata_size,
                                                    &annexb_data,
                                                    &annexb_size,
                                                    &nal_length_size);
            if (conv_ret == 0 && annexb_data && annexb_size > 0 && nal_length_size > 0) {
                av_freep(&player->codec_ctx->extradata);
                player->codec_ctx->extradata = annexb_data;
                player->codec_ctx->extradata_size = (int)annexb_size;
                player->avcc_length_size = nal_length_size;
                player->avcc_extradata_converted = true;
                player->extradata_injected = false;
                LOG_INFO("Converted avcC extradata to Annex-B (%zu bytes, nal_length=%d)",
                         annexb_size, nal_length_size);
            } else {
                if (annexb_data) {
                    av_free(annexb_data);
                }
                if (nal_length_size > 0) {
                    player->avcc_length_size = nal_length_size;
                } else if (avcc_length_size_hint > 0) {
                    player->avcc_length_size = avcc_length_size_hint;
                }
                player->extradata_injected = false;
                LOG_WARN("Failed to convert avcC extradata to Annex-B (ret=%d)", conv_ret);
            }
        } else if (avcc_length_size_hint > 0) {
            player->avcc_length_size = avcc_length_size_hint;
            player->extradata_injected = false;
        }
    }
    
    // Set thread count to 1 for V4L2 (hardware decoder)
    player->codec_ctx->thread_count = 1;
    
    // Set packet time base from stream for better timestamp handling
    player->codec_ctx->pkt_timebase = video_stream->time_base;

    // Allow hardware formats: NV12 or DRM_PRIME. We'll convert on upload if needed.
    player->codec_ctx->get_format = v4l2_get_format;
    
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
    
    log_memory_usage("After codec opened");
    
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
    
    log_memory_usage("After NV12 buffer allocated");
    
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
    
    // Minimal priming without seeking - just let first few packets initialize decoder
    LOG_INFO("V4L2 decoder initialized, ready for playback");
    
    player->initialized = true;
    LOG_INFO("FFmpeg V4L2 player initialized successfully");
    
    return true;
}

/**
 * Get next frame from decoder
 * Based on video_decoder.c - limits work per call to avoid blocking
 */
bool ffmpeg_v4l2_get_frame(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->initialized) {
        return false;
    }
    
    int64_t start_time = get_time_us();
    const int time_budget_us = 8000; // Keep each call under ~8ms to avoid stalls
    static ffmpeg_v4l2_player_t *last_player = NULL;
    static int packet_count = 0;  // Track packets sent for debugging
    static int max_packets = 5;    // Adaptive budget per iteration
    static int consecutive_fails = 0;
    static int total_packets_sent = 0;  // Track total packets sent for early failure detection
    static int64_t first_call_us = 0;   // Watchdog: time when we started decoding this player

    if (player != last_player) {
        last_player = player;
        packet_count = 0;
        max_packets = 5;
        consecutive_fails = 0;
        total_packets_sent = 0;
        first_call_us = start_time;
    }
    if (first_call_us == 0) first_call_us = start_time;

    static bool logged_injection_state = false;
    if (g_debug && !logged_injection_state && !player->extradata_injected) {
        logged_injection_state = true;
        AVCodecContext *ctx = player->codec_ctx;
        LOG_DEBUG("[V4L2] Injection state: converted=%d extradata_ptr=%p size=%d",
                  player->avcc_extradata_converted,
                  ctx ? (void *)ctx->extradata : NULL,
                  ctx ? ctx->extradata_size : -1);
    }

    if (!player->use_annexb_bsf &&
        !player->extradata_injected &&
        player->avcc_extradata_converted &&
        player->codec_ctx &&
        player->codec_ctx->extradata &&
        player->codec_ctx->extradata_size > 0) {
        AVPacket *cfg_packet = av_packet_alloc();
        if (!cfg_packet) {
            LOG_ERROR("Failed to allocate SPS/PPS config packet");
            player->fatal_error = true;
            return false;
        }

        if (av_new_packet(cfg_packet, player->codec_ctx->extradata_size) == 0) {
            memcpy(cfg_packet->data,
                   player->codec_ctx->extradata,
                   (size_t)player->codec_ctx->extradata_size);
            cfg_packet->flags |= AV_PKT_FLAG_KEY;
            if (g_debug) {
                LOG_DEBUG("[V4L2] Injecting SPS/PPS config packet (%d bytes)",
                          player->codec_ctx->extradata_size);
            }
            int cfg_ret = avcodec_send_packet(player->codec_ctx, cfg_packet);
            av_packet_unref(cfg_packet);
            av_packet_free(&cfg_packet);
            if (cfg_ret == 0) {
                player->extradata_injected = true;
            } else if (cfg_ret == AVERROR(EAGAIN)) {
                if (g_debug) {
                    LOG_DEBUG("[V4L2] Decoder busy when sending SPS/PPS (EAGAIN), will retry");
                }
            } else {
                LOG_ERROR("Failed to send SPS/PPS config packet: %s", av_err2str(cfg_ret));
                player->fatal_error = true;
                return false;
            }
        } else {
            LOG_ERROR("Failed to allocate SPS/PPS config packet data (%d bytes)",
                      player->codec_ctx->extradata_size);
            av_packet_free(&cfg_packet);
            player->fatal_error = true;
            return false;
        }
    }
    
    // Try to receive a frame first (might be buffered from previous calls)
    int ret = avcodec_receive_frame(player->codec_ctx, player->frame);
    if (ret == 0) {
        // Frame available
        int64_t decode_time = get_time_us() - start_time;
        player->decode_time_avg = (player->decode_time_avg * 0.9) + 
                                 ((double)decode_time / 1000.0 * 0.1);
        player->frames_decoded++;
        packet_count = 0;  // Reset packet count on successful frame
        
        // Log memory usage every 10 frames to catch leaks early
        if (player->frames_decoded % 10 == 0) {
            log_memory_usage("During decode");
        }
        
        return true;
    }
    
    // OPTIMIZED: Limit packets per call to prevent blocking - based on video_decoder.c
    int packets_processed = 0;
    
    const int first_frame_packet_budget = 200;

    // Early failure detection with smaller budget - if we've processed enough packets without getting a frame,
    // something is wrong. Most working H.264 streams produce the first frame within 8-12 packets.
    // BCM2835 V4L2 decoder issues usually manifest as stuck state with EAGAIN loop, so combine
    // this check with EAGAIN streak detection below for robust stuck decoder detection.
    // Global stuck watchdog: if no first frame within 2 seconds, force software fallback
    if (player->frames_decoded == 0 && player->seen_idr && (start_time - first_call_us) > 2000000) {
        LOG_ERROR("No frames produced after %.2f s, forcing software fallback",
                  (double)(start_time - first_call_us) / 1000000.0);
        return switch_to_software_decoder(player);
    }

    if (player->frames_decoded == 0 && player->seen_idr && total_packets_sent > 100) {
        // Get V4L2 device info before giving up
        if (player->codec_ctx) {
            const char* hw_device = NULL;
            AVBufferRef* hw_device_ctx = player->codec_ctx->hw_device_ctx;
            AVCodecParameters* codecpar = player->format_ctx->streams[player->video_stream_index]->codecpar;
            
            LOG_ERROR("V4L2 decoder status: width=%d, height=%d, format=%s, device=%s",
                      player->codec_ctx->width, player->codec_ctx->height,
                      av_get_pix_fmt_name(player->codec_ctx->pix_fmt),
                      hw_device ? hw_device : "unknown");
            LOG_ERROR("Stream info: codec_id=%d, codec_tag=0x%x, format=%d, extradata_size=%d",
                      codecpar->codec_id, codecpar->codec_tag,
                      codecpar->format, codecpar->extradata_size);
            LOG_ERROR("Extradata converted: %d, NAL length size: %d, extradata injected: %d",
                      player->avcc_extradata_converted, player->avcc_length_size, player->extradata_injected);
                      
            // Do NOT flush here; flushing mid-stream can starve HW decoder of refs
            LOG_WARN("Skipping mid-stream flush (EAGAIN implies need more data)");
        }
        
    LOG_WARN("V4L2 M2M decoder failed to produce first frame after %d packets", total_packets_sent);
    LOG_WARN("Stream is still not producing frames - falling back to software decoder");
        
        // Force immediate fallback to software decoder
        return switch_to_software_decoder(player);
    }
    
    // Add a consecutive EAGAIN counter
    static int eagain_streak = 0;
    if (ret == AVERROR(EAGAIN)) {
        eagain_streak++;
    } else {
        eagain_streak = 0;
    }
    
    // Do not force fallback purely on EAGAIN streaks; keep feeding packets
    
    // Loop to handle non-video packets and decoder EAGAIN without blocking too long
    while (ret == AVERROR(EAGAIN) && packets_processed < max_packets) {
        if (g_debug && packets_processed < 10) {
            LOG_DEBUG("[V4L2] Reading packet %d (max %d)...", packets_processed + 1, max_packets);
        }
        // Don't hog the CPU/GPU: respect per-call time budget
        if ((get_time_us() - start_time) >= time_budget_us) {
            if (g_debug) {
                LOG_DEBUG("[V4L2] Time budget hit after %d packets; returning to main loop", packets_processed);
            }
            return false; // Yield back to main loop; try again next iteration
        }
        
        // Read next packet
        ret = av_read_frame(player->format_ctx, player->packet);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                player->eof_reached = true;
                LOG_INFO("EOF reached in ffmpeg_v4l2_player - %d frames decoded", player->frames_decoded);
                if (g_debug) LOG_DEBUG("[V4L2] EOF reached, sending flush packet");

                if (player->use_annexb_bsf && player->bsf_ctx) {
                    int bsf_flush = av_bsf_send_packet(player->bsf_ctx, NULL);
                    if (bsf_flush >= 0 || bsf_flush == AVERROR_EOF) {
                        while ((bsf_flush = av_bsf_receive_packet(player->bsf_ctx, player->packet)) == 0) {
                            if (player->use_aud_bsf && player->bsf_ctx_aud) {
                                int aud_flush = av_bsf_send_packet(player->bsf_ctx_aud, player->packet);
                                if (aud_flush >= 0 || aud_flush == AVERROR_EOF) {
                                    while ((aud_flush = av_bsf_receive_packet(player->bsf_ctx_aud, player->packet)) == 0) {
                                        int send_result = avcodec_send_packet(player->codec_ctx, player->packet);
                                        if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
                                            LOG_WARN("Error sending flushed AUD packet: %s", av_err2str(send_result));
                                        }
                                        av_packet_unref(player->packet);
                                    }
                                }
                            } else {
                                int send_result = avcodec_send_packet(player->codec_ctx, player->packet);
                                if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
                                    LOG_WARN("Error sending flushed bitstream packet: %s", av_err2str(send_result));
                                }
                                av_packet_unref(player->packet);
                            }
                        }
                        if (bsf_flush != AVERROR_EOF && bsf_flush != AVERROR(EAGAIN)) {
                            LOG_WARN("Unexpected bitstream filter flush status: %s", av_err2str(bsf_flush));
                        }
                    } else {
                        LOG_WARN("Bitstream filter flush send failed: %s", av_err2str(bsf_flush));
                    }
                }
                
                // Send flush packet only at end-of-stream
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
                if (g_debug) LOG_DEBUG("[V4L2] No frames available after flush");
                return false;
            }
            LOG_ERROR("Error reading packet: %s", av_err2str(ret));
            player->fatal_error = true;
            return false;
        }
        
        packets_processed++;
        total_packets_sent++;  // Track global packet count
        
        // Skip non-video packets (continue loop instead of returning)
        if (player->packet->stream_index != player->video_stream_index) {
            if (g_debug && packets_processed <= 10) {
                LOG_DEBUG("[V4L2] Skipping non-video packet (stream %d)", player->packet->stream_index);
            }
            av_packet_unref(player->packet);
            continue; // Try next packet
        }
        if (player->packet->flags & AV_PKT_FLAG_KEY) {
            if (!player->seen_keyframe) LOG_INFO("First keyframe encountered, enabling decode watchdogs");
            player->seen_keyframe = true;
        }
        
        if (player->use_annexb_bsf && player->bsf_ctx) {
            static int bsf_in_dump_count = 0;
            if (g_debug && bsf_in_dump_count < 2) {
                LOG_DEBUG("[BSF] Input packet to h264_mp4toannexb (pre-filter)");
                debug_dump_packet_prefix("BSF in", player->packet, 64);
                bsf_in_dump_count++;
            }
            int bsf_ret = av_bsf_send_packet(player->bsf_ctx, player->packet);
            if (bsf_ret < 0) {
                LOG_ERROR("Bitstream filter send failed: %s", av_err2str(bsf_ret));
                av_packet_unref(player->packet);
                continue;
            }

            av_packet_unref(player->packet);

            // Drain all available BSF outputs for this input
            while ((bsf_ret = av_bsf_receive_packet(player->bsf_ctx, player->packet)) == 0) {
                static int bsf_dump_count = 0;
                if (bsf_dump_count < 4) {
                    // Always show first few BSF outputs to verify ordering and start codes
                    int types[8] = {0};
                    int n = scan_nal_types(player->packet->data, player->packet->size, 8, types);
                    LOG_INFO("[BSF] out #%d: size=%d first64=", bsf_dump_count+1, player->packet->size);
                    const int bytes_to_dump = player->packet->size < 64 ? player->packet->size : 64;
                    char hexbuf[3*64 + 1] = {0};
                    int off = 0;
                    for (int i = 0; i < bytes_to_dump; i++) {
                        off += snprintf(hexbuf + off, sizeof(hexbuf) - (size_t)off, "%02X ", player->packet->data[i]);
                    }
                    LOG_INFO("%s", hexbuf);
                    if (n > 0) {
                        char nalstr[64] = {0};
                        off = 0;
                        for (int i = 0; i < n && i < 8; i++) {
                            off += snprintf(nalstr + off, sizeof(nalstr) - (size_t)off, "%d%s", types[i], (i+1<n && i<7)?",":"");
                        }
                        LOG_INFO("[BSF] NAL sequence (first %d): %s", n, nalstr);
                        // Expect SPS(7), PPS(8), optional AUD(9), then IDR(5) among early packets
                        bool has_sps = false, has_pps = false, has_idr = false;
                        for (int i = 0; i < n; i++) {
                            if (types[i] == 7) has_sps = true;
                            if (types[i] == 8) has_pps = true;
                            if (types[i] == 5) { has_idr = true; break; }
                        }
                        if (has_idr && !(has_sps && has_pps)) {
                            LOG_WARN("[BSF] IDR detected before SPS/PPS in first outputs; decoder may return AVERROR_INVALIDDATA");
                        }
                    }
                    bsf_dump_count++;
                }

                // Optionally run through AUD BSF, then send to decoder
                if (player->use_aud_bsf && player->bsf_ctx_aud) {
                    int aud_ret = av_bsf_send_packet(player->bsf_ctx_aud, player->packet);
                    if (aud_ret < 0) {
                        LOG_ERROR("AUD BSF send failed: %s", av_err2str(aud_ret));
                        av_packet_unref(player->packet);
                        continue;
                    }
                    // Drain AUD outputs
                    while ((aud_ret = av_bsf_receive_packet(player->bsf_ctx_aud, player->packet)) == 0) {
                        // Strict gating on IDR: before first IDR, drop ALL AUs (including AUD/SPS/PPS/SEI-only)
                        if (!player->seen_idr) {
                            int types[8] = {0};
                            int n = scan_nal_types(player->packet->data, player->packet->size, 8, types);
                            bool has_idr = false;
                            for (int i = 0; i < n; i++) {
                                if (types[i] == 5) { has_idr = true; break; }
                            }
                            if (has_idr) {
                                player->seen_idr = true;
                                LOG_INFO("[AUD] First IDR observed in AUD-filtered output; enabling decode watchdogs");
                            } else {
                                if (g_debug) LOG_DEBUG("[V4L2] Dropping pre-IDR AU (AUD/SPS/PPS/SEI or non-IDR) before first IDR");
                                av_packet_unref(player->packet);
                                continue;
                            }
                        }
                        // Log first few AUD outputs to verify AUD placement and start codes
                        static int aud_dump_count = 0;
                        if (aud_dump_count < 4) {
                            int types[8] = {0};
                            int n = scan_nal_types(player->packet->data, player->packet->size, 8, types);
                            LOG_INFO("[AUD] out #%d: size=%d first64=", aud_dump_count+1, player->packet->size);
                            const int bytes_to_dump = player->packet->size < 64 ? player->packet->size : 64;
                            char hexbuf[3*64 + 1] = {0};
                            int off = 0;
                            for (int i = 0; i < bytes_to_dump; i++) {
                                off += snprintf(hexbuf + off, sizeof(hexbuf) - (size_t)off, "%02X ", player->packet->data[i]);
                            }
                            LOG_INFO("%s", hexbuf);
                            if (n > 0) {
                                char nalstr[64] = {0};
                                off = 0;
                                for (int i = 0; i < n && i < 8; i++) {
                                    off += snprintf(nalstr + off, sizeof(nalstr) - (size_t)off, "%d%s", types[i], (i+1<n && i<7)?",":"");
                                }
                                LOG_INFO("[AUD] NAL sequence (first %d): %s", n, nalstr);
                            }
                            aud_dump_count++;
                        }
                        packet_count++;
                        if (g_debug && packets_processed <= 10) {
                            LOG_DEBUG("[V4L2] Sending video packet to decoder (packet_count=%d)...", packet_count);
                        }
                        int send_result;
                        while (1) {
                            send_result = avcodec_send_packet(player->codec_ctx, player->packet);
                            if (g_debug && total_packets_sent <= 10) {
                                LOG_DEBUG("[V4L2] avcodec_send_packet returned: %d (%s), packet size: %d, keyframe: %d",
                                          send_result, av_err2str(send_result), player->packet->size,
                                          !!(player->packet->flags & AV_PKT_FLAG_KEY));
                            }
                            if (send_result == AVERROR(EAGAIN)) {
                                int drain;
                                while ((drain = avcodec_receive_frame(player->codec_ctx, player->frame)) == 0) {
                                    consecutive_fails = 0;
                                    if (packets_processed == 1 && max_packets > 5) max_packets--;
                                    if (g_debug) {
                                        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(player->frame->format);
                                        const char *fmt_name = desc ? desc->name : "unknown";
                                        LOG_DEBUG("[V4L2] Decoded frame %d while draining: %dx%d format=%s (%d)",
                                                  player->frames_decoded + 1,
                                                  player->frame->width,
                                                  player->frame->height,
                                                  fmt_name,
                                                  player->frame->format);
                                    }
                                    int64_t decode_time = get_time_us() - start_time;
                                    player->decode_time_avg = (player->decode_time_avg * 0.9) + ((double)decode_time / 1000.0 * 0.1);
                                    player->frames_decoded++;
                                    total_packets_sent = 0;
                                    return true;
                                }
                                if (drain != AVERROR(EAGAIN) && drain < 0) {
                                    LOG_WARN("Decoder drain returned: %s", av_err2str(drain));
                                    break;
                                }
                                continue; // retry send
                            }
                            break;
                        }
                        if (send_result < 0) {
                            LOG_ERROR("Error sending packet: %s", av_err2str(send_result));
                            av_packet_unref(player->packet);
                            break;
                        }
                        av_packet_unref(player->packet);

                        int rcv = avcodec_receive_frame(player->codec_ctx, player->frame);
                        if (g_debug && total_packets_sent <= 10) {
                            LOG_DEBUG("[V4L2] avcodec_receive_frame returned: %d (%s)", rcv, av_err2str(rcv));
                        }
                        if (rcv == 0) {
                            consecutive_fails = 0;
                            if (packets_processed == 1 && max_packets > 5) max_packets--;
                            if (g_debug) {
                                const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(player->frame->format);
                                const char *fmt_name = desc ? desc->name : "unknown";
                                LOG_DEBUG("[V4L2] Successfully decoded frame %d: %dx%d format=%s (%d) after %d packets",
                                          player->frames_decoded + 1,
                                          player->frame->width,
                                          player->frame->height,
                                          fmt_name,
                                          player->frame->format,
                                          packets_processed);
                            }
                            int64_t decode_time = get_time_us() - start_time;
                            player->decode_time_avg = (player->decode_time_avg * 0.9) + ((double)decode_time / 1000.0 * 0.1);
                            player->frames_decoded++;
                            total_packets_sent = 0;
                            return true;
                        } else if (rcv == AVERROR(EAGAIN)) {
                            continue;
                        } else if (rcv == AVERROR(EINVAL) || rcv == AVERROR_INVALIDDATA) {
                            LOG_WARN("Recoverable error receiving frame: %s - continuing without flush", av_err2str(rcv));
                            continue;
                        } else if (rcv < 0) {
                            LOG_ERROR("Error receiving frame: %s", av_err2str(rcv));
                            if (player->frames_decoded == 0 && player->seen_keyframe) {
                                LOG_WARN("No frames decoded yet - trying software fallback");
                                return switch_to_software_decoder(player);
                            }
                            LOG_WARN("Non-fatal decoder error after %d frames - continuing", player->frames_decoded);
                            return false;
                        }
                    }
                    if (aud_ret == AVERROR(EAGAIN)) {
                        // Need more annexb input to produce AUD output
                        continue;
                    } else if (aud_ret < 0 && aud_ret != AVERROR_EOF) {
                        LOG_ERROR("AUD BSF receive failed: %s", av_err2str(aud_ret));
                        continue;
                    }
                } else {
                    // No AUD BSF; strict gating on IDR before sending anything to the decoder
                    if (!player->seen_idr) {
                        int types[8] = {0};
                        int n = scan_nal_types(player->packet->data, player->packet->size, 8, types);
                        bool has_idr = false;
                        for (int i = 0; i < n; i++) {
                            if (types[i] == 5) { has_idr = true; break; }
                        }
                        if (has_idr) {
                            player->seen_idr = true;
                            LOG_INFO("[BSF] First IDR observed in Annex-B output; enabling decode watchdogs");
                        } else {
                            if (g_debug) LOG_DEBUG("[V4L2] Dropping pre-IDR AU (AUD/SPS/PPS/SEI or non-IDR) before first IDR");
                            av_packet_unref(player->packet);
                            continue;
                        }
                    }
                    // No AUD BSF; send annexb output directly to decoder
                    packet_count++;
                    if (g_debug && packets_processed <= 10) {
                        LOG_DEBUG("[V4L2] Sending video packet to decoder (packet_count=%d)...", packet_count);
                    }
                    int send_result;
                    while (1) {
                        send_result = avcodec_send_packet(player->codec_ctx, player->packet);
                        if (g_debug && total_packets_sent <= 10) {
                            LOG_DEBUG("[V4L2] avcodec_send_packet returned: %d (%s), packet size: %d, keyframe: %d",
                                      send_result, av_err2str(send_result), player->packet->size,
                                      !!(player->packet->flags & AV_PKT_FLAG_KEY));
                        }
                        if (send_result == AVERROR(EAGAIN)) {
                            int drain;
                            while ((drain = avcodec_receive_frame(player->codec_ctx, player->frame)) == 0) {
                                consecutive_fails = 0;
                                if (packets_processed == 1 && max_packets > 5) max_packets--;
                                if (g_debug) {
                                    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(player->frame->format);
                                    const char *fmt_name = desc ? desc->name : "unknown";
                                    LOG_DEBUG("[V4L2] Decoded frame %d while draining: %dx%d format=%s (%d)",
                                              player->frames_decoded + 1,
                                              player->frame->width,
                                              player->frame->height,
                                              fmt_name,
                                              player->frame->format);
                                }
                                int64_t decode_time = get_time_us() - start_time;
                                player->decode_time_avg = (player->decode_time_avg * 0.9) + ((double)decode_time / 1000.0 * 0.1);
                                player->frames_decoded++;
                                total_packets_sent = 0;
                                return true;
                            }
                            if (drain != AVERROR(EAGAIN) && drain < 0) {
                                LOG_WARN("Decoder drain returned: %s", av_err2str(drain));
                                break;
                            }
                            continue; // retry send
                        }
                        break;
                    }
                    if (send_result < 0) {
                        LOG_ERROR("Error sending packet: %s", av_err2str(send_result));
                        av_packet_unref(player->packet);
                        break;
                    }
                    av_packet_unref(player->packet);

                    int rcv = avcodec_receive_frame(player->codec_ctx, player->frame);
                    if (g_debug && total_packets_sent <= 10) {
                        LOG_DEBUG("[V4L2] avcodec_receive_frame returned: %d (%s)", rcv, av_err2str(rcv));
                    }
                    if (rcv == 0) {
                        consecutive_fails = 0;
                        if (packets_processed == 1 && max_packets > 5) max_packets--;
                        if (g_debug) {
                            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(player->frame->format);
                            const char *fmt_name = desc ? desc->name : "unknown";
                            LOG_DEBUG("[V4L2] Successfully decoded frame %d: %dx%d format=%s (%d) after %d packets",
                                      player->frames_decoded + 1,
                                      player->frame->width,
                                      player->frame->height,
                                      fmt_name,
                                      player->frame->format,
                                      packets_processed);
                        }
                        int64_t decode_time = get_time_us() - start_time;
                        player->decode_time_avg = (player->decode_time_avg * 0.9) + ((double)decode_time / 1000.0 * 0.1);
                        player->frames_decoded++;
                        total_packets_sent = 0;
                        return true;
                    } else if (rcv == AVERROR(EAGAIN)) {
                        continue;
                    } else if (rcv == AVERROR(EINVAL) || rcv == AVERROR_INVALIDDATA) {
                        LOG_WARN("Recoverable error receiving frame: %s - continuing without flush", av_err2str(rcv));
                        continue;
                    } else if (rcv < 0) {
                        LOG_ERROR("Error receiving frame: %s", av_err2str(rcv));
                        if (player->frames_decoded == 0 && player->seen_keyframe) {
                            LOG_WARN("No frames decoded yet - trying software fallback");
                            return switch_to_software_decoder(player);
                        }
                        LOG_WARN("Non-fatal decoder error after %d frames - continuing", player->frames_decoded);
                        return false;
                    }
                }
            }
            if (bsf_ret == AVERROR(EAGAIN)) {
                if (g_debug) {
                    LOG_DEBUG("[V4L2] Bitstream filter needs more packets (EAGAIN)");
                }
                continue;
            } else if (bsf_ret < 0 && bsf_ret != AVERROR_EOF) {
                LOG_ERROR("Bitstream filter receive failed: %s", av_err2str(bsf_ret));
                continue;
            }
        } else {
            // No Annex-B BSF in use: send input packet directly (with optional inline avcC->Annex-B conversion)
            packet_count++;
            if (g_debug && packets_processed <= 10) {
                LOG_DEBUG("[V4L2] Sending video packet to decoder (packet_count=%d)...", packet_count);
            }

            // Convert avcC-formatted packet payload to Annex-B if required
            if (player->avcc_length_size > 0) {
                int conv_ret = convert_sample_avcc_to_annexb(player->packet,
                                                             player->avcc_length_size,
                                                             NULL,
                                                             0,
                                                             (player->packet->flags & AV_PKT_FLAG_KEY) != 0);
                if (conv_ret < 0) {
                    LOG_ERROR("Packet Annex-B conversion failed: %s", av_err2str(conv_ret));
                    player->fatal_error = true;
                    av_packet_unref(player->packet);
                    return false;
                }
                if (g_debug) {
                    static int dump_count2 = 0;
                    if (dump_count2 < 2) {
                        LOG_DEBUG("[V4L2] Inline Annex-B conversion succeeded (size=%d)", player->packet->size);
                        dump_count2++;
                    }
                }
            }

            // Send packet to decoder
            int send_result;
            while (1) {
                send_result = avcodec_send_packet(player->codec_ctx, player->packet);
                if (g_debug && total_packets_sent <= 10) {
                    LOG_DEBUG("[V4L2] avcodec_send_packet returned: %d (%s), packet size: %d, keyframe: %d",
                              send_result, av_err2str(send_result), player->packet->size,
                              !!(player->packet->flags & AV_PKT_FLAG_KEY));
                }
                if (send_result == AVERROR(EAGAIN)) {
                    int drain;
                    while ((drain = avcodec_receive_frame(player->codec_ctx, player->frame)) == 0) {
                        consecutive_fails = 0;
                        if (packets_processed == 1 && max_packets > 5) max_packets--;
                        if (g_debug) {
                            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(player->frame->format);
                            const char *fmt_name = desc ? desc->name : "unknown";
                            LOG_DEBUG("[V4L2] Decoded frame %d while draining: %dx%d format=%s (%d)",
                                      player->frames_decoded + 1,
                                      player->frame->width,
                                      player->frame->height,
                                      fmt_name,
                                      player->frame->format);
                        }
                        int64_t decode_time = get_time_us() - start_time;
                        player->decode_time_avg = (player->decode_time_avg * 0.9) + ((double)decode_time / 1000.0 * 0.1);
                        player->frames_decoded++;
                        total_packets_sent = 0;
                        return true;
                    }
                    if (drain != AVERROR(EAGAIN) && drain < 0) {
                        LOG_WARN("Decoder drain returned: %s", av_err2str(drain));
                        break;
                    }
                    continue; // retry send
                }
                break;
            }
            if (send_result < 0) {
                LOG_ERROR("Error sending packet: %s", av_err2str(send_result));
                av_packet_unref(player->packet);
                return false;
            }
            av_packet_unref(player->packet);

            // Try receive once after sending
            ret = avcodec_receive_frame(player->codec_ctx, player->frame);
            if (g_debug && total_packets_sent <= 10) {
                LOG_DEBUG("[V4L2] avcodec_receive_frame returned: %d (%s)", ret, av_err2str(ret));
            }
            if (ret == 0) {
                consecutive_fails = 0;
                if (packets_processed == 1 && max_packets > 5) max_packets--;
                if (g_debug) {
                    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(player->frame->format);
                    const char *fmt_name = desc ? desc->name : "unknown";
                    LOG_DEBUG("[V4L2] Successfully decoded frame %d: %dx%d format=%s (%d) after %d packets",
                              player->frames_decoded + 1,
                              player->frame->width,
                              player->frame->height,
                              fmt_name,
                              player->frame->format,
                              packets_processed);
                }
                int64_t decode_time = get_time_us() - start_time;
                player->decode_time_avg = (player->decode_time_avg * 0.9) + ((double)decode_time / 1000.0 * 0.1);
                player->frames_decoded++;
                total_packets_sent = 0;
                return true;
            } else if (ret == AVERROR(EAGAIN)) {
                // Decoder needs more input; loop will continue
            } else if (ret == AVERROR(EINVAL) || ret == AVERROR_INVALIDDATA) {
                LOG_WARN("Recoverable error receiving frame: %s - continuing without flush", av_err2str(ret));
                // continue outer loop
            } else if (ret < 0) {
                LOG_ERROR("Error receiving frame: %s", av_err2str(ret));
                if (player->frames_decoded == 0 && player->seen_keyframe) {
                    LOG_WARN("No frames decoded yet - trying software fallback");
                    return switch_to_software_decoder(player);
                }
                LOG_WARN("Non-fatal decoder error after %d frames - continuing", player->frames_decoded);
                return false;
            }
        }
    }

    // OPTIMIZED: Adaptive packet processing to prevent blocking
    if (ret == AVERROR(EAGAIN) && packets_processed >= max_packets) {
        consecutive_fails++;
        if (consecutive_fails < 3) {
            // Gradually increase packet limit for difficult sections
            if (max_packets < 21) max_packets += 2;
            if (g_debug) {
                LOG_DEBUG("[V4L2] No frame after %d packets, increasing limit to %d (fails: %d)", 
                         packets_processed, max_packets, consecutive_fails);
            }
        }
        // Return false to let main loop continue and try again next iteration
        return false;
    }
    
    // Handle errors - but don't treat EOF as fatal
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        LOG_ERROR("Error in frame decoding: %s", av_err2str(ret));
        player->fatal_error = true;
    }
    
    return false;
}

static bool copy_frame_to_nv12_buffer(const AVFrame *frame,
                                      ffmpeg_v4l2_player_t *player,
                                      const uint8_t **y_plane_out,
                                      const uint8_t **uv_plane_out) {
    if (!frame || !player || !y_plane_out || !uv_plane_out) {
        return false;
    }

    const int width = frame->width;
    const int height = frame->height;
    if (width <= 0 || height <= 0) {
        return false;
    }

    // Check if format is already NV12 with contiguous memory and stride equals width
    const enum AVPixelFormat fmt = (enum AVPixelFormat)frame->format;
    
    // OPTIMIZATION: If the frame is already in NV12 format with aligned linesize,
    // we can use it directly without copying
    if (fmt == AV_PIX_FMT_NV12 && 
        frame->linesize[0] == width && 
        frame->linesize[1] == width && 
        frame->data[0] && frame->data[1]) {
        
        // Use frame data directly
        *y_plane_out = frame->data[0];
        *uv_plane_out = frame->data[1];
        return true;
    }
    
    // For other cases, we need to copy to our buffer
    const size_t y_plane_size = (size_t)width * (size_t)height;
    const size_t uv_plane_size = y_plane_size / 2;
    const size_t required_size = y_plane_size + uv_plane_size;

    if (!player->nv12_buffer || player->nv12_buffer_size < required_size) {
        LOG_ERROR("NV12 staging buffer unavailable or too small (have %zu need %zu)",
                  player->nv12_buffer_size, required_size);
        return false;
    }

    uint8_t *dst_y = player->nv12_buffer;
    uint8_t *dst_uv = player->nv12_buffer + y_plane_size;

    switch (fmt) {
    case AV_PIX_FMT_NV12:
        for (int y = 0; y < height; ++y) {
            const size_t dst_offset = (size_t)y * (size_t)width;
            const size_t src_offset = (size_t)y * (size_t)frame->linesize[0];
            memcpy(dst_y + dst_offset,
                   frame->data[0] + src_offset,
                   (size_t)width);
        }
        for (int y = 0; y < height / 2; ++y) {
            const size_t dst_offset = (size_t)y * (size_t)width;
            const size_t src_offset = (size_t)y * (size_t)frame->linesize[1];
            memcpy(dst_uv + dst_offset,
                   frame->data[1] + src_offset,
                   (size_t)width);
        }
        break;
    case AV_PIX_FMT_YUV420P:
        for (int y = 0; y < height; ++y) {
            const size_t dst_offset = (size_t)y * (size_t)width;
            const size_t src_offset = (size_t)y * (size_t)frame->linesize[0];
            memcpy(dst_y + dst_offset,
                   frame->data[0] + src_offset,
                   (size_t)width);
        }
        for (int y = 0; y < height / 2; ++y) {
            const size_t dst_offset = (size_t)y * (size_t)width;
            const uint8_t *src_u = frame->data[1] + (size_t)y * (size_t)frame->linesize[1];
            const uint8_t *src_v = frame->data[2] + (size_t)y * (size_t)frame->linesize[2];
            uint8_t *dst_row = dst_uv + dst_offset;
            for (int x = 0; x < width / 2; ++x) {
                dst_row[(size_t)x * 2] = src_u[x];
                dst_row[(size_t)x * 2 + 1] = src_v[x];
            }
        }
        break;
    case AV_PIX_FMT_DRM_PRIME: {
        // Transfer from hardware to a software NV12 frame
        AVFrame *sw = av_frame_alloc();
        if (!sw) {
            LOG_ERROR("Failed to allocate SW frame for DRM_PRIME transfer");
            return false;
        }
        // Prefer NV12; if fails, try YUV420P and convert
        sw->format = AV_PIX_FMT_NV12;
        sw->width = width;
        sw->height = height;
        int ret = av_hwframe_transfer_data(sw, (AVFrame *)frame, 0);
        if (ret < 0) {
            LOG_WARN("NV12 hwframe transfer failed: %s; trying yuv420p", av_err2str(ret));
            av_frame_unref(sw);
            sw->format = AV_PIX_FMT_YUV420P;
            sw->width = width;
            sw->height = height;
            ret = av_hwframe_transfer_data(sw, (AVFrame *)frame, 0);
        }
        if (ret < 0) {
            LOG_ERROR("DRM_PRIME transfer to SW frame failed: %s", av_err2str(ret));
            av_frame_free(&sw);
            return false;
        }
        bool ok;
        if (sw->format == AV_PIX_FMT_NV12) {
            ok = copy_frame_to_nv12_buffer(sw, player, y_plane_out, uv_plane_out);
        } else if (sw->format == AV_PIX_FMT_YUV420P) {
            ok = copy_frame_to_nv12_buffer(sw, player, y_plane_out, uv_plane_out);
        } else {
            LOG_WARN("Unexpected SW format after DRM_PRIME transfer: %s", av_get_pix_fmt_name(sw->format));
            ok = false;
        }
        av_frame_free(&sw);
        return ok;
    }
    default: {
        const char *fmt_name = av_get_pix_fmt_name(fmt);
        LOG_WARN("Unsupported V4L2 pixel format for NV12 upload: %s",
                 fmt_name ? fmt_name : "unknown");
        return false;
    }
    }

    *y_plane_out = dst_y;
    *uv_plane_out = dst_uv;
    return true;
}

/**
 * Upload frame data to OpenGL textures (NV12 format)
 * Optimized to avoid unnecessary copies
 */
bool ffmpeg_v4l2_upload_to_gl(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->frame || !player->frame->data[0]) {
        return false;
    }

    // Check if we already have valid textures with the current frame's data
    // to avoid unnecessary uploads (e.g., when paused)
    static int64_t last_frame_pts = INT64_MIN;
    static int64_t last_frame_count = -1;
    
    if (player->texture_valid && 
        player->frame->pts == last_frame_pts && 
        player->frames_decoded == last_frame_count) {
        // Frame hasn't changed since last upload - skip
        return true;
    }
    
    // Remember current frame details
    last_frame_pts = player->frame->pts;
    last_frame_count = player->frames_decoded;

    AVFrame *frame = player->frame;
    const uint8_t *y_plane = NULL;
    const uint8_t *uv_plane = NULL;

    if (!copy_frame_to_nv12_buffer(frame, player, &y_plane, &uv_plane)) {
        return false;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    // Only upload textures if we have valid data
    if (y_plane && uv_plane) {
        // Upload Y plane
        glBindTexture(GL_TEXTURE_2D, player->y_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        frame->width, frame->height,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE,
                        y_plane);
    
        // Upload UV plane (NV12 format has interleaved U and V)
        glBindTexture(GL_TEXTURE_2D, player->uv_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        frame->width / 2, frame->height / 2,
                        GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
                        uv_plane);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    
    player->texture_valid = true;
    player->frames_rendered++;
    
    // CRITICAL: Unref the frame to release its memory
    // Without this, frame buffers accumulate and cause OOM
    av_frame_unref(player->frame);
    
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
    
    // Flushing is appropriate on seek to discard decoder refs from old position
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
    player->fatal_error = false;
    if (player->use_annexb_bsf && player->bsf_ctx) {
        av_bsf_flush(player->bsf_ctx);
        player->extradata_injected = true;
    }
    if (player->use_aud_bsf && player->bsf_ctx_aud) {
        av_bsf_flush(player->bsf_ctx_aud);
    }
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
    if (player->bsf_ctx) {
        LOG_INFO("Freeing bitstream filter context...");
        av_bsf_free(&player->bsf_ctx);
    }
    if (player->bsf_ctx_aud) {
        LOG_INFO("Freeing AUD bitstream filter context...");
        av_bsf_free(&player->bsf_ctx_aud);
    }
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
    player->extradata_injected = false;
    player->use_annexb_bsf = false;
    
    LOG_INFO("FFmpeg V4L2 player cleaned up");
}
