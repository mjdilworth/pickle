#define _GNU_SOURCE
#include "ffmpeg_v4l2_player.h"
#include "pickle_globals.h"
#include "log.h"
#include "shader.h"
#include "keystone.h"
#include "render.h"
#include "gl_optimize.h"

// Debug flag for BSF/filter packet dumps - set to 0 to disable detailed packet logging
#ifndef FFMPEG_V4L2_DEBUG_BSF
#define FFMPEG_V4L2_DEBUG_BSF 0
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <libavutil/common.h>
#include <libavutil/mem.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
#include <libavutil/hwcontext.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>

static int64_t get_time_us(void);
static void reset_parser_state(ffmpeg_v4l2_player_t *player);
static bool deep_reset_codec(ffmpeg_v4l2_player_t *player);

// Global counters for tracking parser reset effectiveness
static int consecutive_parser_resets = 0;
static int parser_reset_threshold = 3;
static bool deep_reset_attempted = false;
static int consecutive_eagain = 0;
static int total_eagain = 0;
static int max_eagain_sequence = 0;
static int last_total_eagain = 0;
static int64_t last_reset_time = 0;

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

static void reset_parser_state(ffmpeg_v4l2_player_t *player) {
    if (!player) {
        return;
    }
    
    // Track consecutive resets
    consecutive_parser_resets++;
    int64_t current_time = get_time_us();
    
    // Reset and reinitialize the parser
    if (player->parser_ctx) {
        av_parser_close(player->parser_ctx);
        player->parser_ctx = av_parser_init(AV_CODEC_ID_H264);
        
        // If we still can't create a parser, mark as fatal error
        if (!player->parser_ctx) {
            LOG_ERROR("[V4L2] Failed to reinitialize H.264 parser after reset");
            player->fatal_error = true;
            return;
        }
        LOG_WARN("[V4L2] Parser reset #%d complete (threshold for deep reset: %d)", 
                 consecutive_parser_resets, parser_reset_threshold);
    }
    
    // Flush buffers to reset decoder state
    if (player->codec_ctx) {
        avcodec_flush_buffers(player->codec_ctx);
        LOG_WARN("[V4L2] Decoder buffers flushed");
    }
    
    // If we've reset multiple times in quick succession, try a deep reset
    if (consecutive_parser_resets >= parser_reset_threshold && !deep_reset_attempted) {
        LOG_ERROR("[V4L2] Multiple parser resets (%d) haven't fixed the issue, attempting deep reset", 
                  consecutive_parser_resets);
            if (deep_reset_codec(player)) { 
            consecutive_parser_resets = 0;
            deep_reset_attempted = true;
        }
    }
    
    // Store time of last reset
    last_reset_time = current_time;

    if (player->parser_ctx && !player->au_packet) {
        player->au_packet = av_packet_alloc();
        if (!player->au_packet) {
            LOG_WARN("Failed to allocate parser output packet during reset");
            av_parser_close(player->parser_ctx);
            player->parser_ctx = NULL;
        }
    }

    if (player->au_packet) {
        av_packet_unref(player->au_packet);
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

#if FFMPEG_V4L2_DEBUG_BSF
static int64_t last_memory_log_time = 0;  // Throttle memory logging to once per second

static void log_memory_usage(const char *context) {
    // Throttle logging to once per second to avoid spam
    int64_t current_time = get_time_us();
    if (last_memory_log_time > 0 && (current_time - last_memory_log_time) < 1000000) {
        return;  // Skip if less than 1 second has passed since the last log
    }
    
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
    
    LOG_DEBUG("[MEMORY] %s: VmSize=%ld MB, VmRSS=%ld MB, VmPeak=%ld MB",
             context, vm_size/1024, vm_rss/1024, vm_peak/1024);
    
    // Update the last log time
    last_memory_log_time = current_time;
}
#endif

typedef enum {
    DELIVERY_CONTINUE = 0,
    DELIVERY_FRAME_READY,
    DELIVERY_FATAL
} delivery_result_t;

static bool normalize_frame_pts(ffmpeg_v4l2_player_t *player,
                                int64_t *pts_stream_out,
                                int64_t *pts_us_out) {
    if (!player || !player->frame) {
        if (pts_stream_out) *pts_stream_out = AV_NOPTS_VALUE;
        if (pts_us_out) *pts_us_out = AV_NOPTS_VALUE;
        return false;
    }

    bool synthetic = false;
    int64_t pts_stream = player->frame->pts;

    if (pts_stream != AV_NOPTS_VALUE) {
        player->last_valid_pts = pts_stream;
    } else if (player->last_valid_pts != AV_NOPTS_VALUE && player->frame_duration > 0) {
        pts_stream = player->last_valid_pts + player->frame_duration;
        player->last_valid_pts = pts_stream;
        synthetic = true;
    } else {
        if (pts_stream_out) *pts_stream_out = AV_NOPTS_VALUE;
        if (pts_us_out) *pts_us_out = AV_NOPTS_VALUE;
        player->frame->pts = AV_NOPTS_VALUE;
        return synthetic;
    }

    int64_t pts_us = av_rescale_q(pts_stream, player->stream_time_base, AV_TIME_BASE_Q);
    if (pts_us == AV_NOPTS_VALUE) {
        if (pts_stream_out) *pts_stream_out = pts_stream;
        if (pts_us_out) *pts_us_out = AV_NOPTS_VALUE;
        player->frame->pts = pts_stream;
        return synthetic;
    }

    player->frame->pts = pts_us;
    if (pts_stream_out) *pts_stream_out = pts_stream;
    if (pts_us_out) *pts_us_out = pts_us;
    return synthetic;
}

static bool update_seen_idr(ffmpeg_v4l2_player_t *player, const AVPacket *packet) {
    if (!player || player->seen_idr || !packet || !packet->data || packet->size <= 0) {
        return false;
    }

    int types[8] = {0};
    int count = scan_nal_types(packet->data, packet->size, 8, types);
    for (int i = 0; i < count; i++) {
        if (types[i] == 5) {
            player->seen_idr = true;
            return true;
        }
    }
    return false;
}

static delivery_result_t deliver_final_access_unit(ffmpeg_v4l2_player_t *player,
                                                   AVPacket *packet,
                                                   int64_t start_time,
                                                   int *packets_processed,
                                                   int *max_packets,
                                                   int *consecutive_fails,
                                                   uint64_t *total_packets_sent,
                                                   int *packet_count) {
    if (!player || !packet) {
        return DELIVERY_CONTINUE;
    }

    if (packet_count) {
        (*packet_count)++;
        if (g_debug && packets_processed && *packets_processed <= 10) {
            LOG_DEBUG("[V4L2] Sending access unit to decoder (packet_count=%d)", *packet_count);
        }
    }

    // First try to send the packet
    int send_result = avcodec_send_packet(player->codec_ctx, packet);
    
    if (g_debug && total_packets_sent && *total_packets_sent <= 10) {
        LOG_DEBUG("[V4L2] avcodec_send_packet -> %d (%s), size=%d, key=%d",
                  send_result,
                  av_err2str(send_result),
                  packet->size,
                  !!(packet->flags & AV_PKT_FLAG_KEY));
    }
    
    // Done with packet data at this point
    av_packet_unref(packet);

    if (send_result == 0) {
        // Successfully sent packet - now drain all frames produced by this send
        for (;;) {
            int receive_ret = avcodec_receive_frame(player->codec_ctx, player->frame);
            
            if (receive_ret == 0) {
                // Frame successfully received - process it
                int64_t pts_stream = AV_NOPTS_VALUE;
                int64_t pts_us = AV_NOPTS_VALUE;
                bool synthetic = normalize_frame_pts(player, &pts_stream, &pts_us);
                
                if (consecutive_fails) *consecutive_fails = 0;
                if (packets_processed && *packets_processed == 1 && max_packets && *max_packets > 5) {
                    (*max_packets)--;
                }
                
                if (g_debug) {
                    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(player->frame->format);
                    const char *fmt_name = desc ? desc->name : "unknown";
                    if (pts_us != AV_NOPTS_VALUE) {
                        LOG_DEBUG("[V4L2] Frame %" PRIu64 ": %dx%d %s (%d) pts=%" PRId64 " (%.3f ms)%s",
                                  (uint64_t)(player->frames_decoded + 1),
                                  player->frame->width,
                                  player->frame->height,
                                  fmt_name,
                                  player->frame->format,
                                  pts_stream,
                                  (double)pts_us / 1000.0,
                                  synthetic ? " [synthetic]" : "");
                    } else {
                        LOG_DEBUG("[V4L2] Frame %" PRIu64 ": %dx%d %s (%d) pts=NOPTS%s",
                                  (uint64_t)(player->frames_decoded + 1),
                                  player->frame->width,
                                  player->frame->height,
                                  fmt_name,
                                  player->frame->format,
                                  synthetic ? " [synthetic]" : "");
                    }
                }
                
                int64_t decode_time = get_time_us() - start_time;
                player->decode_time_avg = (player->decode_time_avg * 0.9) +
                                          ((double)decode_time / 1000.0 * 0.1);
                player->frames_decoded++;
                
                // Reset all EAGAIN counters since we successfully decoded a frame
                consecutive_eagain = 0;
                last_total_eagain = total_eagain;
                
                if (total_packets_sent) *total_packets_sent = 0;
                
                // Reset parser reset counters as well since we're successfully decoding
                if (consecutive_parser_resets > 0) {
                    LOG_INFO("[V4L2] Successfully decoded frame after %d parser resets, resetting counter", 
                           consecutive_parser_resets);
                    consecutive_parser_resets = 0;
                    
                    // If we successfully decoded after a deep reset, adjust our threshold to try it sooner next time
                    if (deep_reset_attempted) {
                        LOG_INFO("[V4L2] Deep reset was successful, decreasing threshold for future use");
                        if (parser_reset_threshold > 2) {
                            parser_reset_threshold--;
                        }
                        deep_reset_attempted = false;
                    }
                }
                
                // Log successful frame decoding when we've been having EAGAIN issues
                if (total_eagain > 0) {
                    LOG_DEBUG("[V4L2] Successfully decoded frame after %d EAGAIN responses (max consecutive: %d)",
                             total_eagain, max_eagain_sequence);
                }
                
                return DELIVERY_FRAME_READY;
            }
            
            if (receive_ret == AVERROR(EAGAIN)) {
                // Need more input
                break;
            }
            
            if (receive_ret == AVERROR_EOF) {
                LOG_INFO("Decoder reached EOF");
                return DELIVERY_CONTINUE;
            }
            
            if (receive_ret < 0) {
                if (receive_ret == AVERROR(EINVAL) || receive_ret == AVERROR_INVALIDDATA) {
                    LOG_WARN("Recoverable receive error: %s", av_err2str(receive_ret));
                    return DELIVERY_CONTINUE;
                }
                LOG_ERROR("Error receiving frame: %s", av_err2str(receive_ret));
                player->fatal_error = true;
                return DELIVERY_FATAL;
            }
        }
    }
    else if (send_result == AVERROR(EAGAIN)) {
        // Input queue full - try to receive one frame to free space
        int receive_ret = avcodec_receive_frame(player->codec_ctx, player->frame);
        
        if (receive_ret == 0) {
            // Successfully got a frame - process it
            int64_t pts_stream = AV_NOPTS_VALUE;
            int64_t pts_us = AV_NOPTS_VALUE;
            bool synthetic = normalize_frame_pts(player, &pts_stream, &pts_us);
            
            if (consecutive_fails) *consecutive_fails = 0;
            if (packets_processed && *packets_processed == 1 && max_packets && *max_packets > 5) {
                (*max_packets)--;
            }
            
            if (g_debug) {
                const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(player->frame->format);
                const char *fmt_name = desc ? desc->name : "unknown";
                if (pts_us != AV_NOPTS_VALUE) {
                    LOG_DEBUG("[V4L2] EAGAIN Frame %" PRIu64 ": %dx%d %s (%d) pts=%" PRId64 " (%.3f ms)%s",
                              (uint64_t)(player->frames_decoded + 1),
                              player->frame->width,
                              player->frame->height,
                              fmt_name,
                              player->frame->format,
                              pts_stream,
                              (double)pts_us / 1000.0,
                              synthetic ? " [synthetic]" : "");
                }
            }
            
            int64_t decode_time = get_time_us() - start_time;
            player->decode_time_avg = (player->decode_time_avg * 0.9) +
                                      ((double)decode_time / 1000.0 * 0.1);
            player->frames_decoded++;
            
            // Reset EAGAIN counters on successful frame
            consecutive_eagain = 0;
            last_total_eagain = total_eagain;
            
            if (total_packets_sent) *total_packets_sent = 0;
            
            return DELIVERY_FRAME_READY;
        }
    } 
    else if (send_result < 0) {
        // Error on send
        if (send_result == AVERROR(EINVAL) || send_result == AVERROR_INVALIDDATA) {
            LOG_WARN("Recoverable send error: %s", av_err2str(send_result));
            return DELIVERY_CONTINUE;
        }
        
        LOG_ERROR("Error sending packet: %s", av_err2str(send_result));
        player->fatal_error = true;
        return DELIVERY_FATAL;
    }
    
    return DELIVERY_CONTINUE;
}

// Tag helper for logging context
#define STAGE_TAG(tag) ((tag) ? (tag) : "BSF")

static delivery_result_t forward_packet_to_decoder(ffmpeg_v4l2_player_t *player,
                                                   AVPacket *packet,
                                                   int64_t start_time,
                                                   int *packets_processed,
                                                   int *max_packets,
                                                   int *consecutive_fails,
                                                   uint64_t *total_packets_sent,
                                                   int *packet_count,
                                                   const char *stage_tag) {
    if (!player || !packet) {
        return DELIVERY_CONTINUE;
    }

    const char *tag = STAGE_TAG(stage_tag);
    bool new_idr = update_seen_idr(player, packet);
    if (!player->seen_idr) {
        if (g_debug) {
            LOG_DEBUG("[%s] Dropping AU before first IDR", tag);
        }
        av_packet_unref(packet);
        return DELIVERY_CONTINUE;
    }

    if (new_idr) {
#if FFMPEG_V4L2_DEBUG_BSF
        if (strcmp(tag, "AUD") == 0) {
            LOG_DEBUG("[AUD] First IDR observed after AUD insertion");
        } else if (strcmp(tag, "FILTER") == 0) {
            LOG_DEBUG("[FILTER] First IDR observed after SEI/AUD stripping");
        } else {
            LOG_DEBUG("[%s] First IDR observed in Annex-B output", tag);
        }
#endif
    }

    return deliver_final_access_unit(player,
                                     packet,
                                     start_time,
                                     packets_processed,
                                     max_packets,
                                     consecutive_fails,
                                     total_packets_sent,
                                     packet_count);
}

#undef STAGE_TAG

static delivery_result_t forward_parsed_access_unit(ffmpeg_v4l2_player_t *player,
                                                    const uint8_t *data,
                                                    int size,
                                                    int64_t pts,
                                                    int64_t dts,
                                                    int flags,
                                                    int64_t pos,
                                                    int64_t start_time,
                                                    int *packets_processed,
                                                    int *max_packets,
                                                    int *consecutive_fails,
                                                    uint64_t *total_packets_sent,
                                                    int *packet_count,
                                                    const char *stage_tag) {
    if (!player || !data || size <= 0) {
        return DELIVERY_CONTINUE;
    }

    if (!player->au_packet) {
        player->au_packet = av_packet_alloc();
        if (!player->au_packet) {
            LOG_ERROR("Failed to allocate parser output packet");
            player->fatal_error = true;
            return DELIVERY_FATAL;
        }
    }

    int ret = av_new_packet(player->au_packet, size);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate AU buffer (%s)", av_err2str(ret));
        player->fatal_error = true;
        return DELIVERY_FATAL;
    }

    memcpy(player->au_packet->data, data, (size_t)size);
    player->au_packet->pts = pts;
    player->au_packet->dts = dts;
    player->au_packet->duration = 0;
    player->au_packet->pos = pos;
    player->au_packet->flags = flags;

    return forward_packet_to_decoder(player,
                                     player->au_packet,
                                     start_time,
                                     packets_processed,
                                     max_packets,
                                     consecutive_fails,
                                     total_packets_sent,
                                     packet_count,
                                     stage_tag);
}

static delivery_result_t dispatch_packet_to_decoder(ffmpeg_v4l2_player_t *player,
                                                    AVPacket *packet,
                                                    int64_t start_time,
                                                    int *packets_processed,
                                                    int *max_packets,
                                                    int *consecutive_fails,
                                                    uint64_t *total_packets_sent,
                                                    int *packet_count,
                                                    const char *stage_tag) {
    if (!player || !packet) {
        return DELIVERY_CONTINUE;
    }

    if (!player->parser_ctx || packet->size <= 0) {
        return forward_packet_to_decoder(player,
                                         packet,
                                         start_time,
                                         packets_processed,
                                         max_packets,
                                         consecutive_fails,
                                         total_packets_sent,
                                         packet_count,
                                         stage_tag);
    }

    uint8_t *data = packet->data;
    int data_size = packet->size;
    int64_t pts = packet->pts;
    int64_t dts = packet->dts;
    int64_t pos = packet->pos;
    int upstream_flags = packet->flags;

    while (data_size > 0) {
        uint8_t *out_data = NULL;
        int out_size = 0;
        int used = av_parser_parse2(player->parser_ctx,
                                    player->codec_ctx,
                                    &out_data,
                                    &out_size,
                                    data,
                                    data_size,
                                    pts,
                                    dts,
                                    pos);
        if (used < 0) {
            LOG_ERROR("H.264 parser failed: %s", av_err2str(used));
            av_packet_unref(packet);
            player->fatal_error = true;
            return DELIVERY_FATAL;
        }

        data += used;
        data_size -= used;
        pts = AV_NOPTS_VALUE;
        dts = AV_NOPTS_VALUE;
        pos = -1;

        if (out_size > 0 && out_data) {
            int flags = 0;
            if (player->parser_ctx->key_frame || (upstream_flags & AV_PKT_FLAG_KEY)) {
                flags |= AV_PKT_FLAG_KEY;
            }

            int64_t out_pts = player->parser_ctx->pts;
            int64_t out_dts = player->parser_ctx->dts;
            if (out_pts == AV_NOPTS_VALUE) {
                out_pts = packet->pts;
            }
            if (out_dts == AV_NOPTS_VALUE) {
                out_dts = packet->dts;
            }

            delivery_result_t res = forward_parsed_access_unit(player,
                                                                out_data,
                                                                out_size,
                                                                out_pts,
                                                                out_dts,
                                                                flags,
                                                                packet->pos,
                                                                start_time,
                                                                packets_processed,
                                                                max_packets,
                                                                consecutive_fails,
                                                                total_packets_sent,
                                                                packet_count,
                                                                stage_tag);
            if (res != DELIVERY_CONTINUE) {
                av_packet_unref(packet);
                return res;
            }
        }

        if (used == 0 && out_size == 0) {
            break;
        }
    }

    av_packet_unref(packet);
    return DELIVERY_CONTINUE;
}

static delivery_result_t flush_parser_output(ffmpeg_v4l2_player_t *player,
                                             int64_t start_time,
                                             int *packets_processed,
                                             int *max_packets,
                                             int *consecutive_fails,
                                             uint64_t *total_packets_sent,
                                             int *packet_count,
                                             const char *stage_tag) {
    if (!player || !player->parser_ctx) {
        return DELIVERY_CONTINUE;
    }

    while (1) {
        uint8_t *out_data = NULL;
        int out_size = 0;
        int ret = av_parser_parse2(player->parser_ctx,
                                   player->codec_ctx,
                                   &out_data,
                                   &out_size,
                                   NULL,
                                   0,
                                   AV_NOPTS_VALUE,
                                   AV_NOPTS_VALUE,
                                   0);
        if (ret < 0) {
            LOG_WARN("Parser flush failed: %s", av_err2str(ret));
            break;
        }

        if (out_size <= 0 || !out_data) {
            break;
        }

        int flags = player->parser_ctx->key_frame ? AV_PKT_FLAG_KEY : 0;
        int64_t pts = player->parser_ctx->pts;
        int64_t dts = player->parser_ctx->dts;

        delivery_result_t res = forward_parsed_access_unit(player,
                                                            out_data,
                                                            out_size,
                                                            pts,
                                                            dts,
                                                            flags,
                                                            -1,
                                                            start_time,
                                                            packets_processed,
                                                            max_packets,
                                                            consecutive_fails,
                                                            total_packets_sent,
                                                            packet_count,
                                                            stage_tag);
        if (res != DELIVERY_CONTINUE) {
            return res;
        }
    }

    return DELIVERY_CONTINUE;
}
static delivery_result_t forward_through_aud(ffmpeg_v4l2_player_t *player,
                                             int64_t start_time,
                                             int *packets_processed,
                                             int *max_packets,
                                             int *consecutive_fails,
                                             uint64_t *total_packets_sent,
                                             int *packet_count,
                                             const char *origin_tag,
                                             const char *final_stage_tag) {
    if (!player || !player->packet) {
        return DELIVERY_CONTINUE;
    }

    if (player->use_aud_bsf && player->bsf_ctx_aud) {
        int aud_ret = av_bsf_send_packet(player->bsf_ctx_aud, player->packet);
        if (aud_ret < 0) {
            LOG_ERROR("AUD BSF send failed: %s", av_err2str(aud_ret));
            av_packet_unref(player->packet);
            return (aud_ret == AVERROR(EINVAL) || aud_ret == AVERROR_INVALIDDATA) ? DELIVERY_CONTINUE : DELIVERY_CONTINUE;
        }

        av_packet_unref(player->packet);

        // Process ONE packet at a time to maintain send/receive balance
        aud_ret = av_bsf_receive_packet(player->bsf_ctx_aud, player->packet);
        if (aud_ret == 0) {
#if FFMPEG_V4L2_DEBUG_BSF
            static int aud_dump_count = 0;
            if (aud_dump_count < 4) {
                int types[8] = {0};
                int n = scan_nal_types(player->packet->data, player->packet->size, 8, types);
                LOG_DEBUG("[AUD] out #%d: size=%d first64=", aud_dump_count + 1, player->packet->size);
                const int bytes_to_dump = player->packet->size < 64 ? player->packet->size : 64;
                char hexbuf[3 * 64 + 1] = {0};
                int off = 0;
                for (int i = 0; i < bytes_to_dump; i++) {
                    off += snprintf(hexbuf + off, sizeof(hexbuf) - (size_t)off, "%02X ", player->packet->data[i]);
                }
                LOG_DEBUG("%s", hexbuf);
                if (n > 0) {
                    char nalstr[64] = {0};
                    off = 0;
                    for (int i = 0; i < n && i < 8; i++) {
                        off += snprintf(nalstr + off, sizeof(nalstr) - (size_t)off, "%d%s", types[i], (i + 1 < n && i < 7) ? "," : "");
                    }
                    LOG_DEBUG("[AUD] NAL sequence (first %d): %s", n, nalstr);
                }
                aud_dump_count++;
            }
#endif

            delivery_result_t result = dispatch_packet_to_decoder(player,
                                                                  player->packet,
                                                                  start_time,
                                                                  packets_processed,
                                                                  max_packets,
                                                                  consecutive_fails,
                                                                  total_packets_sent,
                                                                  packet_count,
                                                                  final_stage_tag ? final_stage_tag : "AUD");
            if (result != DELIVERY_CONTINUE) {
                return result;
            }
        }

        if (aud_ret == AVERROR(EAGAIN)) {
            return DELIVERY_CONTINUE;
        }
        if (aud_ret != AVERROR_EOF && aud_ret < 0) {
            LOG_ERROR("AUD BSF receive failed: %s", av_err2str(aud_ret));
        }
        return DELIVERY_CONTINUE;
    }

    return dispatch_packet_to_decoder(player,
                                      player->packet,
                                      start_time,
                                      packets_processed,
                                      max_packets,
                                      consecutive_fails,
                                      total_packets_sent,
                                      packet_count,
                                      origin_tag);
}

#undef STAGE_TAG

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
#if FFMPEG_V4L2_DEBUG_BSF
    log_memory_usage("Before switching to software decoder");
#endif

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

    reset_parser_state(player);

    // Reset flags and state appropriate for software decoding
    player->fatal_error = false;
    player->extradata_injected = true; // not needed for software

    return true;
}

// Declare external shader program and uniforms (from pickle.c)
extern GLuint g_nv12_shader_program;
extern GLint g_nv12_u_texture_y_loc;
extern GLint g_nv12_u_texture_uv_loc;

// Declare keystone state (from keystone.h)
#include "keystone.h"

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
    player->bsf_ctx_filter_units = NULL;
    player->bsf_ctx_aud = NULL;
    player->use_annexb_bsf = false;
    player->use_filter_units_bsf = false;
    player->use_aud_bsf = false;
    player->seen_idr = false;
    player->stream_time_base = video_stream->time_base;
    player->frame_duration = 0;
    player->last_valid_pts = AV_NOPTS_VALUE;
    
    // Calculate FPS - use actual stream values, with better fallback
    if (video_stream->avg_frame_rate.den != 0) {
        player->fps = av_q2d(video_stream->avg_frame_rate);
    } else if (video_stream->r_frame_rate.den != 0) {
        player->fps = av_q2d(video_stream->r_frame_rate);
    } else {
        // Calculate from time_base as last resort
        AVRational tb = video_stream->time_base;
        if (tb.num > 0 && tb.den > 0) {
            player->fps = (double)tb.den / (double)tb.num;
        } else {
            // Only use fallback if absolutely necessary - changed from 30.0 to 60.0
            LOG_WARN("Unable to determine video FPS, using 60 FPS fallback");
            player->fps = 60.0; // Modern videos are often 60fps
        }
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

    if (player->frame_duration <= 0) {
        AVRational guessed_rate = av_guess_frame_rate(player->format_ctx, video_stream, NULL);
        if (guessed_rate.num > 0 && guessed_rate.den > 0) {
            int64_t duration = av_rescale_q(1, av_inv_q(guessed_rate), player->stream_time_base);
            if (duration > 0) {
                player->frame_duration = duration;
            }
        }
    }

    if (player->frame_duration <= 0 && player->codec_ctx->framerate.num > 0 && player->codec_ctx->framerate.den > 0) {
        int64_t duration = av_rescale_q(1, av_inv_q(player->codec_ctx->framerate), player->stream_time_base);
        if (duration > 0) {
            player->frame_duration = duration;
        }
    }

    if (player->frame_duration <= 0 && player->fps > 0.0) {
        AVRational fps_q = av_d2q(player->fps, 1000);
        if (fps_q.num > 0 && fps_q.den > 0) {
            int64_t duration = av_rescale_q(1, av_inv_q(fps_q), player->stream_time_base);
            if (duration > 0) {
                player->frame_duration = duration;
            }
        }
    }

    if (player->frame_duration <= 0) {
        AVRational fallback = av_make_q(1, 30);
        int64_t duration = av_rescale_q(1, fallback, player->stream_time_base);
        if (duration <= 0) {
            duration = 1;
        }
        player->frame_duration = duration;
    }

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

                        AVBSFContext *annexb_out_ctx = player->bsf_ctx;

                        const AVBitStreamFilter *filter_units_bsf = av_bsf_get_by_name("filter_units");
                        if (filter_units_bsf) {
                            int filter_ret = av_bsf_alloc(filter_units_bsf, &player->bsf_ctx_filter_units);
                            if (filter_ret == 0 && player->bsf_ctx_filter_units) {
                                filter_ret = avcodec_parameters_copy(player->bsf_ctx_filter_units->par_in,
                                                                      player->bsf_ctx->par_out);
                                if (filter_ret == 0) {
                                    player->bsf_ctx_filter_units->time_base_in = player->bsf_ctx->time_base_out;
                                    av_opt_set(player->bsf_ctx_filter_units->priv_data, "remove_types", "6|9", 0);
                                    filter_ret = av_bsf_init(player->bsf_ctx_filter_units);
                                    if (filter_ret == 0) {
                                        player->use_filter_units_bsf = true;
                                        annexb_out_ctx = player->bsf_ctx_filter_units;
                                        LOG_INFO("Chained filter_units bitstream filter with remove_types=6");
                                    } else {
                                        LOG_WARN("Failed to initialize filter_units bitstream filter: %s", av_err2str(filter_ret));
                                        av_bsf_free(&player->bsf_ctx_filter_units);
                                        player->bsf_ctx_filter_units = NULL;
                                    }
                                } else {
                                    LOG_WARN("Failed to copy codec parameters to filter_units: %s", av_err2str(filter_ret));
                                    av_bsf_free(&player->bsf_ctx_filter_units);
                                    player->bsf_ctx_filter_units = NULL;
                                }
                            } else {
                                LOG_WARN("Failed to allocate filter_units bitstream filter (ret=%d)", filter_ret);
                            }
                        } else {
                            LOG_WARN("filter_units bitstream filter not available; SEI messages will pass through");
                        }

                        // Chain an AUD inserter after mp4->annexb: use h264_metadata with aud=insert
                        const AVBitStreamFilter *aud_bsf = av_bsf_get_by_name("h264_metadata");
                        if (aud_bsf) {
                            int aud_ret = av_bsf_alloc(aud_bsf, &player->bsf_ctx_aud);
                            if (aud_ret == 0 && player->bsf_ctx_aud) {
                                AVCodecParameters *source_par = annexb_out_ctx ? annexb_out_ctx->par_out : player->bsf_ctx->par_out;
                                AVRational source_time_base = annexb_out_ctx ? annexb_out_ctx->time_base_out : player->bsf_ctx->time_base_out;
                                aud_ret = avcodec_parameters_copy(player->bsf_ctx_aud->par_in, source_par);
                                if (aud_ret == 0) {
                                    player->bsf_ctx_aud->time_base_in = source_time_base;
                                    av_opt_set(player->bsf_ctx_aud->priv_data, "aud", "insert", 0);
                                    aud_ret = av_bsf_init(player->bsf_ctx_aud);
                                    if (aud_ret == 0) {
                                        player->use_aud_bsf = true;
                                        LOG_INFO("Chained h264_metadata bitstream filter with aud=insert");
                                    } else {
                                        LOG_WARN("Failed to initialize h264_metadata (aud insert): %s", av_err2str(aud_ret));
                                        av_bsf_free(&player->bsf_ctx_aud);
                                        player->bsf_ctx_aud = NULL;
                                    }
                                } else {
                                    LOG_WARN("Failed to copy codec parameters to AUD BSF: %s", av_err2str(aud_ret));
                                    av_bsf_free(&player->bsf_ctx_aud);
                                    player->bsf_ctx_aud = NULL;
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
    
    // Additional optimizations for smoother playback
    player->codec_ctx->extra_hw_frames = 16;  // Extra hardware frames for smoother playback
    
    // Open codec (V4L2 M2M will negotiate pixel format during open)
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "num_capture_buffers", "32", 0);  // More buffers for V4L2
    av_dict_set(&opts, "num_output_buffers", "8", 0);    // Output buffers for V4L2
    av_dict_set(&opts, "async_depth", "3", 0);           // Optimize for parallel processing
    
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
    
#if FFMPEG_V4L2_DEBUG_BSF
    log_memory_usage("After codec opened");
#endif
    
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

    if (player->codec_ctx->codec_id == AV_CODEC_ID_H264) {
        player->parser_ctx = av_parser_init(AV_CODEC_ID_H264);
        if (!player->parser_ctx) {
            LOG_WARN("Failed to initialize H.264 parser; continuing without AU aggregation");
        } else {
            player->au_packet = av_packet_alloc();
            if (!player->au_packet) {
                LOG_WARN("Failed to allocate parser output packet; continuing without AU aggregation");
                av_parser_close(player->parser_ctx);
                player->parser_ctx = NULL;
            }
        }
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
    
#if FFMPEG_V4L2_DEBUG_BSF
    log_memory_usage("After NV12 buffer allocated");
#endif
    
    // Create OpenGL textures for NV12 format
    glGenTextures(1, &player->y_texture);
    glGenTextures(1, &player->uv_texture);
    
    // Setup Y texture (luminance) - use GL_R8 for GLES 3.x
    glBindTexture(GL_TEXTURE_2D, player->y_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)player->width, (GLsizei)player->height, 
                 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Setup UV texture (chrominance) - use GL_RG8 for GLES 3.x
    glBindTexture(GL_TEXTURE_2D, player->uv_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, (GLsizei)player->width / 2, 
                 (GLsizei)player->height / 2, 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    LOG_INFO("Created GL textures: Y=%u (R8 %ux%u), UV=%u (RG8 %ux%u)",
             player->y_texture, player->width, player->height,
             player->uv_texture, player->width/2, player->height/2);
    
    // Create VBO/VAO for optimized rendering (one-time setup)
    glGenVertexArrays(1, &player->vao);
    glGenBuffers(1, &player->vbo);
    
    // Bind VAO first, then configure VBO
    glBindVertexArray(player->vao);
    glBindBuffer(GL_ARRAY_BUFFER, player->vbo);
    
    // Fullscreen quad vertices (position + texcoord interleaved)
    // For TRIANGLE_STRIP: TL -> TR -> BL -> BR creates two triangles correctly
    static const float vertices[] = {
        -1.0f,  1.0f, 0.0f, 1.0f,  // Vertex 0: Top-left
         1.0f,  1.0f, 1.0f, 1.0f,  // Vertex 1: Top-right
        -1.0f, -1.0f, 0.0f, 0.0f,  // Vertex 2: Bottom-left
         1.0f, -1.0f, 1.0f, 0.0f   // Vertex 3: Bottom-right
    };
    
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    // Configure vertex attributes
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Unbind
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    
    LOG_INFO("Created VBO/VAO for optimized rendering: VAO=%u, VBO=%u", player->vao, player->vbo);
    
    // Note: Using global uniform locations (g_nv12_u_texture_y_loc, g_nv12_u_texture_uv_loc)
    // that were cached when the NV12 shader was initialized in pickle.c
    
    // Bind textures to texture units once (state persists between frames)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, player->y_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, player->uv_texture);
    LOG_INFO("Bound textures to units 0 and 1 (persistent state)");
    
    player->initialized = true;
    LOG_INFO("FFmpeg V4L2 player initialized successfully");
    
    // NOTE: Removed decoder priming - the BSF chain (h264_mp4toannexb + filter_units + h264_metadata)
    // automatically handles SPS/PPS injection on the first keyframe. Priming was causing issues
    // because seeking back after priming would restart the stream without re-sending parameter sets.
    
    // Start decode thread if threaded decoding is enabled
    if (player->use_threaded_decoding) {
        if (ffmpeg_v4l2_start_decode_thread(player)) {
            LOG_INFO("Decode thread started successfully");
        } else {
            LOG_ERROR("Failed to start decode thread, falling back to synchronous decoding");
            player->use_threaded_decoding = false;
        }
    }
    
    LOG_INFO("V4L2 decoder initialized, ready for playback");
    return true;
}

static bool init_frame_queue(ffmpeg_v4l2_player_t *player);
static bool push_frame_to_queue(ffmpeg_v4l2_player_t *player, AVFrame *frame);
static AVFrame* pop_frame_from_queue(ffmpeg_v4l2_player_t *player);

/**
 * Get next frame from decoder
 * Based on video_decoder.c - limits work per call to avoid blocking
 */

/**
 * Performs a more aggressive reset when parser resets aren't solving the problem
 * This does a full codec close and reopen to completely refresh the decoder state
 */
static bool deep_reset_codec(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->codec_ctx) {
        return false;
    }
    
    LOG_ERROR("[V4L2] Performing deep codec reset - closing and reopening codec");
    
    // Save important state
    AVBufferRef *hw_device_ctx = NULL;
    if (player->codec_ctx->hw_device_ctx) {
        hw_device_ctx = av_buffer_ref(player->codec_ctx->hw_device_ctx);
    }
    
    // Close codec and parser
    if (player->parser_ctx) {
        av_parser_close(player->parser_ctx);
        player->parser_ctx = NULL;
    }
    
    // Instead of closing the codec with avcodec_close, use avcodec_flush_buffers
    // followed by reopening to achieve a similar effect without version compatibility issues
    avcodec_flush_buffers(player->codec_ctx);
    
    // Re-open the codec with fresh state
    int ret = avcodec_open2(player->codec_ctx, player->codec, NULL);
    if (ret < 0) {
        LOG_ERROR("[V4L2] Failed to reopen codec during deep reset: %s", av_err2str(ret));
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        player->fatal_error = true;
        return false;
    }
    
    // Restore hardware context if it existed
    if (hw_device_ctx) {
        if (player->codec_ctx->hw_device_ctx) {
            av_buffer_unref(&player->codec_ctx->hw_device_ctx);
        }
        player->codec_ctx->hw_device_ctx = hw_device_ctx;
    }
    
    // Reinitialize parser
    if (player->codec_ctx->codec_id == AV_CODEC_ID_H264) {
        player->parser_ctx = av_parser_init(AV_CODEC_ID_H264);
        if (!player->parser_ctx) {

            LOG_ERROR("[V4L2] Failed to initialize parser after deep reset");
            player->fatal_error = true;
            return false;
        }
    }
    
    // Reset counters and state
    consecutive_parser_resets = 0;
    
    // Reset the stream state
    player->last_valid_pts = AV_NOPTS_VALUE;
    
    LOG_INFO("[V4L2] Deep reset completed successfully");
    return true;
}

bool ffmpeg_v4l2_get_frame(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->initialized) {
        return false;
    }
    
    int64_t start_time = get_time_us();
    
    // Handle threaded decoding mode first
    if (player->use_threaded_decoding && player->thread_running) {
        // Try to get a frame from the queue
        AVFrame *queued_frame = pop_frame_from_queue(player);
        if (queued_frame) {
            // If we had a previous frame, release it
            if (player->frame) {
                av_frame_unref(player->frame);
            } else {
                // First time, allocate the frame
                player->frame = av_frame_alloc();
                if (!player->frame) {
                    av_frame_free(&queued_frame);
                    return false;
                }
            }
            
            // Move the data from queued frame to player frame
            av_frame_move_ref(player->frame, queued_frame);
            av_frame_free(&queued_frame);
            
            // Update stats
            player->frames_decoded++;
            int64_t decode_time = get_time_us() - start_time;
            player->decode_time_avg = (player->decode_time_avg * 0.9) +
                                      ((double)decode_time / 1000.0 * 0.1);
                                      
            player->texture_valid = false;  // Will need to upload to GL
            return true;
        }
        
        // No frame available and at EOF
        if (player->eof_reached) {
            return false;
        }
        
        // No frame available yet but not EOF - will try again later
        return false;
    }
    
    // Non-threaded mode follows original logic
    // Dynamic time budget based on frame rate - at 60 FPS, each frame is ~16.6ms
    const int time_budget_us = player->fps > 50.0 ? 12000 : 8000; // 12ms for 60fps, 8ms for 30fps
    static int initial_budget_logs = 0; // Track limited diagnostics when startup stalls
    static ffmpeg_v4l2_player_t *last_player = NULL;
    static int packet_count = 0;  // Track packets sent for debugging
    static int max_packets = 10;    // Increased from 5 to 10 for better throughput
    static int consecutive_fails = 0;
    static uint64_t total_packets_sent = 0;  // Track total packets sent for early failure detection
    static int64_t first_call_us = 0;   // Watchdog: time when we started decoding this player

    if (player != last_player) {
        last_player = player;
        packet_count = 0;
        max_packets = 10;
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
        
        // Log first few frames to confirm decoding is working
        if (player->frames_decoded <= 5) {
            LOG_INFO("Decoded frame #%lu (PTS: %ld, format: %d, size: %dx%d)", 
                     player->frames_decoded, player->frame->pts, player->frame->format,
                     player->frame->width, player->frame->height);
        }
        
        packet_count = 0;  // Reset packet count on successful frame
        
#if FFMPEG_V4L2_DEBUG_BSF
        // Log memory usage periodically - using time-based throttling now
        log_memory_usage("During decode");
#endif
        
        return true;
    }
    
    // OPTIMIZED: Limit packets per call to prevent blocking - based on video_decoder.c
    int packets_processed = 0;
    
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
        
    LOG_WARN("V4L2 M2M decoder failed to produce first frame after %" PRIu64 " packets", (uint64_t)total_packets_sent);
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
        int64_t elapsed_us = get_time_us() - start_time;
        if (elapsed_us >= time_budget_us) {
            if (g_debug) {
                LOG_DEBUG("[V4L2] Time budget hit after %d packets; returning to main loop", packets_processed);
            }
            if (initial_budget_logs < 6 && player->frames_decoded == 0 && player->seen_keyframe) {
                LOG_INFO("[V4L2] Decoder busy after keyframe (elapsed=%.2f ms, packets=%d, total=%" PRIu64 ")",
                         (double)elapsed_us / 1000.0,
                         packets_processed,
                         total_packets_sent);
                initial_budget_logs++;
            }
            return false; // Yield back to main loop; try again next iteration
        }
        
        // Read next packet
        ret = av_read_frame(player->format_ctx, player->packet);
        
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                player->eof_reached = true;
                LOG_INFO("EOF reached in ffmpeg_v4l2_player - %" PRIu64 " frames decoded", (uint64_t)player->frames_decoded);
                if (g_debug) LOG_DEBUG("[V4L2] EOF reached, sending flush packet");

                if (player->use_annexb_bsf && player->bsf_ctx) {
                    int bsf_flush = av_bsf_send_packet(player->bsf_ctx, NULL);
                    if (bsf_flush >= 0 || bsf_flush == AVERROR_EOF) {
                        while ((bsf_flush = av_bsf_receive_packet(player->bsf_ctx, player->packet)) == 0) {
                            if (player->use_filter_units_bsf && player->bsf_ctx_filter_units) {
                                int filter_flush = av_bsf_send_packet(player->bsf_ctx_filter_units, player->packet);
                                if (filter_flush < 0 && filter_flush != AVERROR_EOF) {
                                    LOG_WARN("filter_units flush send failed: %s", av_err2str(filter_flush));
                                    av_packet_unref(player->packet);
                                    continue;
                                }
                                av_packet_unref(player->packet);

                                while ((filter_flush = av_bsf_receive_packet(player->bsf_ctx_filter_units, player->packet)) == 0) {
                                    delivery_result_t flush_result = forward_through_aud(player,
                                                                                        start_time,
                                                                                        &packets_processed,
                                                                                        &max_packets,
                                                                                        &consecutive_fails,
                                                                                        &total_packets_sent,
                                                                                        &packet_count,
                                                                                        "FILTER",
                                                                                        player->use_aud_bsf ? "AUD" : "FILTER");
                                    if (flush_result == DELIVERY_FRAME_READY) {
                                        return true;
                                    }
                                    if (flush_result == DELIVERY_FATAL) {
                                        return false;
                                    }
                                }
                                if (filter_flush != AVERROR_EOF && filter_flush != AVERROR(EAGAIN)) {
                                    LOG_WARN("filter_units flush receive failed: %s", av_err2str(filter_flush));
                                }
                                continue;
                            }

                            delivery_result_t flush_result = forward_through_aud(player,
                                                                                   start_time,
                                                                                   &packets_processed,
                                                                                   &max_packets,
                                                                                   &consecutive_fails,
                                                                                   &total_packets_sent,
                                                                                   &packet_count,
                                                                                   "ANNEXB",
                                                                                   player->use_aud_bsf ? "AUD" : "ANNEXB");
                            if (flush_result == DELIVERY_FRAME_READY) {
                                return true;
                            }
                            if (flush_result == DELIVERY_FATAL) {
                                return false;
                            }
                        }
                        if (bsf_flush != AVERROR_EOF && bsf_flush != AVERROR(EAGAIN)) {
                            LOG_WARN("Unexpected bitstream filter flush status: %s", av_err2str(bsf_flush));
                        }
                    } else {
                        LOG_WARN("Bitstream filter flush send failed: %s", av_err2str(bsf_flush));
                    }
                }
                
                delivery_result_t parser_flush = flush_parser_output(player,
                                                                       start_time,
                                                                       &packets_processed,
                                                                       &max_packets,
                                                                       &consecutive_fails,
                                                                       &total_packets_sent,
                                                                       &packet_count,
                                                                       player->use_aud_bsf ? "AUD" : "ANNEXB");
                if (parser_flush == DELIVERY_FRAME_READY) {
                    return true;
                }
                if (parser_flush == DELIVERY_FATAL) {
                    return false;
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

            // Receive ONE BSF output for this input to maintain balance between send and receive operations
            bsf_ret = av_bsf_receive_packet(player->bsf_ctx, player->packet);
            if (bsf_ret == 0) {
#if FFMPEG_V4L2_DEBUG_BSF
                static int bsf_dump_count = 0;
                if (bsf_dump_count < 4) {
                    int types[8] = {0};
                    int n = scan_nal_types(player->packet->data, player->packet->size, 8, types);
                    LOG_DEBUG("[BSF] out #%d: size=%d first64=", bsf_dump_count + 1, player->packet->size);
                    const int bytes_to_dump = player->packet->size < 64 ? player->packet->size : 64;
                    char hexbuf[3 * 64 + 1] = {0};
                    int off = 0;
                    for (int i = 0; i < bytes_to_dump; i++) {
                        off += snprintf(hexbuf + off, sizeof(hexbuf) - (size_t)off, "%02X ", player->packet->data[i]);
                    }
                    LOG_DEBUG("%s", hexbuf);
                    if (n > 0) {
                        char nalstr[64] = {0};
                        off = 0;
                        for (int i = 0; i < n && i < 8; i++) {
                            off += snprintf(nalstr + off, sizeof(nalstr) - (size_t)off, "%d%s", types[i], (i + 1 < n && i < 7) ? "," : "");
                        }
                        LOG_DEBUG("[BSF] NAL sequence (first %d): %s", n, nalstr);
                    }
                    bsf_dump_count++;
                }
#endif

                if (player->use_filter_units_bsf && player->bsf_ctx_filter_units) {
                    int filter_ret = av_bsf_send_packet(player->bsf_ctx_filter_units, player->packet);
                    if (filter_ret < 0) {
                        LOG_ERROR("filter_units send failed: %s", av_err2str(filter_ret));
                        av_packet_unref(player->packet);
                        continue;
                    }

                    av_packet_unref(player->packet);

                    // Receive ONE filter_units output packet for this input to maintain balance
                    // between send and receive operations
                    filter_ret = av_bsf_receive_packet(player->bsf_ctx_filter_units, player->packet);
                    if (filter_ret == 0) {
#if FFMPEG_V4L2_DEBUG_BSF
                        static int filter_dump_count = 0;
                        if (filter_dump_count < 4) {
                            int types[8] = {0};
                            int n = scan_nal_types(player->packet->data, player->packet->size, 8, types);
                            LOG_DEBUG("[FILTER] out #%d: size=%d first64=", filter_dump_count + 1, player->packet->size);
                            const int bytes_to_dump = player->packet->size < 64 ? player->packet->size : 64;
                            char hexbuf[3 * 64 + 1] = {0};
                            int off = 0;
                            for (int i = 0; i < bytes_to_dump; i++) {
                                off += snprintf(hexbuf + off, sizeof(hexbuf) - (size_t)off, "%02X ", player->packet->data[i]);
                            }
                            LOG_DEBUG("%s", hexbuf);
                            if (n > 0) {
                                char nalstr[64] = {0};
                                off = 0;
                                for (int i = 0; i < n && i < 8; i++) {
                                    off += snprintf(nalstr + off, sizeof(nalstr) - (size_t)off, "%d%s", types[i], (i + 1 < n && i < 7) ? "," : "");
                                }
                                LOG_DEBUG("[FILTER] NAL sequence (first %d): %s", n, nalstr);
                            }
                            filter_dump_count++;
                        }
#endif

                        delivery_result_t result = forward_through_aud(player,
                                                                         start_time,
                                                                         &packets_processed,
                                                                         &max_packets,
                                                                         &consecutive_fails,
                                                                         &total_packets_sent,
                                                                         &packet_count,
                                                                         "FILTER",
                                                                         player->use_aud_bsf ? "AUD" : "FILTER");
                        if (result == DELIVERY_FRAME_READY) {
                            return true;
                        }
                        if (result == DELIVERY_FATAL) {
                            return false;
                        }
                        
                        // Don't continue to the outer loop after successful processing
                        // Just break from the filter_units loop and continue with the bsf loop
                        break;
                    }

                    if (filter_ret == AVERROR(EAGAIN)) {
                        // We've tried to get one packet out of the filter_units BSF for this input
                        // If we get EAGAIN, we should move on to the next packet
                        break;
                    }
                    if (filter_ret != AVERROR_EOF && filter_ret < 0) {
                        LOG_WARN("filter_units receive failed: %s", av_err2str(filter_ret));
                    }
                    // Done with filter_units processing for this input packet
                    break;
                }

                delivery_result_t result = forward_through_aud(player,
                                                                 start_time,
                                                                 &packets_processed,
                                                                 &max_packets,
                                                                 &consecutive_fails,
                                                                 &total_packets_sent,
                                                                 &packet_count,
                                                                 "ANNEXB",
                                                                 player->use_aud_bsf ? "AUD" : "ANNEXB");
                if (result == DELIVERY_FRAME_READY) {
                    return true;
                }
                if (result == DELIVERY_FATAL) {
                    return false;
                }
            }
            if (bsf_ret == AVERROR(EAGAIN)) {
                // Track EAGAIN responses to detect getting stuck
                consecutive_eagain++;
                total_eagain++;
                
                // Update the max consecutive EAGAIN count seen
                if (consecutive_eagain > max_eagain_sequence) {
                    max_eagain_sequence = consecutive_eagain;
                }
                
                // Log periodically to avoid flooding
                if (g_debug && (consecutive_eagain < 10 || consecutive_eagain % 20 == 0)) {
                    LOG_DEBUG("[V4L2] Bitstream filter needs more packets (EAGAIN) - count: %d/%d (max: %d)",
                             consecutive_eagain, total_eagain, max_eagain_sequence);
                }
                
                // If we get too many EAGAINs in a row, something is wrong with the parser state
                if (consecutive_eagain >= 50) {  // Reduced from 200 to 50 to be more aggressive
                    LOG_WARN("[V4L2] Parser stuck in EAGAIN loop for %d iterations (total: %d), forcing reset",
                             consecutive_eagain, total_eagain);
                    reset_parser_state(player);
                    consecutive_eagain = 0;
                    return false;
                }
                
                // Even more aggressive measure for severe hangs
                if (consecutive_eagain >= 100) {
                    LOG_ERROR("[V4L2] Critical parser hang detected after %d consecutive EAGAIN responses",
                              consecutive_eagain);
                    // Try to restart the entire decoder as a last resort
                    if (player->codec_ctx) {
                        LOG_ERROR("[V4L2] Flushing and reopening codec as last resort");
                        avcodec_flush_buffers(player->codec_ctx);
                        avcodec_open2(player->codec_ctx, player->codec, NULL);
                        consecutive_eagain = 0;
                        total_eagain = 0;
                        player->fatal_error = false;  // Reset the error state to try again
                        return false;
                    }
                }
                
                // Also detect if total EAGAINs is growing too fast without progress
                if (total_eagain - last_total_eagain > 200) {
                    LOG_WARN("[V4L2] Too many total EAGAIN responses (%d) without progress, forcing reset",
                             total_eagain);
                    reset_parser_state(player);
                    last_total_eagain = total_eagain;
                    consecutive_eagain = 0;
                    return false;
                }
                
                continue;
            } else if (bsf_ret < 0 && bsf_ret != AVERROR_EOF) {
                // Reset the counter when we get something other than EAGAIN
                consecutive_eagain = 0;
                LOG_ERROR("Bitstream filter receive failed: %s", av_err2str(bsf_ret));
                continue;
            }
        } else {
            // No Annex-B BSF in use: send input packet directly (with optional inline avcC->Annex-B conversion)
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
                            LOG_DEBUG("[V4L2] Decoded frame %" PRIu64 " while draining: %dx%d format=%s (%d)",
                                      (uint64_t)(player->frames_decoded + 1),
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
            delivery_result_t result = dispatch_packet_to_decoder(player,
                                                                  player->packet,
                                                                  start_time,
                                                                  &packets_processed,
                                                                  &max_packets,
                                                                  &consecutive_fails,
                                                                  &total_packets_sent,
                                                                  &packet_count,
                                                                  "ANNEXB");
            if (result == DELIVERY_FRAME_READY) {
                return true;
            }
            if (result == DELIVERY_FATAL) {
                return false;
            }
        }
    }
    
    return false;
}

/**
 * Copy frame data to NV12 buffer with proper format handling
 */
static bool copy_frame_to_nv12_buffer(AVFrame *frame, ffmpeg_v4l2_player_t *player,
                                     const uint8_t **y_plane_out, 
                                     const uint8_t **uv_plane_out) {
    if (!frame || !player || !y_plane_out || !uv_plane_out) {
        return false;
    }

    enum AVPixelFormat fmt = frame->format;
    int width = frame->width;
    int height = frame->height;
    
    // Debug: Log format on first few frames
    static int format_log_count = 0;
    if (format_log_count < 3) {
        LOG_INFO("Frame format: %d (%s), size: %dx%d, linesize: [%d, %d, %d]",
                 fmt, av_get_pix_fmt_name(fmt), width, height,
                 frame->linesize[0], frame->linesize[1], frame->linesize[2]);
        format_log_count++;
    }
    
    // Calculate buffer sizes
    size_t y_plane_size = width * height;
    size_t uv_plane_size = width * height / 2;
    size_t required_size = y_plane_size + uv_plane_size;
    
    // Ensure our buffer is big enough
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
    static uint64_t last_frame_count = UINT64_MAX;
    
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
    
    // Debug: Check first few pixels to verify we have valid data
    if (player->frames_rendered < 3 && y_plane && uv_plane) {
        LOG_INFO("Frame #%lu Y samples: [0]=%u [100]=%u [1000]=%u, UV samples: [0]=%u,%u [100]=%u,%u",
                 player->frames_rendered + 1,
                 y_plane[0], y_plane[100], y_plane[1000],
                 uv_plane[0], uv_plane[1], uv_plane[100], uv_plane[101]);
    }
    
    // Only upload textures if we have valid data
    if (y_plane && uv_plane) {
        // Upload Y plane - use GL_RED for GLES 3.x
        glBindTexture(GL_TEXTURE_2D, player->y_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        frame->width, frame->height,
                        GL_RED, GL_UNSIGNED_BYTE,
                        y_plane);
    
        // Upload UV plane (NV12 format has interleaved U and V) - use GL_RG for GLES 3.x
        glBindTexture(GL_TEXTURE_2D, player->uv_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        frame->width / 2, frame->height / 2,
                        GL_RG, GL_UNSIGNED_BYTE,
                        uv_plane);
    }

    // Don't unbind - keep textures bound for rendering
    
    player->texture_valid = true;
    player->frames_rendered++;
    
    // Log first few uploads to confirm rendering path is working
    if (player->frames_rendered <= 5) {
        LOG_INFO("Uploaded frame #%lu to GL (Y tex: %u, UV tex: %u)", 
                 player->frames_rendered, player->y_texture, player->uv_texture);
    }
    
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

    bool keystone_requested = false;
    if (g_keystone.enabled && !should_skip_feature_for_performance("keystone")) {
        keystone_requested = true;
        if (g_keystone_shader_program == 0) {
            if (!init_keystone_shader()) {
                LOG_WARN("Failed to initialize keystone shader, skipping keystone rendering");
                keystone_requested = false;
            }
        }
    }

    if (keystone_requested) {
        int target_w = (int)d->mode.hdisplay;
        int target_h = (int)d->mode.vdisplay;
        if (!ensure_keystone_fbo(target_w, target_h)) {
            LOG_WARN("Failed to set up keystone FBO, rendering without keystone");
            keystone_requested = false;
        }
    }

    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0};
    if (keystone_requested) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glGetIntegerv(GL_VIEWPORT, prev_viewport);
        glBindFramebuffer(GL_FRAMEBUFFER, g_keystone_fbo);
        glViewport(0, 0, g_keystone_fbo_w, g_keystone_fbo_h);
    }
    
    // Clear the framebuffer
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Use NV12 shader program
    glUseProgram(g_nv12_shader_program);
    
    // Textures are already bound at init - just set the uniform samplers
    // Use global uniform locations that were cached when shader was initialized
    static int uniform_log_count = 0;
    if (uniform_log_count < 2) {
        LOG_INFO("Setting uniforms: Y_loc=%d, UV_loc=%d, shader=%u", 
                 g_nv12_u_texture_y_loc, g_nv12_u_texture_uv_loc, g_nv12_shader_program);
        uniform_log_count++;
    }
    
    if (g_nv12_u_texture_y_loc >= 0) glUniform1i(g_nv12_u_texture_y_loc, 0);
    if (g_nv12_u_texture_uv_loc >= 0) glUniform1i(g_nv12_u_texture_uv_loc, 1);

    // Bind textures to the expected units each frame to avoid state drift
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, player->y_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, player->uv_texture);
    
    // TODO: Handle keystone vertex updates
    // For now, just use fullscreen quad always
    // Keystone vertex update code temporarily disabled to debug hang
    
    // Use pre-configured VAO (contains VBO and vertex attribute setup)
    glBindVertexArray(player->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    
    if (keystone_requested) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

        if (!keystone_render_texture(g_keystone_fbo_texture,
                                     (int)d->mode.hdisplay,
                                     (int)d->mode.vdisplay,
                                     false,
                                     false)) {
            LOG_WARN("Keystone rendering failed, falling back to direct presentation");
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(g_nv12_shader_program);
            if (g_nv12_u_texture_y_loc >= 0) glUniform1i(g_nv12_u_texture_y_loc, 0);
            if (g_nv12_u_texture_uv_loc >= 0) glUniform1i(g_nv12_u_texture_uv_loc, 1);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, player->y_texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, player->uv_texture);
            glBindVertexArray(player->vao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
        }

        glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    }

    // Check for GL errors if in debug mode
    if (g_debug) {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            LOG_ERROR("GL error during render: 0x%04x", err);
        }
    }
    
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

    reset_parser_state(player);
    
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
    reset_parser_state(player);
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
    if (player->parser_ctx) {
        LOG_INFO("Closing parser context...");
        av_parser_close(player->parser_ctx);
        player->parser_ctx = NULL;
    }
    if (player->au_packet) {
        LOG_INFO("Freeing parser output packet...");
        av_packet_free(&player->au_packet);
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
    
    // Clean up threading resources if used
    if (player->use_threaded_decoding) {
        ffmpeg_v4l2_stop_decode_thread(player);
        
        // Clean up frame queue
        if (player->frame_queue.mutex) {
            pthread_mutex_t *mutex = (pthread_mutex_t *)player->frame_queue.mutex;
            pthread_mutex_destroy(mutex);
            free(mutex);
            player->frame_queue.mutex = NULL;
        }
        
        if (player->frame_queue.cond) {
            pthread_cond_t *cond = (pthread_cond_t *)player->frame_queue.cond;
            pthread_cond_destroy(cond);
            free(cond);
            player->frame_queue.cond = NULL;
        }
        
        // Free any remaining frames in queue
        for (int i = 0; i < 3; i++) {
            if (player->frame_queue.frames[i]) {
                av_frame_free(&player->frame_queue.frames[i]);
                player->frame_queue.frames[i] = NULL;
            }
        }
    }
    
    LOG_INFO("FFmpeg V4L2 player cleaned up");
}

/**
 * Initialize the frame queue for threaded decoding
 */
static bool init_frame_queue(ffmpeg_v4l2_player_t *player) {
    if (!player) return false;
    
    // Initialize frame queue state
    player->frame_queue.write_idx = 0;
    player->frame_queue.read_idx = 0;
    player->frame_queue.count = 0;
    
    // Clear frame pointers
    for (int i = 0; i < 3; i++) {
        player->frame_queue.frames[i] = NULL;
    }
    
    // Create mutex
    player->frame_queue.mutex = malloc(sizeof(pthread_mutex_t));
    if (!player->frame_queue.mutex) {
        LOG_ERROR("Failed to allocate memory for frame queue mutex");
        return false;
    }
    
    // Create condition variable
    player->frame_queue.cond = malloc(sizeof(pthread_cond_t));
    if (!player->frame_queue.cond) {
        LOG_ERROR("Failed to allocate memory for frame queue condition variable");
        free(player->frame_queue.mutex);
        player->frame_queue.mutex = NULL;
        return false;
    }
    
    // Initialize mutex and condition variable
    pthread_mutex_t *mutex = (pthread_mutex_t *)player->frame_queue.mutex;
    pthread_cond_t *cond = (pthread_cond_t *)player->frame_queue.cond;
    
    if (pthread_mutex_init(mutex, NULL) != 0) {
        LOG_ERROR("Failed to initialize frame queue mutex");
        free(player->frame_queue.mutex);
        free(player->frame_queue.cond);
        player->frame_queue.mutex = NULL;
        player->frame_queue.cond = NULL;
        return false;
    }
    
    if (pthread_cond_init(cond, NULL) != 0) {
        LOG_ERROR("Failed to initialize frame queue condition variable");
        pthread_mutex_destroy(mutex);
        free(player->frame_queue.mutex);
        free(player->frame_queue.cond);
        player->frame_queue.mutex = NULL;
        player->frame_queue.cond = NULL;
        return false;
    }
    
    LOG_INFO("Frame queue initialized successfully");
    return true;
}

/**
 * Push a frame into the queue (for producer thread)
 */
static bool push_frame_to_queue(ffmpeg_v4l2_player_t *player, AVFrame *frame) {
    if (!player || !frame || !player->frame_queue.mutex || !player->frame_queue.cond) {
        return false;
    }
    
    pthread_mutex_t *mutex = (pthread_mutex_t *)player->frame_queue.mutex;
    pthread_cond_t *cond = (pthread_cond_t *)player->frame_queue.cond;
    bool result = false;
    
    pthread_mutex_lock(mutex);
    
    // Wait if queue is full
    while (player->frame_queue.count >= 3 && !player->thread_stop_requested) {
        LOG_DEBUG("Frame queue full, waiting...");
        pthread_cond_wait(cond, mutex);
    }
    
    // Don't push if we're stopping
    if (player->thread_stop_requested) {
        pthread_mutex_unlock(mutex);
        return false;
    }
    
    // If there's already a frame in this slot, free it
    if (player->frame_queue.frames[player->frame_queue.write_idx]) {
        av_frame_free(&player->frame_queue.frames[player->frame_queue.write_idx]);
    }
    
    // Create a new reference to the frame
    player->frame_queue.frames[player->frame_queue.write_idx] = av_frame_clone(frame);
    if (player->frame_queue.frames[player->frame_queue.write_idx]) {
        player->frame_queue.write_idx = (player->frame_queue.write_idx + 1) % 3;
        player->frame_queue.count++;
        result = true;
        
        // Signal that a frame is available
        pthread_cond_signal(cond);
        LOG_DEBUG("Frame pushed to queue (count=%d)", player->frame_queue.count);
    } else {
        LOG_ERROR("Failed to clone frame for queue");
    }
    
    pthread_mutex_unlock(mutex);
    return result;
}

/**
 * Pop a frame from the queue (for consumer thread)
 */
static AVFrame* pop_frame_from_queue(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->frame_queue.mutex || !player->frame_queue.cond) {
        return NULL;
    }
    
    pthread_mutex_t *mutex = (pthread_mutex_t *)player->frame_queue.mutex;
    pthread_cond_t *cond = (pthread_cond_t *)player->frame_queue.cond;
    AVFrame *frame = NULL;
    
    pthread_mutex_lock(mutex);
    
    // Return NULL if queue is empty
    if (player->frame_queue.count == 0) {
        pthread_mutex_unlock(mutex);
        return NULL;
    }
    
    // Get frame and move read index
    frame = player->frame_queue.frames[player->frame_queue.read_idx];
    player->frame_queue.frames[player->frame_queue.read_idx] = NULL;
    player->frame_queue.read_idx = (player->frame_queue.read_idx + 1) % 3;
    player->frame_queue.count--;
    
    // Signal that there's space in the queue
    pthread_cond_signal(cond);
    LOG_DEBUG("Frame popped from queue (count=%d)", player->frame_queue.count);
    
    pthread_mutex_unlock(mutex);
    return frame;
}

/**
 * Decode thread function
 */
static void* decode_thread_func(void *arg) {
    ffmpeg_v4l2_player_t *player = (ffmpeg_v4l2_player_t *)arg;
    if (!player) return NULL;
    
    LOG_INFO("Decode thread started");
    
    AVPacket *packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR("Failed to allocate packet in decode thread");
        return NULL;
    }
    
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("Failed to allocate frame in decode thread");
        av_packet_free(&packet);
        return NULL;
    }
    
    while (!player->thread_stop_requested && !player->eof_reached && !player->fatal_error) {
        // Don't read too many packets if the queue is full
        if (player->frame_queue.count >= 2) {
            usleep(1000); // Reduced from 5000 to 1000 (1ms) for better responsiveness at 60fps
            continue;
        }
        
        // Read a packet
        int ret = av_read_frame(player->format_ctx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_INFO("EOF reached in decode thread");
                player->eof_reached = true;
                break;
            }
            LOG_WARN("Error reading frame: %s", av_err2str(ret));
            continue;
        }
        
        // Skip non-video packets
        if (packet->stream_index != player->video_stream_index) {
            av_packet_unref(packet);
            continue;
        }
        
        // Send packet to decoder
        ret = avcodec_send_packet(player->codec_ctx, packet);
        av_packet_unref(packet);
        
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            LOG_ERROR("Error sending packet to decoder: %s", av_err2str(ret));
            continue;
        }
        
        // Receive frames from decoder
        while (!player->thread_stop_requested) {
            ret = avcodec_receive_frame(player->codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                // Need more input or end of stream
                break;
            } else if (ret < 0) {
                LOG_ERROR("Error receiving frame from decoder: %s", av_err2str(ret));
                break;
            }
            
            // Got a frame, push it to queue
            if (!push_frame_to_queue(player, frame)) {
                LOG_WARN("Failed to push frame to queue");
            }
            
            av_frame_unref(frame);
        }
    }
    
    // Clean up
    av_frame_free(&frame);
    av_packet_free(&packet);
    
    LOG_INFO("Decode thread finished");
    return NULL;
}

bool ffmpeg_v4l2_enable_threaded_decoding(ffmpeg_v4l2_player_t *player) {
    if (!player) return false;
    
    // Can only be called before initialization
    if (player->initialized) {
        LOG_ERROR("Cannot enable threaded decoding after player is initialized");
        return false;
    }
    
    player->use_threaded_decoding = true;
    LOG_INFO("Threaded decoding enabled");
    return true;
}

bool ffmpeg_v4l2_start_decode_thread(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->initialized || !player->use_threaded_decoding) {
        return false;
    }
    
    // Don't start if already running
    if (player->thread_running) {
        LOG_WARN("Decode thread already running");
        return true;
    }
    
    // Initialize frame queue
    if (!init_frame_queue(player)) {
        LOG_ERROR("Failed to initialize frame queue");
        return false;
    }
    
    // Reset thread control flags
    player->thread_stop_requested = false;
    player->thread_running = false;
    
    // Create thread
    pthread_t *thread = malloc(sizeof(pthread_t));
    if (!thread) {
        LOG_ERROR("Failed to allocate memory for decode thread");
        return false;
    }
    
    if (pthread_create(thread, NULL, decode_thread_func, player) != 0) {
        LOG_ERROR("Failed to create decode thread");
        free(thread);
        return false;
    }
    
    player->decode_thread = thread;
    player->thread_running = true;
    LOG_INFO("Decode thread started successfully");
    return true;
}

bool ffmpeg_v4l2_stop_decode_thread(ffmpeg_v4l2_player_t *player) {
    if (!player || !player->use_threaded_decoding) {
        return false;
    }
    
    // Don't stop if not running
    if (!player->thread_running || !player->decode_thread) {
        return true;
    }
    
    // Set stop flag
    player->thread_stop_requested = true;
    
    // Signal condition variables to unblock thread
    if (player->frame_queue.mutex && player->frame_queue.cond) {
        pthread_mutex_t *mutex = (pthread_mutex_t *)player->frame_queue.mutex;
        pthread_cond_t *cond = (pthread_cond_t *)player->frame_queue.cond;
        
        pthread_mutex_lock(mutex);
        pthread_cond_broadcast(cond);
        pthread_mutex_unlock(mutex);
    }
    
    // Wait for thread to exit
    pthread_t *thread = (pthread_t *)player->decode_thread;
    if (thread) {
        pthread_join(*thread, NULL);
        free(thread);
        player->decode_thread = NULL;
    }
    
    player->thread_running = false;
    LOG_INFO("Decode thread stopped");
    return true;
}
