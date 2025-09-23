#ifndef PICKLE_DISPMANX_H
#define PICKLE_DISPMANX_H

#include <stdbool.h>
#include <stdint.h>

// Forward declarations
typedef struct dispmanx_ctx dispmanx_ctx_t;
typedef struct egl_ctx egl_ctx_t;

// Include bcm_host.h only when DISPMANX_ENABLED is defined
#if defined(DISPMANX_ENABLED)
#include <bcm_host.h>
// Define these constants as they're not in the standard headers
#define ELEMENT_CHANGE_LAYER         (1<<0)
#define ELEMENT_CHANGE_OPACITY       (1<<1)
#define ELEMENT_CHANGE_DEST_RECT     (1<<2)
#define ELEMENT_CHANGE_SRC_RECT      (1<<3)
#define ELEMENT_CHANGE_MASK_RESOURCE (1<<4)
#define ELEMENT_CHANGE_TRANSFORM     (1<<5)
#else
// Provide stub typedefs for non-RPi platforms
typedef uint32_t DISPMANX_DISPLAY_HANDLE_T;
typedef uint32_t DISPMANX_ELEMENT_HANDLE_T;
typedef uint32_t DISPMANX_UPDATE_HANDLE_T;
typedef uint32_t DISPMANX_RESOURCE_HANDLE_T;
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} VC_RECT_T;
typedef uint32_t VC_IMAGE_TYPE_T;
typedef uint32_t VC_IMAGE_TRANSFORM_T;
typedef uint32_t DISPMANX_TRANSFORM_T;
#define DISPMANX_NO_HANDLE 0
#define ELEMENT_CHANGE_DEST_RECT 1
#define ELEMENT_CHANGE_TRANSFORM 4
#endif

// EGL_DISPMANX_WINDOW_T definition needed for both cases
typedef struct {
    int32_t width;
    int32_t height;
    uint32_t handle;
} EGL_DISPMANX_WINDOW_T;

#include "log.h"

/**
 * DispmanX context structure for RPi-specific hardware acceleration
 */
struct dispmanx_ctx {
    // Display information
    DISPMANX_DISPLAY_HANDLE_T display;
    DISPMANX_ELEMENT_HANDLE_T element;
    DISPMANX_UPDATE_HANDLE_T update;
    DISPMANX_RESOURCE_HANDLE_T resource;
    
    // Display dimensions and configuration
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t frame_width;  // Current frame width
    uint32_t frame_height; // Current frame height
    VC_RECT_T src_rect;
    VC_RECT_T dst_rect;
    
    // Keystone parameters for hardware transform
    float keystone_coords[8]; // x1,y1,x2,y2,x3,y3,x4,y4
    VC_IMAGE_TRANSFORM_T transform;
    
    // EGL interop
    EGL_DISPMANX_WINDOW_T egl_window;
    
    // State flags
    bool initialized;
    bool keystone_enabled;
    bool direct_mode;
};

// Initialization and cleanup
bool init_dispmanx(dispmanx_ctx_t *ctx);
void deinit_dispmanx(dispmanx_ctx_t *ctx);

// Direct path rendering (bypassing EGL)
bool dispmanx_direct_display(dispmanx_ctx_t *ctx, void *buffer, uint32_t width, uint32_t height, uint32_t pitch);

// EGL interop for OpenGL-based rendering
bool dispmanx_create_egl_window(dispmanx_ctx_t *ctx, egl_ctx_t *egl);

// Keystone correction using hardware transform
bool dispmanx_set_keystone(dispmanx_ctx_t *ctx, float *coords);
bool dispmanx_apply_transform(dispmanx_ctx_t *ctx);

// Zero-copy display from DMA-BUF
bool dispmanx_display_dmabuf(dispmanx_ctx_t *ctx, int dmabuf_fd, uint32_t width, uint32_t height, uint32_t format);

// Check if DispmanX is available on this system
bool is_dispmanx_supported(void);

#endif // PICKLE_DISPMANX_H