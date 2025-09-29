#ifndef PICKLE_V4L2_PLAYER_H
#define PICKLE_V4L2_PLAYER_H

#include <stdio.h>
#include <stdint.h>
#include <GLES3/gl31.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "v4l2_decoder.h"

#ifdef USE_V4L2_DECODER
#include "v4l2_demuxer.h"
#endif

/**
 * @file v4l2_player.h
 * @brief V4L2 decoder player integration for Pickle
 */

// V4L2 player context
typedef struct {
    v4l2_decoder_t *decoder;     // V4L2 decoder instance
    v4l2_codec_t codec;          // Codec being used
    uint32_t width;              // Video width
    uint32_t height;             // Video height
    int is_active;               // Flag indicating decoder is active
    FILE *input_file;            // Input file handle
    uint8_t *buffer;             // Buffer for reading file data
    size_t buffer_size;          // Size of the buffer
    int64_t timestamp;           // Current timestamp
    GLuint texture;              // OpenGL texture for rendering
    
#ifdef USE_V4L2_DECODER
    // V4L2 demuxer integration (for V4L2 mode)
    v4l2_demuxer_t *demuxer;     // V4L2 demuxer instance
    bool use_demuxer;            // Flag indicating demuxer is being used
#endif
    
    // Current frame information
    struct {
        bool valid;              // Is the current frame valid
        int dmabuf_fd;           // DMA-BUF file descriptor for current frame
        uint32_t width;          // Frame width
        uint32_t height;         // Frame height
        uint32_t stride;         // Frame stride/pitch in bytes
        uint32_t format;         // Frame format
        GLuint texture;          // OpenGL texture for current frame
        int buf_index;           // Buffer index for returning to decoder
        EGLImageKHR egl_image;   // EGL image handle for DMA-BUF texture cleanup
        bool is_dmabuf_texture;  // True if texture was created from DMA-BUF
    } current_frame;
} v4l2_player_t;

#endif /* PICKLE_V4L2_PLAYER_H */