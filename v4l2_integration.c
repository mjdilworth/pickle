#define _GNU_SOURCE
#include "v4l2_integration.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>

// Default bridge queue size
#define DEFAULT_BRIDGE_QUEUE_SIZE 32

v4l2_integration_t* v4l2_integration_create(void) {
    v4l2_integration_t *integration = calloc(1, sizeof(v4l2_integration_t));
    if (!integration) {
        LOG_ERROR("V4L2 integration: Failed to allocate memory");
        return NULL;
    }
    
    LOG_INFO("V4L2 integration created");
    return integration;
}

void v4l2_integration_destroy(v4l2_integration_t *integration) {
    if (!integration) return;
    
    // Stop playback if running
    v4l2_integration_stop_playback(integration);
    
    // Clean up components in reverse order
    if (integration->bridge) {
        v4l2_demux_bridge_destroy(integration->bridge);
        integration->bridge = NULL;
    }
    
    if (integration->demuxer) {
        v4l2_demuxer_destroy(integration->demuxer);
        integration->demuxer = NULL;
    }
    
    if (integration->decoder) {
        v4l2_decoder_destroy(integration->decoder);
        integration->decoder = NULL;
    }
    
    free(integration);
    LOG_INFO("V4L2 integration destroyed");
}

bool v4l2_integration_open_file(v4l2_integration_t *integration, const char *file_path) {
    if (!integration || !file_path) {
        LOG_ERROR("V4L2 integration: Invalid parameters for open_file");
        return false;
    }
    
    // Stop any existing playback
    v4l2_integration_stop_playback(integration);
    
    // Check if this is a container format
    if (!v4l2_integration_is_container_format(file_path)) {
        LOG_INFO("V4L2 integration: File is not a container format: %s", file_path);
        return false;
    }
    
    // First, create the bridge and decoder before the demuxer since the demuxer needs the callback
    
    // Create V4L2 decoder (temporary initialization, will be properly configured later)
    integration->decoder = malloc(sizeof(v4l2_decoder_t));
    if (!integration->decoder) {
        LOG_ERROR("V4L2 integration: Failed to allocate decoder");
        return false;
    }
    
    // Create bridge (will be properly configured after we get stream info)
    integration->bridge = v4l2_demux_bridge_create(integration->decoder, DEFAULT_BRIDGE_QUEUE_SIZE);
    if (!integration->bridge) {
        LOG_ERROR("V4L2 integration: Failed to create demux bridge");
        free(integration->decoder);
        integration->decoder = NULL;
        return false;
    }
    
    // Get callback for demuxer
    v4l2_packet_callback_t callback = v4l2_demux_bridge_get_callback(integration->bridge);
    void *user_data = v4l2_demux_bridge_get_user_data(integration->bridge);
    
    // Create demuxer with callback
    integration->demuxer = v4l2_demuxer_create(file_path, callback, user_data);
    if (!integration->demuxer) {
        LOG_ERROR("V4L2 integration: Failed to create demuxer for: %s", file_path);
        v4l2_demux_bridge_destroy(integration->bridge);
        integration->bridge = NULL;
        free(integration->decoder);
        integration->decoder = NULL;
        return false;
    }
    
    // Get stream information
    const v4l2_stream_info_t *stream_info_ptr = v4l2_demuxer_get_stream_info(integration->demuxer);
    if (!stream_info_ptr) {
        LOG_ERROR("V4L2 integration: Failed to get stream info");
        v4l2_demuxer_destroy(integration->demuxer);
        integration->demuxer = NULL;
        v4l2_demux_bridge_destroy(integration->bridge);
        integration->bridge = NULL;
        free(integration->decoder);
        integration->decoder = NULL;
        return false;
    }
    
    v4l2_stream_info_t stream_info = *stream_info_ptr;
    
    // Check codec support
    if (!v4l2_demux_bridge_is_codec_supported(stream_info.codec_id)) {
        LOG_ERROR("V4L2 integration: Codec not supported by V4L2: %d", stream_info.codec_id);
        v4l2_demuxer_destroy(integration->demuxer);
        integration->demuxer = NULL;
        v4l2_demux_bridge_destroy(integration->bridge);
        integration->bridge = NULL;
        free(integration->decoder);
        integration->decoder = NULL;
        return false;
    }
    
    // Map codec for V4L2
    v4l2_codec_t v4l2_codec = v4l2_demux_bridge_map_codec(stream_info.codec_id);
    LOG_INFO("V4L2 integration: Codec mapping - stream codec_id=%d -> V4L2 codec=%d", stream_info.codec_id, v4l2_codec);
    
    // Initialize V4L2 decoder
    if (!v4l2_decoder_init(integration->decoder, v4l2_codec, 
                           (uint32_t)stream_info.width, (uint32_t)stream_info.height)) {
        LOG_ERROR("V4L2 integration: Failed to initialize V4L2 decoder");
        integration->decoder = NULL; // v4l2_decoder_init frees on failure
        v4l2_demuxer_destroy(integration->demuxer);
        integration->demuxer = NULL;
        v4l2_demux_bridge_destroy(integration->bridge);
        integration->bridge = NULL;
        return false;
    }
    
    // Configure stream in bridge
    v4l2_stream_config_t *config = v4l2_demux_bridge_configure_stream(integration->bridge, &stream_info);
    if (!config || !config->is_supported) {
        LOG_ERROR("V4L2 integration: Stream configuration failed");
        v4l2_decoder_destroy(integration->decoder);
        integration->decoder = NULL;
        v4l2_demuxer_destroy(integration->demuxer);
        integration->demuxer = NULL;
        v4l2_demux_bridge_destroy(integration->bridge);
        integration->bridge = NULL;
        return false;
    }
    
    // Copy stream configuration
    integration->stream_config = *config;
    integration->is_initialized = true;
    
    LOG_INFO("V4L2 integration: Successfully opened file: %s (%dx%d, %.2f fps)", 
             file_path, integration->stream_config.width, 
             integration->stream_config.height, integration->stream_config.fps);
    
    return true;
}

bool v4l2_integration_is_container_format(const char *file_path) {
    if (!file_path) return false;
    
    // Simple file extension check for common container formats
    const char *ext = strrchr(file_path, '.');
    if (!ext) return false;
    
    ext++; // Skip the dot
    
    // Check for common container formats
    return (strcasecmp(ext, "mp4") == 0 ||
            strcasecmp(ext, "mkv") == 0 ||
            strcasecmp(ext, "avi") == 0 ||
            strcasecmp(ext, "mov") == 0 ||
            strcasecmp(ext, "webm") == 0);
}

bool v4l2_integration_start_playback(v4l2_integration_t *integration) {
    if (!integration || !integration->is_initialized) {
        LOG_ERROR("V4L2 integration: Not initialized for start_playback");
        return false;
    }
    
    if (integration->is_playing) {
        LOG_WARN("V4L2 integration: Already playing");
        return true;
    }
    
    // Allocate V4L2 decoder buffers
    if (!v4l2_decoder_allocate_buffers(integration->decoder, 8, 8)) {
        LOG_ERROR("V4L2 integration: Failed to allocate V4L2 decoder buffers");
        return false;
    }
    
    // Enable DMA-BUF for zero-copy if available
    if (!v4l2_decoder_use_dmabuf(integration->decoder)) {
        LOG_WARN("V4L2 integration: DMA-BUF not supported, using memory copy");
    }
    
    // Start V4L2 decoder streaming
    if (!v4l2_decoder_start(integration->decoder)) {
        LOG_ERROR("V4L2 integration: Failed to start V4L2 decoder - this is the capture streaming failure");
        LOG_ERROR("V4L2 integration: Decoder state - fd=%d, initialized=%d, streaming=%d", 
                  integration->decoder->fd, 
                  integration->decoder->initialized, 
                  integration->decoder->streaming);
        return false;
    }
    
    // Start demuxer in threaded mode
    if (!v4l2_demuxer_start_threaded(integration->demuxer)) {
        LOG_ERROR("V4L2 integration: Failed to start demuxer");
        v4l2_decoder_stop(integration->decoder);
        return false;
    }
    
    integration->is_playing = true;
    LOG_INFO("V4L2 integration: Playback started");
    return true;
}

void v4l2_integration_stop_playback(v4l2_integration_t *integration) {
    if (!integration || !integration->is_playing) return;
    
    // Stop demuxer first
    if (integration->demuxer) {
        v4l2_demuxer_stop(integration->demuxer);
    }
    
    // Flush bridge
    if (integration->bridge) {
        v4l2_demux_bridge_flush(integration->bridge);
    }
    
    // Stop decoder
    if (integration->decoder) {
        v4l2_decoder_stop(integration->decoder);
    }
    
    integration->is_playing = false;
    LOG_INFO("V4L2 integration: Playback stopped");
}

int v4l2_integration_process(v4l2_integration_t *integration) {
    if (!integration || !integration->is_playing || !integration->bridge) {
        return 0;
    }
    
    // Process available packets (limit to avoid blocking)
    return v4l2_demux_bridge_process_packets(integration->bridge, 10);
}

bool v4l2_integration_is_available(void) {
    // Check if V4L2 decoder is supported
    return v4l2_decoder_is_supported();
}

bool v4l2_integration_has_packets(v4l2_integration_t *integration) {
    if (!integration || !integration->bridge) return false;
    
    return v4l2_demux_bridge_has_packets(integration->bridge);
}

const v4l2_stream_config_t* v4l2_integration_get_stream_config(v4l2_integration_t *integration) {
    if (!integration || !integration->is_initialized) return NULL;
    
    return &integration->stream_config;
}

void v4l2_integration_set_error_callback(v4l2_integration_t *integration,
                                          void (*callback)(const char *error, void *user_data),
                                          void *user_data) {
    if (!integration || !integration->bridge) return;
    
    v4l2_demux_bridge_set_error_callback(integration->bridge, callback, user_data);
}

// Internal implementations