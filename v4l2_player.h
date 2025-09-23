#ifndef PICKLE_V4L2_PLAYER_H
#define PICKLE_V4L2_PLAYER_H

#include <stdio.h>
#include <stdint.h>
#include <GLES2/gl2.h>
#include "v4l2_decoder.h"

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
    
    // Current frame information
    struct {
        bool valid;              // Is the current frame valid
        int dmabuf_fd;           // DMA-BUF file descriptor for current frame
        uint32_t width;          // Frame width
        uint32_t height;         // Frame height
        uint32_t format;         // Frame format
        GLuint texture;          // OpenGL texture for current frame
        int buf_index;           // Buffer index for returning to decoder
    } current_frame;
} v4l2_player_t;

#endif /* PICKLE_V4L2_PLAYER_H */