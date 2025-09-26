#include "mp4_demuxer.h"
#include <stdlib.h>
#include <string.h>

/**
 * @file mp4_demuxer.c
 * @brief Minimal stub implementation of MP4 demuxer
 * 
 * This is a placeholder implementation. In a full implementation,
 * this would use libavformat or similar to parse MP4 files.
 */

bool mp4_demuxer_init(mp4_demuxer_t *demuxer, const char *filename) {
    if (!demuxer || !filename) return false;
    
    // Initialize structure
    memset(demuxer, 0, sizeof(*demuxer));
    
    // For now, just fail gracefully - this would need libavformat integration
    return false;
}

bool mp4_demuxer_get_stream_info(mp4_demuxer_t *demuxer, uint32_t *width, uint32_t *height, 
                                  double *fps, const char **codec_name) {
    (void)demuxer;
    (void)width;
    (void)height;
    (void)fps;
    (void)codec_name;
    return false;
}

bool mp4_demuxer_is_codec_supported(mp4_demuxer_t *demuxer) {
    (void)demuxer;
    return false;
}

bool mp4_demuxer_get_packet(mp4_demuxer_t *demuxer, mp4_packet_t *packet) {
    (void)demuxer;
    (void)packet;
    return false;
}

void mp4_demuxer_free_packet(mp4_packet_t *packet) {
    if (!packet) return;
    
    if (packet->data) {
        free(packet->data);
        packet->data = NULL;
    }
    packet->size = 0;
}

void mp4_demuxer_cleanup(mp4_demuxer_t *demuxer) {
    if (!demuxer) return;
    
    if (demuxer->file) {
        fclose(demuxer->file);
        demuxer->file = NULL;
    }
    
    if (demuxer->priv) {
        free(demuxer->priv);
        demuxer->priv = NULL;
    }
    
    memset(demuxer, 0, sizeof(*demuxer));
}