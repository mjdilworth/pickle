#include "v4l2_decoder.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#if defined(USE_V4L2_DECODER)
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/poll.h>
#include <linux/videodev2.h>
#include <linux/media.h>

// Helper macros
#define CLEAR(x) memset(&(x), 0, sizeof(x))

// Convert codec enum to V4L2 format
static uint32_t codec_to_v4l2_format(v4l2_codec_t codec) {
    switch (codec) {
        case V4L2_CODEC_H264:  return V4L2_PIX_FMT_H264;
        case V4L2_CODEC_HEVC:  return V4L2_PIX_FMT_HEVC;
        case V4L2_CODEC_VP8:   return V4L2_PIX_FMT_VP8;
        case V4L2_CODEC_VP9:   return V4L2_PIX_FMT_VP9;
        case V4L2_CODEC_MPEG2: return V4L2_PIX_FMT_MPEG2;
        case V4L2_CODEC_MPEG4: return V4L2_PIX_FMT_MPEG4;
        default:               return 0;
    }
}

// Check if a specific codec is supported by the hardware
bool v4l2_decoder_check_format(v4l2_codec_t codec) {
    int fd = -1;
    bool supported = false;
    
    // Try to open the first V4L2 M2M device
    fd = open("/dev/video0", O_RDWR);
    if (fd < 0) {
        LOG_ERROR("Failed to open video device: %s", strerror(errno));
        return false;
    }
    
    // Check if it's a V4L2 M2M device
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("Failed to query capabilities: %s", strerror(errno));
        close(fd);
        return false;
    }
    
    if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) && 
        !(cap.capabilities & V4L2_CAP_VIDEO_M2M)) {
        LOG_ERROR("Not a memory-to-memory video device");
        close(fd);
        return false;
    }
    
    // Check if the codec is supported
    struct v4l2_fmtdesc fmt;
    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    
    uint32_t v4l2_format = codec_to_v4l2_format(codec);
    if (v4l2_format == 0) {
        LOG_ERROR("Unknown codec");
        close(fd);
        return false;
    }
    
    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        if (fmt.pixelformat == v4l2_format) {
            supported = true;
            break;
        }
        fmt.index++;
    }
    
    close(fd);
    return supported;
}

// Check if any V4L2 M2M decoder is available
bool v4l2_decoder_is_supported(void) {
    // Try all common V4L2 M2M device paths
    const char *dev_paths[] = {
        "/dev/video0",
        "/dev/video1",
        "/dev/video10",
        "/dev/video11",
        NULL
    };
    
    for (int i = 0; dev_paths[i] != NULL; i++) {
        int fd = open(dev_paths[i], O_RDWR);
        if (fd < 0) {
            continue;
        }
        
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            if ((cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) || 
                (cap.capabilities & V4L2_CAP_VIDEO_M2M)) {
                LOG_INFO("Found M2M device: %s", dev_paths[i]);
                close(fd);
                return true;
            }
        }
        
        close(fd);
    }
    
    LOG_INFO("No V4L2 M2M devices found");
    return false;
}

// Initialize the V4L2 decoder
bool v4l2_decoder_init(v4l2_decoder_t *dec, v4l2_codec_t codec, uint32_t width, uint32_t height) {
    if (!dec) {
        LOG_ERROR("Invalid decoder context");
        return false;
    }
    
    // Clear the decoder context
    memset(dec, 0, sizeof(v4l2_decoder_t));
    
    // Find a suitable V4L2 M2M device
    const char *dev_paths[] = {
        "/dev/video0",
        "/dev/video1",
        "/dev/video10",
        "/dev/video11",
        NULL
    };
    
    bool found = false;
    for (int i = 0; dev_paths[i] != NULL; i++) {
        dec->fd = open(dev_paths[i], O_RDWR);
        if (dec->fd < 0) {
            continue;
        }
        
        struct v4l2_capability cap;
        if (ioctl(dec->fd, VIDIOC_QUERYCAP, &cap) == 0) {
            if ((cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) || 
                (cap.capabilities & V4L2_CAP_VIDEO_M2M)) {
                
                // Check if the device supports the codec
                struct v4l2_fmtdesc fmt;
                CLEAR(fmt);
                fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                
                uint32_t v4l2_format = codec_to_v4l2_format(codec);
                if (v4l2_format == 0) {
                    LOG_ERROR("Unknown codec");
                    close(dec->fd);
                    dec->fd = -1;
                    continue;
                }
                
                while (ioctl(dec->fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
                    if (fmt.pixelformat == v4l2_format) {
                        found = true;
                        break;
                    }
                    fmt.index++;
                }
                
                if (found) {
                    LOG_INFO("Found suitable M2M device: %s", dev_paths[i]);
                    break;
                }
            }
        }
        
        close(dec->fd);
        dec->fd = -1;
    }
    
    if (!found || dec->fd < 0) {
        LOG_ERROR("No suitable V4L2 M2M device found");
        return false;
    }
    
    // Set initial properties
    dec->codec = codec;
    dec->width = width;
    dec->height = height;
    
    // Set default buffer types
    if (ioctl(dec->fd, VIDIOC_QUERYCAP, &(struct v4l2_capability){}) == 0) {
        struct v4l2_capability cap;
        ioctl(dec->fd, VIDIOC_QUERYCAP, &cap);
        
        if (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) {
            dec->output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            dec->capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            LOG_INFO("Using multi-planar API");
        } else {
            dec->output_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            dec->capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            LOG_INFO("Using single-planar API");
        }
    } else {
        LOG_ERROR("Failed to query capabilities");
        close(dec->fd);
        dec->fd = -1;
        return false;
    }
    
    // Configure input and output formats
    if (!v4l2_decoder_set_format(dec, codec, width, height)) {
        LOG_ERROR("Failed to set input format");
        close(dec->fd);
        dec->fd = -1;
        return false;
    }
    
    // Set default output format to NV12 (widely supported)
    if (!v4l2_decoder_set_output_format(dec, V4L2_PIX_FMT_NV12)) {
        LOG_ERROR("Failed to set output format");
        close(dec->fd);
        dec->fd = -1;
        return false;
    }
    
    dec->initialized = true;
    
    return true;
}

// Clean up the V4L2 decoder
void v4l2_decoder_destroy(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized) {
        return;
    }
    
    // Stop streaming if active
    if (dec->streaming) {
        v4l2_decoder_stop(dec);
    }
    
    // Free buffers
    if (dec->output_buffers) {
        // Unmap memory-mapped buffers
        for (int i = 0; i < dec->num_output_buffers; i++) {
            if (dec->output_mmap && dec->output_mmap[i]) {
                munmap(dec->output_mmap[i], dec->output_buffers[i].length);
            }
        }
        free(dec->output_buffers);
        free(dec->output_mmap);
        dec->output_buffers = NULL;
        dec->output_mmap = NULL;
    }
    
    if (dec->capture_buffers) {
        // Close DMA-BUF file descriptors or unmap memory
        for (int i = 0; i < dec->num_capture_buffers; i++) {
            if (dec->dmabuf_fds && dec->dmabuf_fds[i] >= 0) {
                close(dec->dmabuf_fds[i]);
            } else if (dec->capture_mmap && dec->capture_mmap[i]) {
                munmap(dec->capture_mmap[i], dec->capture_buffers[i].length);
            }
        }
        free(dec->capture_buffers);
        free(dec->capture_mmap);
        free(dec->dmabuf_fds);
        dec->capture_buffers = NULL;
        dec->capture_mmap = NULL;
        dec->dmabuf_fds = NULL;
    }
    
    // Close device
    if (dec->fd >= 0) {
        close(dec->fd);
        dec->fd = -1;
    }
    
    dec->initialized = false;
}

// Set input format for the decoder
bool v4l2_decoder_set_format(v4l2_decoder_t *dec, v4l2_codec_t codec, uint32_t width, uint32_t height) {
    if (!dec || !dec->initialized) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }
    
    uint32_t v4l2_format = codec_to_v4l2_format(codec);
    if (v4l2_format == 0) {
        LOG_ERROR("Unknown codec");
        return false;
    }
    
    // Set input (encoded) format
    struct v4l2_format fmt;
    CLEAR(fmt);
    
    fmt.type = dec->output_type;
    if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        // Multi-planar API
        fmt.fmt.pix_mp.pixelformat = v4l2_format;
        fmt.fmt.pix_mp.width = width;
        fmt.fmt.pix_mp.height = height;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes = 1;
    } else {
        // Single-planar API
        fmt.fmt.pix.pixelformat = v4l2_format;
        fmt.fmt.pix.width = width;
        fmt.fmt.pix.height = height;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }
    
    if (ioctl(dec->fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Failed to set input format: %s", strerror(errno));
        return false;
    }
    
    dec->codec = codec;
    dec->width = width;
    dec->height = height;
    
    return true;
}

// Set output format for the decoder
bool v4l2_decoder_set_output_format(v4l2_decoder_t *dec, uint32_t pixel_format) {
    if (!dec || !dec->initialized) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }
    
    // Set output (decoded) format
    struct v4l2_format fmt;
    CLEAR(fmt);
    
    fmt.type = dec->capture_type;
    if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        // Multi-planar API
        fmt.fmt.pix_mp.pixelformat = pixel_format;
        fmt.fmt.pix_mp.width = dec->width;
        fmt.fmt.pix_mp.height = dec->height;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    } else {
        // Single-planar API
        fmt.fmt.pix.pixelformat = pixel_format;
        fmt.fmt.pix.width = dec->width;
        fmt.fmt.pix.height = dec->height;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
    }
    
    if (ioctl(dec->fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Failed to set output format: %s", strerror(errno));
        return false;
    }
    
    // Get the actual dimensions and format (might be adjusted by the driver)
    if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        dec->width = fmt.fmt.pix_mp.width;
        dec->height = fmt.fmt.pix_mp.height;
        dec->stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        dec->pixel_format = fmt.fmt.pix_mp.pixelformat;
    } else {
        dec->width = fmt.fmt.pix.width;
        dec->height = fmt.fmt.pix.height;
        dec->stride = fmt.fmt.pix.bytesperline;
        dec->pixel_format = fmt.fmt.pix.pixelformat;
    }
    
    LOG_INFO("V4L2", "Output format: %dx%d, stride=%d, format=%4.4s",
             dec->width, dec->height, dec->stride,
             (char*)&dec->pixel_format);
    
    return true;
}

// Set callback for decoded frames
bool v4l2_decoder_set_frame_callback(v4l2_decoder_t *dec, v4l2_decoded_frame_cb cb, void *user_data) {
    if (!dec || !dec->initialized) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }
    
    dec->frame_cb = cb;
    dec->user_data = user_data;
    
    return true;
}

// Allocate buffers for input and output
bool v4l2_decoder_allocate_buffers(v4l2_decoder_t *dec, int num_output, int num_capture) {
    if (!dec || !dec->initialized) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }
    
    // Request output (encoded) buffers
    struct v4l2_requestbuffers req;
    CLEAR(req);
    
    req.count = num_output;
    req.type = dec->output_type;
    req.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(dec->fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("Failed to request output buffers: %s", strerror(errno));
        return false;
    }
    
    if (req.count < 1) {
        LOG_ERROR("Insufficient output buffer memory");
        return false;
    }
    
    // Allocate buffer structures
    dec->num_output_buffers = req.count;
    dec->output_buffers = calloc(req.count, sizeof(struct v4l2_buffer));
    dec->output_mmap = calloc(req.count, sizeof(void*));
    
    if (!dec->output_buffers || !dec->output_mmap) {
        LOG_ERROR("Failed to allocate buffer memory");
        free(dec->output_buffers);
        free(dec->output_mmap);
        dec->output_buffers = NULL;
        dec->output_mmap = NULL;
        return false;
    }
    
    // Map output buffers
    for (int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        CLEAR(buf);
        CLEAR(planes);
        
        buf.index = i;
        buf.type = dec->output_type;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        
        if (ioctl(dec->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("Failed to query output buffer: %s", strerror(errno));
            return false;
        }
        
        // Store buffer information
        dec->output_buffers[i] = buf;
        
        // Map the buffer
        void *start;
        size_t length;
        
        if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            start = mmap(NULL, buf.m.planes[0].length,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         dec->fd, buf.m.planes[0].m.mem_offset);
            length = buf.m.planes[0].length;
        } else {
            start = mmap(NULL, buf.length,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         dec->fd, buf.m.offset);
            length = buf.length;
        }
        
        if (start == MAP_FAILED) {
            LOG_ERROR("Failed to mmap output buffer: %s", strerror(errno));
            return false;
        }
        
        dec->output_mmap[i] = start;
        
        LOG_INFO("Output buffer %d: length=%zu", i, length);
    }
    
    // Request capture (decoded) buffers
    CLEAR(req);
    
    req.count = num_capture;
    req.type = dec->capture_type;
    req.memory = V4L2_MEMORY_MMAP;  // Will be overridden if using DMABUF
    
    if (ioctl(dec->fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("Failed to request capture buffers: %s", strerror(errno));
        return false;
    }
    
    if (req.count < 1) {
        LOG_ERROR("Insufficient capture buffer memory");
        return false;
    }
    
    // Allocate buffer structures
    dec->num_capture_buffers = req.count;
    dec->capture_buffers = calloc(req.count, sizeof(struct v4l2_buffer));
    dec->capture_mmap = calloc(req.count, sizeof(void*));
    dec->dmabuf_fds = calloc(req.count, sizeof(int));
    
    if (!dec->capture_buffers || !dec->capture_mmap || !dec->dmabuf_fds) {
        LOG_ERROR("Failed to allocate buffer memory");
        free(dec->capture_buffers);
        free(dec->capture_mmap);
        free(dec->dmabuf_fds);
        dec->capture_buffers = NULL;
        dec->capture_mmap = NULL;
        dec->dmabuf_fds = NULL;
        return false;
    }
    
    // Initialize file descriptors to -1
    for (int i = 0; i < req.count; i++) {
        dec->dmabuf_fds[i] = -1;
    }
    
    // Map capture buffers
    for (int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        CLEAR(buf);
        CLEAR(planes);
        
        buf.index = i;
        buf.type = dec->capture_type;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        
        if (ioctl(dec->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("Failed to query capture buffer: %s", strerror(errno));
            return false;
        }
        
        // Store buffer information
        dec->capture_buffers[i] = buf;
        
        // Map the buffer
        void *start;
        size_t length;
        
        if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            start = mmap(NULL, buf.m.planes[0].length,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         dec->fd, buf.m.planes[0].m.mem_offset);
            length = buf.m.planes[0].length;
        } else {
            start = mmap(NULL, buf.length,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         dec->fd, buf.m.offset);
            length = buf.length;
        }
        
        if (start == MAP_FAILED) {
            LOG_ERROR("Failed to mmap capture buffer: %s", strerror(errno));
            return false;
        }
        
        dec->capture_mmap[i] = start;
        
        LOG_INFO("Capture buffer %d: length=%zu", i, length);
    }
    
    return true;
}

// Enable DMABUF export for zero-copy
bool v4l2_decoder_use_dmabuf(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }
    
    // TODO: Implement DMABUF export
    // This requires using V4L2_MEMORY_DMABUF and creating/exporting DMABUFs
    
    LOG_WARN("DMABUF export not yet implemented");
    return false;
}

// Start the decoder
bool v4l2_decoder_start(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }
    
    // Enqueue all capture buffers
    for (int i = 0; i < dec->num_capture_buffers; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        CLEAR(buf);
        CLEAR(planes);
        
        buf.index = i;
        buf.type = dec->capture_type;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buf.m.planes = planes;
            buf.length = 1;
            
            if (dec->capture_buffers[i].m.planes) {
                buf.m.planes[0].length = dec->capture_buffers[i].m.planes[0].length;
                buf.m.planes[0].m.mem_offset = dec->capture_buffers[i].m.planes[0].m.mem_offset;
            }
        } else {
            buf.length = dec->capture_buffers[i].length;
            buf.m.offset = dec->capture_buffers[i].m.offset;
        }
        
        if (ioctl(dec->fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("Failed to queue capture buffer: %s", strerror(errno));
            return false;
        }
    }
    
    // Start streaming
    int type = dec->capture_type;
    if (ioctl(dec->fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Failed to start capture streaming: %s", strerror(errno));
        return false;
    }
    
    type = dec->output_type;
    if (ioctl(dec->fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Failed to start output streaming: %s", strerror(errno));
        
        // Stop capture streaming
        type = dec->capture_type;
        ioctl(dec->fd, VIDIOC_STREAMOFF, &type);
        
        return false;
    }
    
    dec->streaming = true;
    dec->next_output_buffer = 0;
    dec->next_capture_buffer = 0;
    
    LOG_INFO("Decoder streaming started");
    
    return true;
}

// Stop the decoder
bool v4l2_decoder_stop(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized || !dec->streaming) {
        return false;
    }
    
    // Stop streaming
    int type = dec->output_type;
    if (ioctl(dec->fd, VIDIOC_STREAMOFF, &type) < 0) {
        LOG_ERROR("Failed to stop output streaming: %s", strerror(errno));
    }
    
    type = dec->capture_type;
    if (ioctl(dec->fd, VIDIOC_STREAMOFF, &type) < 0) {
        LOG_ERROR("Failed to stop capture streaming: %s", strerror(errno));
    }
    
    dec->streaming = false;
    
    LOG_INFO("Decoder streaming stopped");
    
    return true;
}

// Flush the decoder
bool v4l2_decoder_flush(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }
    
    // Send V4L2_DEC_CMD_STOP command
    struct v4l2_decoder_cmd cmd;
    CLEAR(cmd);
    
    cmd.cmd = V4L2_DEC_CMD_STOP;
    
    if (ioctl(dec->fd, VIDIOC_DECODER_CMD, &cmd) < 0) {
        LOG_ERROR("Failed to send stop command: %s", strerror(errno));
        return false;
    }
    
    // Wait for all buffers to be processed
    struct pollfd fds[1];
    fds[0].fd = dec->fd;
    fds[0].events = POLLIN;
    
    while (true) {
        int ret = poll(fds, 1, 100);  // 100ms timeout
        if (ret <= 0) {
            break;  // Timeout or error
        }
        
        v4l2_decoder_process_events(dec);
    }
    
    // Send V4L2_DEC_CMD_START command to restart
    CLEAR(cmd);
    cmd.cmd = V4L2_DEC_CMD_START;
    
    if (ioctl(dec->fd, VIDIOC_DECODER_CMD, &cmd) < 0) {
        LOG_ERROR("Failed to send start command: %s", strerror(errno));
        return false;
    }
    
    LOG_INFO("Decoder flushed");
    
    return true;
}

// Decode a frame
bool v4l2_decoder_decode(v4l2_decoder_t *dec, const void *data, size_t size, int64_t timestamp) {
    if (!dec || !dec->initialized || !dec->streaming) {
        LOG_ERROR("Decoder not initialized or not streaming");
        return false;
    }
    
    if (!data || size == 0) {
        LOG_ERROR("Invalid data or size");
        return false;
    }
    
    // Find an available output buffer
    int buf_index = -1;
    
    for (int i = 0; i < dec->num_output_buffers; i++) {
        int index = (dec->next_output_buffer + i) % dec->num_output_buffers;
        
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        CLEAR(buf);
        CLEAR(planes);
        
        buf.index = index;
        buf.type = dec->output_type;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        
        if (ioctl(dec->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("Failed to query output buffer: %s", strerror(errno));
            continue;
        }
        
        // Check if the buffer is available
        if (!(buf.flags & V4L2_BUF_FLAG_QUEUED) && !(buf.flags & V4L2_BUF_FLAG_DONE)) {
            buf_index = index;
            break;
        }
    }
    
    if (buf_index < 0) {
        // Try to dequeue a buffer
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        CLEAR(buf);
        CLEAR(planes);
        
        buf.type = dec->output_type;
        buf.memory = V4L2_MEMORY_MMAP;
        
        if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        
        if (ioctl(dec->fd, VIDIOC_DQBUF, &buf) < 0) {
            LOG_ERROR("No output buffers available");
            return false;
        }
        
        buf_index = buf.index;
    }
    
    // Copy data to the buffer
    size_t buffer_size;
    if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        buffer_size = dec->output_buffers[buf_index].m.planes[0].length;
    } else {
        buffer_size = dec->output_buffers[buf_index].length;
    }
    
    if (size > buffer_size) {
        LOG_ERROR("Data too large for buffer (%zu > %zu)", size, buffer_size);
        return false;
    }
    
    // Copy data to mapped memory
    memcpy(dec->output_mmap[buf_index], data, size);
    
    // Queue the buffer
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    CLEAR(buf);
    CLEAR(planes);
    
    buf.index = buf_index;
    buf.type = dec->output_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.timestamp.tv_sec = timestamp / 1000000;
    buf.timestamp.tv_usec = timestamp % 1000000;
    
    if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        buf.m.planes = planes;
        buf.length = 1;
        buf.m.planes[0].bytesused = size;
        buf.m.planes[0].length = buffer_size;
    } else {
        buf.bytesused = size;
        buf.length = buffer_size;
    }
    
    if (ioctl(dec->fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to queue output buffer: %s", strerror(errno));
        return false;
    }
    
    dec->next_output_buffer = (buf_index + 1) % dec->num_output_buffers;
    
    return true;
}

// Get a decoded frame
bool v4l2_decoder_get_frame(v4l2_decoder_t *dec, v4l2_decoded_frame_t *frame) {
    if (!dec || !dec->initialized || !dec->streaming || !frame) {
        LOG_ERROR("Invalid decoder or frame");
        return false;
    }
    
    // Try to dequeue a capture buffer
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    CLEAR(buf);
    CLEAR(planes);
    
    buf.type = dec->capture_type;
    buf.memory = V4L2_MEMORY_MMAP;
    
    if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = 1;
    }
    
    if (ioctl(dec->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) {
            // No buffer available yet
            return false;
        }
        
        LOG_ERROR("Failed to dequeue capture buffer: %s", strerror(errno));
        return false;
    }
    
    // Fill frame information
    frame->width = dec->width;
    frame->height = dec->height;
    frame->format = dec->pixel_format;
    frame->timestamp = buf.timestamp.tv_sec * 1000000LL + buf.timestamp.tv_usec;
    frame->keyframe = (buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
    
    if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        frame->bytesused = buf.m.planes[0].bytesused;
    } else {
        frame->bytesused = buf.bytesused;
    }
    
    // TODO: Handle DMA-BUF export
    frame->dmabuf_fd = -1;
    
    // Keep track of the buffer index for re-queueing
    int buf_index = buf.index;
    
    // If using a frame callback, call it
    if (dec->frame_cb) {
        dec->frame_cb(frame, dec->user_data);
    }
    
    // Re-queue the buffer
    if (ioctl(dec->fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to re-queue capture buffer: %s", strerror(errno));
        return false;
    }
    
    return true;
}

// Poll for events
bool v4l2_decoder_poll(v4l2_decoder_t *dec, int timeout_ms) {
    if (!dec || !dec->initialized || !dec->streaming) {
        LOG_ERROR("Decoder not initialized or not streaming");
        return false;
    }
    
    struct pollfd fds[1];
    fds[0].fd = dec->fd;
    fds[0].events = POLLIN | POLLOUT | POLLPRI;
    
    int ret = poll(fds, 1, timeout_ms);
    if (ret < 0) {
        LOG_ERROR("Poll failed: %s", strerror(errno));
        return false;
    }
    
    if (ret == 0) {
        // Timeout
        return false;
    }
    
    // Check if there are events to process
    if (fds[0].revents & (POLLIN | POLLOUT | POLLPRI)) {
        return v4l2_decoder_process_events(dec);
    }
    
    return false;
}

// Process decoder events
bool v4l2_decoder_process_events(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized || !dec->streaming) {
        LOG_ERROR("Decoder not initialized or not streaming");
        return false;
    }
    
    bool frame_available = false;
    
    // Process available frames
    while (true) {
        v4l2_decoded_frame_t frame;
        
        if (!v4l2_decoder_get_frame(dec, &frame)) {
            break;
        }
        
        frame_available = true;
    }
    
    return frame_available;
}
#else // !defined(USE_V4L2_DECODER)

// Stub implementations for platforms without V4L2 support
bool v4l2_decoder_check_format(v4l2_codec_t codec) {
    (void)codec; // unused
    return false;
}

bool v4l2_decoder_is_supported(void) {
    return false;
}

bool v4l2_decoder_init(v4l2_decoder_t *dec, v4l2_codec_t codec, uint32_t width, uint32_t height) {
    (void)dec; // unused
    (void)codec; // unused
    (void)width; // unused
    (void)height; // unused
    return false;
}

void v4l2_decoder_destroy(v4l2_decoder_t *dec) {
    (void)dec; // unused
}

bool v4l2_decoder_set_format(v4l2_decoder_t *dec, v4l2_codec_t codec, uint32_t width, uint32_t height) {
    (void)dec; // unused
    (void)codec; // unused
    (void)width; // unused
    (void)height; // unused
    return false;
}

bool v4l2_decoder_set_output_format(v4l2_decoder_t *dec, uint32_t pixel_format) {
    (void)dec; // unused
    (void)pixel_format; // unused
    return false;
}

bool v4l2_decoder_set_frame_callback(v4l2_decoder_t *dec, v4l2_decoded_frame_cb cb, void *user_data) {
    (void)dec; // unused
    (void)cb; // unused
    (void)user_data; // unused
    return false;
}

bool v4l2_decoder_allocate_buffers(v4l2_decoder_t *dec, int num_output, int num_capture) {
    (void)dec; // unused
    (void)num_output; // unused
    (void)num_capture; // unused
    return false;
}

bool v4l2_decoder_use_dmabuf(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }

#if defined(USE_V4L2_DECODER)
    struct v4l2_requestbuffers req;
    CLEAR(req);
    
    // Check if the device supports DMA-BUF export
    struct v4l2_capability caps;
    CLEAR(caps);
    
    if (ioctl(dec->fd, VIDIOC_QUERYCAP, &caps) < 0) {
        LOG_ERROR("Failed to query device capabilities: %s", strerror(errno));
        return false;
    }
    
    // Check if device supports DMA-BUF export
    if (!(caps.capabilities & V4L2_CAP_DEVICE_CAPS)) {
        LOG_ERROR("Device doesn't support device capabilities");
        return false;
    }
    
    // Set the DMA-BUF flag for our capture buffers (output frames)
    req.count = dec->num_capture_buffers > 0 ? dec->num_capture_buffers : 4;
    req.type = dec->capture_type;
    req.memory = V4L2_MEMORY_DMABUF;
    
    if (ioctl(dec->fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR("DMA-BUF export not supported by device: %s", strerror(errno));
        return false;
    }
    
    // Allocate DMA-BUF file descriptor array
    dec->dmabuf_fds = calloc(req.count, sizeof(int));
    if (!dec->dmabuf_fds) {
        LOG_ERROR("Failed to allocate DMA-BUF file descriptor array");
        return false;
    }
    
    // Initialize file descriptors to -1 (invalid)
    for (int i = 0; i < req.count; i++) {
        dec->dmabuf_fds[i] = -1;
    }
    
    dec->num_capture_buffers = req.count;
    LOG_INFO("DMA-BUF export enabled with %d buffers", req.count);
    return true;
#else
    LOG_ERROR("V4L2 decoder not compiled with DMA-BUF support");
    return false;
#endif
}

bool v4l2_decoder_start(v4l2_decoder_t *dec) {
    (void)dec; // unused
    return false;
}

bool v4l2_decoder_stop(v4l2_decoder_t *dec) {
    (void)dec; // unused
    return false;
}

bool v4l2_decoder_flush(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized || !dec->streaming) {
        return false;
    }

#if defined(USE_V4L2_DECODER)
    // Send an empty buffer with the EOS flag to signal end of stream
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    CLEAR(buf);
    CLEAR(planes);
    
    // Get an available output buffer
    int idx = dec->next_output_buffer;
    if (idx < 0 || idx >= dec->num_output_buffers) {
        // Find any available buffer
        for (idx = 0; idx < dec->num_output_buffers; idx++) {
            if (dec->output_buffers[idx].flags & V4L2_BUF_FLAG_QUEUED) {
                continue;
            }
            break;
        }
        
        if (idx >= dec->num_output_buffers) {
            // Wait for a buffer to become available
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(dec->fd, &fds);
            
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int ret = select(dec->fd + 1, &fds, NULL, NULL, &tv);
            if (ret <= 0) {
                LOG_ERROR("Timeout waiting for buffer to become available");
                return false;
            }
            
            // Try to dequeue a buffer
            struct v4l2_buffer dqbuf;
            CLEAR(dqbuf);
            dqbuf.type = dec->output_type;
            dqbuf.memory = V4L2_MEMORY_MMAP;
            
            if (ioctl(dec->fd, VIDIOC_DQBUF, &dqbuf) < 0) {
                LOG_ERROR("Failed to dequeue buffer: %s", strerror(errno));
                return false;
            }
            
            idx = dqbuf.index;
        }
    }
    
    buf.type = dec->output_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = idx;
    buf.flags = V4L2_BUF_FLAG_LAST; // Signal last buffer
    
    if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        buf.m.planes = planes;
        buf.length = 1;
        planes[0].bytesused = 0;
        planes[0].length = 0;
    } else {
        buf.bytesused = 0;
    }
    
    // Queue the empty buffer with EOS flag
    if (ioctl(dec->fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to queue EOS buffer: %s", strerror(errno));
        return false;
    }
    
    // Update next buffer index
    dec->next_output_buffer = (idx + 1) % dec->num_output_buffers;
    
    LOG_INFO("Sent EOS to decoder");
    return true;
#else
    return false;
#endif
}

bool v4l2_decoder_decode(v4l2_decoder_t *dec, const void *data, size_t size, int64_t timestamp) {
    if (!dec || !dec->initialized || !dec->streaming || !data || size == 0) {
        return false;
    }

#if defined(USE_V4L2_DECODER)
    // Get an available output buffer
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    CLEAR(buf);
    CLEAR(planes);
    
    buf.type = dec->output_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = dec->next_output_buffer;
    
    if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        buf.m.planes = planes;
        buf.length = 1;
    }
    
    // Copy data to the buffer
    void *buffer_data = NULL;
    size_t buffer_size = 0;
    
    if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        buffer_data = dec->output_mmap[buf.index * dec->num_output_planes + 0];
        buffer_size = dec->output_buffers[buf.index].m.planes[0].length;
    } else {
        buffer_data = dec->output_mmap[buf.index];
        buffer_size = dec->output_buffers[buf.index].length;
    }
    
    if (size > buffer_size) {
        LOG_ERROR("Data size %zu exceeds buffer size %zu", size, buffer_size);
        return false;
    }
    
    memcpy(buffer_data, data, size);
    
    // Set buffer properties
    if (dec->output_type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
        planes[0].bytesused = size;
        planes[0].length = buffer_size;
    } else {
        buf.bytesused = size;
    }
    
    // Set timestamp
    buf.timestamp.tv_sec = timestamp / 1000000;
    buf.timestamp.tv_usec = timestamp % 1000000;
    
    // Queue the buffer
    if (ioctl(dec->fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to queue output buffer: %s", strerror(errno));
        return false;
    }
    
    // Update next buffer index
    dec->next_output_buffer = (dec->next_output_buffer + 1) % dec->num_output_buffers;
    
    return true;
#else
    return false;
#endif
}

bool v4l2_decoder_get_frame(v4l2_decoder_t *dec, v4l2_decoded_frame_t *frame) {
    if (!dec || !frame) {
        return false;
    }

#if defined(USE_V4L2_DECODER)
    // Initialize frame
    memset(frame, 0, sizeof(v4l2_decoded_frame_t));
    frame->dmabuf_fd = -1;
    
    // Try to dequeue a capture buffer
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    CLEAR(buf);
    CLEAR(planes);
    
    buf.type = dec->capture_type;
    
    // Use DMA-BUF memory type if DMA-BUF export is enabled
    if (dec->dmabuf_fds) {
        buf.memory = V4L2_MEMORY_DMABUF;
    } else {
        buf.memory = V4L2_MEMORY_MMAP;
    }
    
    if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buf.m.planes = planes;
        buf.length = 1;
    }
    
    // Dequeue a buffer
    if (ioctl(dec->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) {
            // No buffer available, not an error
            return false;
        }
        LOG_ERROR("Failed to dequeue capture buffer: %s", strerror(errno));
        return false;
    }
    
    // Get frame information
    frame->width = dec->width;
    frame->height = dec->height;
    frame->format = dec->pixel_format;
    frame->timestamp = buf.timestamp.tv_sec * 1000000LL + buf.timestamp.tv_usec;
    frame->keyframe = (buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
    
    if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        frame->bytesused = buf.m.planes[0].bytesused;
    } else {
        frame->bytesused = buf.bytesused;
    }
    
    // Handle DMA-BUF export if enabled
    if (dec->dmabuf_fds) {
        // For DMA-BUF, we need to export the buffer to a DMA-BUF file descriptor
        struct v4l2_exportbuffer expbuf;
        CLEAR(expbuf);
        
        expbuf.type = dec->capture_type;
        expbuf.index = buf.index;
        expbuf.flags = O_RDONLY;
        
        if (ioctl(dec->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
            LOG_ERROR("Failed to export buffer to DMA-BUF: %s", strerror(errno));
            
            // Requeue the buffer
            if (ioctl(dec->fd, VIDIOC_QBUF, &buf) < 0) {
                LOG_ERROR("Failed to requeue buffer: %s", strerror(errno));
            }
            
            return false;
        }
        
        // Store the DMA-BUF file descriptor
        frame->dmabuf_fd = expbuf.fd;
        dec->dmabuf_fds[buf.index] = expbuf.fd;
        
        LOG_INFO("Exported buffer %d to DMA-BUF fd %d", buf.index, frame->dmabuf_fd);
    } else {
        // For memory-mapped buffers, get a pointer to the buffer data
        void *data = NULL;
        if (dec->capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            data = dec->capture_mmap[buf.index * dec->num_capture_planes + 0];
        } else {
            data = dec->capture_mmap[buf.index];
        }
        
        // Set the data pointer in the frame
        frame->data = data;
    }
    
    // Remember the buffer index for returning it later
    frame->buf_index = buf.index;
    
    return true;
#else
    return false;
#endif
}

bool v4l2_decoder_poll(v4l2_decoder_t *dec, int timeout_ms) {
    if (!dec || !dec->initialized || !dec->streaming) {
        return false;
    }

#if defined(USE_V4L2_DECODER)
    struct pollfd fds[1];
    fds[0].fd = dec->fd;
    fds[0].events = POLLIN | POLLOUT | POLLPRI;
    fds[0].revents = 0;
    
    // Poll for events with the specified timeout
    int ret = poll(fds, 1, timeout_ms);
    if (ret < 0) {
        if (errno != EINTR) {
            LOG_ERROR("Poll failed: %s", strerror(errno));
        }
        return false;
    }
    
    if (ret == 0) {
        // Timeout, no events
        return false;
    }
    
    // Check for events
    if (fds[0].revents & (POLLIN | POLLPRI)) {
        // Data is available for reading (decoded frame)
        return true;
    }
    
    if (fds[0].revents & POLLOUT) {
        // Device is ready for writing (can accept more input)
        return true;
    }
    
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        LOG_ERROR("Poll error on V4L2 device");
        return false;
    }
    
    return false;
#else
    return false;
#endif
}

bool v4l2_decoder_process_events(v4l2_decoder_t *dec) {
    if (!dec || !dec->initialized || !dec->streaming) {
        return false;
    }

#if defined(USE_V4L2_DECODER)
    // Process any events that might have occurred
    struct v4l2_event event;
    while (ioctl(dec->fd, VIDIOC_DQEVENT, &event) == 0) {
        switch (event.type) {
            case V4L2_EVENT_SOURCE_CHANGE:
                LOG_INFO("V4L2 source change event detected");
                // Handle resolution change
                // We'd need to stop streaming, query the new format, and restart
                break;
                
            case V4L2_EVENT_EOS:
                LOG_INFO("V4L2 end of stream event");
                // Handle end of stream
                break;
                
            default:
                LOG_INFO("Unknown V4L2 event: %d", event.type);
                break;
        }
    }
    
    // Check if there was an error other than EAGAIN (no more events)
    if (errno != EAGAIN) {
        LOG_ERROR("Error dequeueing V4L2 event: %s", strerror(errno));
        return false;
    }
    
    return true;
#else
    return false;
#endif
}

#endif // USE_V4L2_DECODER

