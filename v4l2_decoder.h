#ifndef PICKLE_V4L2_DECODER_H
#define PICKLE_V4L2_DECODER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Only include videodev2.h when V4L2 is enabled
#if defined(USE_V4L2_DECODER)
#include <linux/videodev2.h>
#endif

// Forward declaration
typedef struct v4l2_decoder v4l2_decoder_t;

// Supported codecs
typedef enum {
    V4L2_CODEC_H264,
    V4L2_CODEC_HEVC,
    V4L2_CODEC_VP8,
    V4L2_CODEC_VP9,
    V4L2_CODEC_MPEG2,
    V4L2_CODEC_MPEG4,
    V4L2_CODEC_UNKNOWN
} v4l2_codec_t;

// Decoded frame information
typedef struct {
    int dmabuf_fd;          // DMA-BUF file descriptor for zero-copy
    uint32_t width;         // Frame width in pixels
    uint32_t height;        // Frame height in pixels
    uint32_t format;        // Pixel format (V4L2_PIX_FMT_*)
    uint32_t bytesused;     // Number of bytes used in the buffer
    uint32_t flags;         // Buffer flags
    int64_t timestamp;      // Timestamp in microseconds
    bool keyframe;          // Whether this is a keyframe
    void *data;             // Pointer to memory-mapped frame data (NULL if using DMA-BUF)
    int buf_index;          // Buffer index for returning to decoder
} v4l2_decoded_frame_t;

// Callback for decoded frames
typedef void (*v4l2_decoded_frame_cb)(v4l2_decoded_frame_t *frame, void *user_data);

// V4L2 M2M decoder context
struct v4l2_decoder {
    // Device handles
    int fd;                 // Device file descriptor
    int output_type;        // Output buffer type
    int capture_type;       // Capture buffer type
    
    // Buffer information
    struct v4l2_buffer *output_buffers;
    struct v4l2_buffer *capture_buffers;
    void **output_mmap;     // Memory-mapped input buffers
    void **capture_mmap;    // Memory-mapped output buffers (if not using DMABUF)
    int *dmabuf_fds;        // DMA-BUF file descriptors
    int num_output_buffers;
    int num_capture_buffers;
    
    // Stream information
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    v4l2_codec_t codec;
    uint32_t pixel_format;  // Output pixel format
    
    // State tracking
    bool initialized;
    bool streaming;
    int next_output_buffer;
    int next_capture_buffer;
    
    // Callback for decoded frames
    v4l2_decoded_frame_cb frame_cb;
    void *user_data;
};

// Initialization and cleanup
bool v4l2_decoder_init(v4l2_decoder_t *dec, v4l2_codec_t codec, uint32_t width, uint32_t height);
void v4l2_decoder_destroy(v4l2_decoder_t *dec);

// Codec support detection
bool v4l2_decoder_check_format(v4l2_codec_t codec);
bool v4l2_decoder_is_supported(void);

// Configure decoder
bool v4l2_decoder_set_format(v4l2_decoder_t *dec, v4l2_codec_t codec, uint32_t width, uint32_t height);
bool v4l2_decoder_set_output_format(v4l2_decoder_t *dec, uint32_t pixel_format);
bool v4l2_decoder_set_frame_callback(v4l2_decoder_t *dec, v4l2_decoded_frame_cb cb, void *user_data);

// Buffer allocation
bool v4l2_decoder_allocate_buffers(v4l2_decoder_t *dec, int num_output, int num_capture);
bool v4l2_decoder_use_dmabuf(v4l2_decoder_t *dec);

// Streaming control
bool v4l2_decoder_start(v4l2_decoder_t *dec);
bool v4l2_decoder_stop(v4l2_decoder_t *dec);
bool v4l2_decoder_flush(v4l2_decoder_t *dec);

// Decode operations
bool v4l2_decoder_decode(v4l2_decoder_t *dec, const void *data, size_t size, int64_t timestamp);
bool v4l2_decoder_get_frame(v4l2_decoder_t *dec, v4l2_decoded_frame_t *frame);
bool v4l2_decoder_poll(v4l2_decoder_t *dec, int timeout_ms);

// Event handling
bool v4l2_decoder_process_events(v4l2_decoder_t *dec);

#endif // PICKLE_V4L2_DECODER_H