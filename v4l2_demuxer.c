#define _GNU_SOURCE

#include "v4l2_demuxer.h"
#include "log.h"

// Only compile demuxer when enabled
#if defined(USE_V4L2_DECODER) && defined(ENABLE_V4L2_DEMUXER)

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

// Map FFmpeg codec IDs to human readable names
static const char* get_codec_name(int codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264: return "H.264/AVC";
        case AV_CODEC_ID_HEVC: return "H.265/HEVC";
        case AV_CODEC_ID_VP8: return "VP8";
        case AV_CODEC_ID_VP9: return "VP9";
        case AV_CODEC_ID_MPEG2VIDEO: return "MPEG-2";
        case AV_CODEC_ID_MPEG4: return "MPEG-4";
        case AV_CODEC_ID_AV1: return "AV1";
        default: return "Unknown";
    }
}

// Check if codec is supported by V4L2
static bool is_codec_supported(int codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_VP8:
        case AV_CODEC_ID_VP9:
        case AV_CODEC_ID_MPEG2VIDEO:
        case AV_CODEC_ID_MPEG4:
            return true;
        default:
            return false;
    }
}

bool v4l2_demuxer_is_available(void) {
    // Check FFmpeg version and availability
    return true; // FFmpeg is already linked, assume it's available
}

v4l2_demuxer_t* v4l2_demuxer_create(const char *filename, 
                                    v4l2_demuxed_packet_cb callback, 
                                    void *user_data) {
    if (!filename || !callback) {
        LOG_ERROR("V4L2 demuxer: Invalid parameters");
        return NULL;
    }
    
    v4l2_demuxer_t *demuxer = calloc(1, sizeof(v4l2_demuxer_t));
    if (!demuxer) {
        LOG_ERROR("V4L2 demuxer: Failed to allocate memory");
        return NULL;
    }
    
    // Store callback and user data
    demuxer->packet_callback = callback;
    demuxer->user_data = user_data;
    demuxer->video_stream_index = -1;
    
    // Duplicate filename
    demuxer->filename = strdup(filename);
    if (!demuxer->filename) {
        LOG_ERROR("V4L2 demuxer: Failed to duplicate filename");
        free(demuxer);
        return NULL;
    }
    
    // Open input file
    int ret = avformat_open_input(&demuxer->fmt_ctx, filename, NULL, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("V4L2 demuxer: Failed to open %s: %s", filename, errbuf);
        goto error;
    }
    
    // Retrieve stream information
    ret = avformat_find_stream_info(demuxer->fmt_ctx, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("V4L2 demuxer: Failed to find stream info: %s", errbuf);
        goto error;
    }
    
    // Find video stream
    for (unsigned int i = 0; i < demuxer->fmt_ctx->nb_streams; i++) {
        if (demuxer->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            demuxer->video_stream = demuxer->fmt_ctx->streams[i];
            demuxer->video_stream_index = (int)i;
            break;
        }
    }
    
    if (demuxer->video_stream_index == -1) {
        LOG_ERROR("V4L2 demuxer: No video stream found in %s", filename);
        goto error;
    }
    
    AVCodecParameters *codecpar = demuxer->video_stream->codecpar;
    
    // Check if codec is supported
    if (!is_codec_supported(codecpar->codec_id)) {
        LOG_WARN("V4L2 demuxer: Unsupported codec %s (%d)", 
                 get_codec_name(codecpar->codec_id), codecpar->codec_id);
        goto error;
    }
    
    // Fill stream info
    demuxer->stream_info.codec_id = codecpar->codec_id;
    demuxer->stream_info.width = codecpar->width;
    demuxer->stream_info.height = codecpar->height;
    demuxer->stream_info.codec_name = get_codec_name(codecpar->codec_id);
    
    // Calculate duration
    if (demuxer->video_stream->duration != AV_NOPTS_VALUE) {
        demuxer->stream_info.duration = av_rescale_q(demuxer->video_stream->duration,
                                                     demuxer->video_stream->time_base,
                                                     (AVRational){1, AV_TIME_BASE});
    } else {
        demuxer->stream_info.duration = 0;
    }
    
    // Calculate FPS
    AVRational fps = av_guess_frame_rate(demuxer->fmt_ctx, demuxer->video_stream, NULL);
    if (fps.num && fps.den) {
        demuxer->stream_info.fps = (double)fps.num / fps.den;
    } else {
        demuxer->stream_info.fps = 25.0; // Default fallback
    }
    
    demuxer->initialized = true;
    
    LOG_INFO("V4L2 demuxer: Initialized %s - %s %dx%d %.2f fps", 
             filename, demuxer->stream_info.codec_name,
             demuxer->stream_info.width, demuxer->stream_info.height,
             demuxer->stream_info.fps);
    
    return demuxer;
    
error:
    if (demuxer->fmt_ctx) {
        avformat_close_input(&demuxer->fmt_ctx);
    }
    if (demuxer->filename) {
        free(demuxer->filename);
    }
    free(demuxer);
    return NULL;
}

const v4l2_stream_info_t* v4l2_demuxer_get_stream_info(v4l2_demuxer_t *demuxer) {
    if (!demuxer || !demuxer->initialized) {
        return NULL;
    }
    return &demuxer->stream_info;
}

bool v4l2_demuxer_process_packet(v4l2_demuxer_t *demuxer) {
    if (!demuxer || !demuxer->initialized || demuxer->eof_reached) {
        return false;
    }
    
    AVPacket packet;
    int ret = av_read_frame(demuxer->fmt_ctx, &packet);
    
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            demuxer->eof_reached = true;
        } else {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("V4L2 demuxer: Error reading frame: %s", errbuf);
        }
        return false;
    }
    
    // Only process video packets from our target stream
    if (packet.stream_index != demuxer->video_stream_index) {
        av_packet_unref(&packet);
        return true; // Continue processing, just skip this packet
    }
    
    // Convert to our packet structure
    v4l2_demuxed_packet_t demux_packet = {0};
    demux_packet.data = packet.data;
    demux_packet.size = (size_t)packet.size;
    demux_packet.stream_index = packet.stream_index;
    demux_packet.keyframe = (packet.flags & AV_PKT_FLAG_KEY) != 0;
    
    // Convert timestamps from stream timebase to microseconds
    if (packet.pts != AV_NOPTS_VALUE) {
        demux_packet.pts = av_rescale_q(packet.pts, demuxer->video_stream->time_base, (AVRational){1, AV_TIME_BASE});
    } else {
        demux_packet.pts = AV_NOPTS_VALUE;
    }
    
    if (packet.dts != AV_NOPTS_VALUE) {
        demux_packet.dts = av_rescale_q(packet.dts, demuxer->video_stream->time_base, (AVRational){1, AV_TIME_BASE});
    } else {
        demux_packet.dts = AV_NOPTS_VALUE;
    }
    
    // Call user callback
    demuxer->packet_callback(&demux_packet, demuxer->user_data);
    
    av_packet_unref(&packet);
    return true;
}

bool v4l2_demuxer_start_blocking(v4l2_demuxer_t *demuxer) {
    if (!demuxer || !demuxer->initialized) {
        return false;
    }
    
    LOG_INFO("V4L2 demuxer: Starting blocking demux loop");
    
    while (!demuxer->eof_reached) {
        if (!v4l2_demuxer_process_packet(demuxer)) {
            break;
        }
    }
    
    LOG_INFO("V4L2 demuxer: Blocking demux loop finished");
    return true;
}

// Thread function for threaded demuxing
static void* demuxer_thread_func(void *arg) {
    v4l2_demuxer_t *demuxer = (v4l2_demuxer_t*)arg;
    
    LOG_INFO("V4L2 demuxer: Thread started");
    
    while (demuxer->thread_running && !demuxer->thread_stop_requested && !demuxer->eof_reached) {
        if (!v4l2_demuxer_process_packet(demuxer)) {
            break;
        }
        // Small sleep to prevent excessive CPU usage
        usleep(1000); // 1ms
    }
    
    demuxer->thread_running = false;
    LOG_INFO("V4L2 demuxer: Thread finished");
    
    return NULL;
}

bool v4l2_demuxer_start_threaded(v4l2_demuxer_t *demuxer) {
    if (!demuxer || !demuxer->initialized || demuxer->use_threading) {
        return false;
    }
    
    pthread_t *thread = malloc(sizeof(pthread_t));
    if (!thread) {
        LOG_ERROR("V4L2 demuxer: Failed to allocate thread handle");
        return false;
    }
    
    demuxer->thread_handle = thread;
    demuxer->thread_running = true;
    demuxer->thread_stop_requested = false;
    demuxer->use_threading = true;
    
    int ret = pthread_create(thread, NULL, demuxer_thread_func, demuxer);
    if (ret != 0) {
        LOG_ERROR("V4L2 demuxer: Failed to create thread: %s", strerror(ret));
        free(thread);
        demuxer->thread_handle = NULL;
        demuxer->use_threading = false;
        return false;
    }
    
    LOG_INFO("V4L2 demuxer: Started threaded demuxing");
    return true;
}

void v4l2_demuxer_stop(v4l2_demuxer_t *demuxer) {
    if (!demuxer || !demuxer->use_threading) {
        return;
    }
    
    LOG_INFO("V4L2 demuxer: Stopping threaded demuxing");
    
    demuxer->thread_stop_requested = true;
    
    if (demuxer->thread_handle) {
        pthread_t *thread = (pthread_t*)demuxer->thread_handle;
        pthread_join(*thread, NULL);
        free(thread);
        demuxer->thread_handle = NULL;
    }
    
    demuxer->use_threading = false;
    LOG_INFO("V4L2 demuxer: Threaded demuxing stopped");
}

bool v4l2_demuxer_seek(v4l2_demuxer_t *demuxer, int64_t timestamp_us) {
    if (!demuxer || !demuxer->initialized) {
        return false;
    }
    
    // Convert timestamp to stream timebase
    int64_t seek_target = av_rescale_q(timestamp_us, (AVRational){1, AV_TIME_BASE}, demuxer->video_stream->time_base);
    
    int ret = av_seek_frame(demuxer->fmt_ctx, demuxer->video_stream_index, seek_target, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("V4L2 demuxer: Seek failed: %s", errbuf);
        return false;
    }
    
    demuxer->eof_reached = false;
    return true;
}

bool v4l2_demuxer_is_eof(v4l2_demuxer_t *demuxer) {
    return demuxer ? demuxer->eof_reached : true;
}

void v4l2_demuxer_destroy(v4l2_demuxer_t *demuxer) {
    if (!demuxer) {
        return;
    }
    
    LOG_INFO("V4L2 demuxer: Destroying demuxer");
    
    // Stop threading if active
    v4l2_demuxer_stop(demuxer);
    
    // Close FFmpeg resources
    if (demuxer->fmt_ctx) {
        avformat_close_input(&demuxer->fmt_ctx);
    }
    
    // Free filename
    if (demuxer->filename) {
        free(demuxer->filename);
    }
    
    // Free demuxer
    free(demuxer);
}

#endif // USE_V4L2_DECODER && ENABLE_V4L2_DEMUXER