#define _GNU_SOURCE
#include "v4l2_demux_bridge.h"
#include "log.h" // For LOG_* macros
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <libavcodec/avcodec.h>

// Packet queue node
typedef struct packet_node {
    uint8_t *data;               // Packet data (copied)
    size_t size;                 // Packet size
    int64_t pts;                 // Presentation timestamp
    bool is_keyframe;            // Whether this is a keyframe
    struct packet_node *next;    // Next packet in queue
} packet_node_t;

// V4L2 demuxer bridge structure
struct v4l2_demux_bridge {
    v4l2_decoder_t *decoder;     // V4L2 decoder instance
    
    // Packet queue
    packet_node_t *queue_head;   // First packet in queue
    packet_node_t *queue_tail;   // Last packet in queue
    size_t queue_size;           // Current queue size
    size_t max_queue_size;       // Maximum queue size
    
    // Thread synchronization
    pthread_mutex_t queue_mutex; // Queue access mutex
    
    // Statistics
    v4l2_bridge_stats_t stats;   // Bridge statistics
    
    // Error handling
    void (*error_callback)(const char *error, void *user_data);
    void *error_user_data;
    
    // Stream configuration
    v4l2_stream_config_t stream_config;
    bool stream_configured;
};

// Internal packet callback function
static void packet_callback_impl(v4l2_demuxed_packet_t *packet, void *user_data);

// Internal error reporting
static void report_error(v4l2_demux_bridge_t *bridge, const char *format, ...);

// Packet queue management
static bool enqueue_packet(v4l2_demux_bridge_t *bridge, const uint8_t *data, 
                          size_t size, int64_t pts, bool is_keyframe);
static packet_node_t* dequeue_packet(v4l2_demux_bridge_t *bridge);
static void free_packet_node(packet_node_t *node);
static void clear_queue(v4l2_demux_bridge_t *bridge);

v4l2_demux_bridge_t* v4l2_demux_bridge_create(v4l2_decoder_t *decoder, size_t max_queue_size) {
    if (!decoder || max_queue_size == 0) {
        LOG_ERROR("V4L2 demux bridge: Invalid parameters");
        return NULL;
    }
    
    v4l2_demux_bridge_t *bridge = calloc(1, sizeof(v4l2_demux_bridge_t));
    if (!bridge) {
        LOG_ERROR("V4L2 demux bridge: Failed to allocate memory");
        return NULL;
    }
    
    bridge->decoder = decoder;
    bridge->max_queue_size = max_queue_size;
    
    // Initialize mutex
    if (pthread_mutex_init(&bridge->queue_mutex, NULL) != 0) {
        LOG_ERROR("V4L2 demux bridge: Failed to initialize mutex");
        free(bridge);
        return NULL;
    }
    
    // Initialize statistics
    memset(&bridge->stats, 0, sizeof(bridge->stats));
    
    LOG_INFO("V4L2 demux bridge created with max queue size: %zu", max_queue_size);
    return bridge;
}

void v4l2_demux_bridge_destroy(v4l2_demux_bridge_t *bridge) {
    if (!bridge) return;
    
    // Clear all queued packets
    clear_queue(bridge);
    
    // Destroy mutex
    pthread_mutex_destroy(&bridge->queue_mutex);
    
    free(bridge);
    LOG_INFO("V4L2 demux bridge destroyed");
}

v4l2_stream_config_t* v4l2_demux_bridge_configure_stream(v4l2_demux_bridge_t *bridge, 
                                                          const v4l2_stream_info_t *stream_info) {
    if (!bridge || !stream_info) {
        return NULL;
    }
    
    // Map codec from stream info
    bridge->stream_config.codec = v4l2_demux_bridge_map_codec(stream_info->codec_id);
    bridge->stream_config.width = (uint32_t)stream_info->width;
    bridge->stream_config.height = (uint32_t)stream_info->height;
    bridge->stream_config.fps = stream_info->fps;
    bridge->stream_config.is_supported = (bridge->stream_config.codec != V4L2_CODEC_UNKNOWN);
    
    bridge->stream_configured = true;
    
    if (bridge->stream_config.is_supported) {
        LOG_INFO("V4L2 demux bridge: Stream configured - %dx%d %.2f fps, codec: %d", 
                 bridge->stream_config.width, bridge->stream_config.height,
                 bridge->stream_config.fps, bridge->stream_config.codec);
    } else {
        LOG_WARN("V4L2 demux bridge: Unsupported codec ID: %d", stream_info->codec_id);
    }
    
    return &bridge->stream_config;
}

v4l2_packet_callback_t v4l2_demux_bridge_get_callback(v4l2_demux_bridge_t *bridge) {
    if (!bridge) return NULL;
    return packet_callback_impl;
}

void* v4l2_demux_bridge_get_user_data(v4l2_demux_bridge_t *bridge) {
    return bridge;
}

int v4l2_demux_bridge_process_packets(v4l2_demux_bridge_t *bridge, int max_packets) {
    if (!bridge) return 0;
    
    int processed = 0;
    
    while ((max_packets <= 0 || processed < max_packets)) {
        packet_node_t *node = dequeue_packet(bridge);
        if (!node) break; // No more packets
        
        // Send packet to V4L2 decoder
        bool success = v4l2_decoder_decode(bridge->decoder, node->data, node->size, node->pts);
        
        if (success) {
            bridge->stats.packets_decoded++;
        } else {
            bridge->stats.decode_errors++;
            report_error(bridge, "V4L2 decode failed for packet (size: %zu, pts: %lld)", 
                        node->size, (long long)node->pts);
        }
        
        free_packet_node(node);
        processed++;
    }
    
    return processed;
}

bool v4l2_demux_bridge_has_packets(v4l2_demux_bridge_t *bridge) {
    if (!bridge) return false;
    
    pthread_mutex_lock(&bridge->queue_mutex);
    bool has_packets = (bridge->queue_head != NULL);
    pthread_mutex_unlock(&bridge->queue_mutex);
    
    return has_packets;
}

void v4l2_demux_bridge_flush(v4l2_demux_bridge_t *bridge) {
    if (!bridge) return;
    
    clear_queue(bridge);
    LOG_INFO("V4L2 demux bridge: Flushed all queued packets");
}

void v4l2_demux_bridge_get_stats(v4l2_demux_bridge_t *bridge, v4l2_bridge_stats_t *stats) {
    if (!bridge || !stats) return;
    
    pthread_mutex_lock(&bridge->queue_mutex);
    *stats = bridge->stats;
    stats->queue_size = bridge->queue_size;
    pthread_mutex_unlock(&bridge->queue_mutex);
}

void v4l2_demux_bridge_set_error_callback(v4l2_demux_bridge_t *bridge,
                                           void (*callback)(const char *error, void *user_data),
                                           void *user_data) {
    if (!bridge) return;
    
    bridge->error_callback = callback;
    bridge->error_user_data = user_data;
}

v4l2_codec_t v4l2_demux_bridge_map_codec(int codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return V4L2_CODEC_H264;
        case AV_CODEC_ID_HEVC:
            return V4L2_CODEC_HEVC;
        case AV_CODEC_ID_VP8:
            return V4L2_CODEC_VP8;
        case AV_CODEC_ID_VP9:
            return V4L2_CODEC_VP9;
        default:
            return V4L2_CODEC_UNKNOWN;
    }
}

bool v4l2_demux_bridge_is_codec_supported(int codec_id) {
    return v4l2_demux_bridge_map_codec(codec_id) != V4L2_CODEC_UNKNOWN;
}

// Internal implementations

static void packet_callback_impl(v4l2_demuxed_packet_t *packet, void *user_data) {
    v4l2_demux_bridge_t *bridge = (v4l2_demux_bridge_t*)user_data;
    
    if (!bridge || !packet || !packet->data || packet->size <= 0) {
        return;
    }
    
    // Update statistics
    bridge->stats.packets_received++;
    
    // Enqueue packet (this will copy the data)
    if (!enqueue_packet(bridge, packet->data, packet->size, packet->pts, packet->keyframe)) {
        bridge->stats.packets_dropped++;
    }
}

static void report_error(v4l2_demux_bridge_t *bridge, const char *format, ...) {
    if (!bridge) return;
    
    char error_msg[512];
    va_list args;
    va_start(args, format);
    vsnprintf(error_msg, sizeof(error_msg), format, args);
    va_end(args);
    
    LOG_ERROR("V4L2 demux bridge: %s", error_msg);
    
    if (bridge->error_callback) {
        bridge->error_callback(error_msg, bridge->error_user_data);
    }
}

static bool enqueue_packet(v4l2_demux_bridge_t *bridge, const uint8_t *data, 
                          size_t size, int64_t pts, bool is_keyframe) {
    if (!bridge || !data || size == 0) return false;
    
    pthread_mutex_lock(&bridge->queue_mutex);
    
    // Check queue size limit
    if (bridge->queue_size >= bridge->max_queue_size) {
        pthread_mutex_unlock(&bridge->queue_mutex);
        return false; // Queue full
    }
    
    // Allocate new packet node
    packet_node_t *node = malloc(sizeof(packet_node_t));
    if (!node) {
        pthread_mutex_unlock(&bridge->queue_mutex);
        return false;
    }
    
    // Copy packet data
    node->data = malloc(size);
    if (!node->data) {
        free(node);
        pthread_mutex_unlock(&bridge->queue_mutex);
        return false;
    }
    
    memcpy(node->data, data, size);
    node->size = size;
    node->pts = pts;
    node->is_keyframe = is_keyframe;
    node->next = NULL;
    
    // Add to queue
    if (bridge->queue_tail) {
        bridge->queue_tail->next = node;
    } else {
        bridge->queue_head = node;
    }
    bridge->queue_tail = node;
    bridge->queue_size++;
    
    // Update statistics
    if (bridge->queue_size > bridge->stats.max_queue_size) {
        bridge->stats.max_queue_size = bridge->queue_size;
    }
    
    pthread_mutex_unlock(&bridge->queue_mutex);
    return true;
}

static packet_node_t* dequeue_packet(v4l2_demux_bridge_t *bridge) {
    if (!bridge) return NULL;
    
    pthread_mutex_lock(&bridge->queue_mutex);
    
    packet_node_t *node = bridge->queue_head;
    if (node) {
        bridge->queue_head = node->next;
        if (!bridge->queue_head) {
            bridge->queue_tail = NULL;
        }
        bridge->queue_size--;
    }
    
    pthread_mutex_unlock(&bridge->queue_mutex);
    return node;
}

static void free_packet_node(packet_node_t *node) {
    if (!node) return;
    
    free(node->data);
    free(node);
}

static void clear_queue(v4l2_demux_bridge_t *bridge) {
    if (!bridge) return;
    
    pthread_mutex_lock(&bridge->queue_mutex);
    
    packet_node_t *current = bridge->queue_head;
    while (current) {
        packet_node_t *next = current->next;
        free_packet_node(current);
        current = next;
    }
    
    bridge->queue_head = NULL;
    bridge->queue_tail = NULL;
    bridge->queue_size = 0;
    
    pthread_mutex_unlock(&bridge->queue_mutex);
}