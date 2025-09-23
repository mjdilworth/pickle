#include "egl.h"
#include "utils.h"
#include "keystone.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h> // For close()
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// Define logging macros if not already defined
#ifndef LOG_EGL
#define LOG_EGL(fmt, ...) fprintf(stderr, "[EGL] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef LOG_GL
#define LOG_GL(fmt, ...) fprintf(stderr, "[GL] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef LOG_ERROR
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef RETURN_ERROR
#define RETURN_ERROR(msg) do { LOG_ERROR("%s", msg); return false; } while(0)
#endif

#ifndef RETURN_ERROR_ERRNO
#define RETURN_ERROR_ERRNO(msg) do { LOG_ERROR("%s: %s", msg, strerror(errno)); return false; } while(0)
#endif

#ifndef RETURN_ERROR_EGL
#define RETURN_ERROR_EGL(msg) do { LOG_ERROR("%s: EGL error 0x%x", msg, eglGetError()); return false; } while(0)
#endif

// Global state
static int g_vsync_enabled = 1;   // Default to vsync on for smoother playback
static int g_dmabuf_supported = 0; // Flag to indicate if DMA-BUF is supported

/**
 * Check if DMA-BUF is supported
 * 
 * @return true if DMA-BUF is supported, false otherwise
 */
bool is_dmabuf_supported(void) {
    return g_dmabuf_supported != 0;
}

/**
 * Initialize DMA-BUF extension
 * 
 * @param e Pointer to EGL context
 * @return true on success, false on failure
 */
bool init_dmabuf_extension(egl_ctx_t *e) {
    const char *extensions = eglQueryString(e->dpy, EGL_EXTENSIONS);
    if (!extensions) {
        LOG_ERROR("Failed to query EGL extensions");
        return false;
    }
    
    if (!strstr(extensions, "EGL_EXT_image_dma_buf_import")) {
        LOG_INFO("DMA-BUF import not supported by EGL");
        return false;
    }
    
    // Get function pointers for DMA-BUF operations
    e->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    e->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    e->glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (!e->eglCreateImageKHR || !e->eglDestroyImageKHR || !e->glEGLImageTargetTexture2DOES) {
        LOG_ERROR("Failed to get required EGL/GL extension functions for DMA-BUF");
        return false;
    }
    
    e->dmabuf_supported = true;
    g_dmabuf_supported = 1;
    LOG_INFO("DMA-BUF support initialized successfully");
    return true;
}

/**
 * Create a DMA-BUF from a GBM buffer object
 * 
 * @param e Pointer to EGL context
 * @param bo GBM buffer object
 * @param dmabuf Pointer to DMA-BUF info structure to initialize
 * @return true on success, false on failure
 */
bool create_dmabuf_from_bo(egl_ctx_t *e, struct gbm_bo *bo, dmabuf_info_t *dmabuf) {
    if (!e || !bo || !dmabuf || !e->dmabuf_supported) {
        return false;
    }
    
    // Get DMA-BUF file descriptor
    int fd = gbm_bo_get_fd(bo);
    if (fd < 0) {
        LOG_ERROR("Failed to get DMA-BUF fd: %s", strerror(errno));
        return false;
    }
    
    // Get buffer properties
    dmabuf->fd = fd;
    dmabuf->width = gbm_bo_get_width(bo);
    dmabuf->height = gbm_bo_get_height(bo);
    dmabuf->format = gbm_bo_get_format(bo);
    dmabuf->stride = gbm_bo_get_stride(bo);
    dmabuf->modifier = 0; // Linear format
    
    // Create EGL image from DMA-BUF
    EGLint attribs[] = {
        EGL_WIDTH, (EGLint)dmabuf->width,
        EGL_HEIGHT, (EGLint)dmabuf->height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)dmabuf->format,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)dmabuf->stride,
        EGL_NONE
    };
    
    dmabuf->image = e->eglCreateImageKHR(e->dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (dmabuf->image == EGL_NO_IMAGE_KHR) {
        LOG_ERROR("Failed to create EGL image from DMA-BUF: 0x%x", eglGetError());
        close(dmabuf->fd);
        return false;
    }
    
    // Create a GL texture from the EGL image
    glGenTextures(1, &dmabuf->texture);
    glBindTexture(GL_TEXTURE_2D, dmabuf->texture);
    e->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, dmabuf->image);
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    LOG_INFO("Created DMA-BUF texture from GBM BO: %dx%d, format 0x%x", dmabuf->width, dmabuf->height, dmabuf->format);
    return true;
}

/**
 * Destroy a DMA-BUF and associated resources
 * 
 * @param e Pointer to EGL context
 * @param dmabuf Pointer to DMA-BUF info structure
 */
void destroy_dmabuf(egl_ctx_t *e, dmabuf_info_t *dmabuf) {
    if (!e || !dmabuf || !e->dmabuf_supported) {
        return;
    }
    
    if (dmabuf->texture) {
        glDeleteTextures(1, &dmabuf->texture);
        dmabuf->texture = 0;
    }
    
    if (dmabuf->image != EGL_NO_IMAGE_KHR) {
        e->eglDestroyImageKHR(e->dpy, dmabuf->image);
        dmabuf->image = EGL_NO_IMAGE_KHR;
    }
    
    if (dmabuf->fd >= 0) {
        close(dmabuf->fd);
        dmabuf->fd = -1;
    }
}

/**
 * Initialize GBM and EGL for OpenGL ES rendering
 * 
 * @param d Pointer to initialized DRM context
 * @param e Pointer to EGL context structure to initialize
 * @return true on success, false on failure
 */
bool init_gbm_egl(const kms_ctx_t *d, egl_ctx_t *e) {
    memset(e, 0, sizeof(*e));
    
    // Create GBM device from DRM file descriptor
    e->gbm_dev = gbm_create_device(d->fd);
    if (!e->gbm_dev) {
        RETURN_ERROR_ERRNO("gbm_create_device failed");
    }
    
    // Create GBM surface matching display resolution
    e->gbm_surf = gbm_surface_create(e->gbm_dev, d->mode.hdisplay, d->mode.vdisplay,
                         GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!e->gbm_surf) {
        RETURN_ERROR_ERRNO("gbm_surface_create failed");
    }

    // Initialize EGL from GBM device
    e->dpy = eglGetDisplay((EGLNativeDisplayType)e->gbm_dev);
    if (e->dpy == EGL_NO_DISPLAY) {
        RETURN_ERROR("eglGetDisplay failed");
    }
    
    if (!eglInitialize(e->dpy, NULL, NULL)) {
        RETURN_ERROR_EGL("eglInitialize failed");
    }
    
    eglBindAPI(EGL_OPENGL_ES_API);

    // We must pick an EGLConfig compatible with the GBM surface format (XRGB8888)
    EGLint cfg_attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,  // Request OpenGL ES 3.x
        EGL_CONFORMANT, EGL_OPENGL_ES3_BIT,       // Ensure conformant implementation
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 0,  // No depth buffer needed for 2D video
        EGL_STENCIL_SIZE, 0, // No stencil buffer needed
        EGL_NONE
    };
    
    // First query how many configs match our criteria
    EGLint num = 0;
    if (!eglChooseConfig(e->dpy, cfg_attrs, NULL, 0, &num) || num == 0) {
        RETURN_ERROR_EGL("eglChooseConfig(query) failed");
    }
    
    // Allocate space for matching configs
    size_t cfg_count = (size_t)num;
    EGLConfig *cfgs = cfg_count ? calloc(cfg_count, sizeof(EGLConfig)) : NULL;
    if (!cfgs) {
        RETURN_ERROR("Out of memory allocating config list");
    }
    
    // Get the actual matching configs
    if (!eglChooseConfig(e->dpy, cfg_attrs, cfgs, num, &num)) {
        free(cfgs);
        RETURN_ERROR_EGL("eglChooseConfig(list) failed");
    }
    
    // Choose the best config - prefer one with 0 alpha for XRGB format
    EGLConfig chosen = NULL;
    for (int i = 0; i < num; ++i) {
        EGLint r, g, b, a;
        eglGetConfigAttrib(e->dpy, cfgs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(e->dpy, cfgs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(e->dpy, cfgs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(e->dpy, cfgs[i], EGL_ALPHA_SIZE, &a);
        
        if (r == 8 && g == 8 && b == 8) { // alpha may be 0 or 8; either OK with XRGB
            chosen = cfgs[i];
            if (a == 0) break; // perfect match for XRGB
        }
    }
    
    if (!chosen) chosen = cfgs[0]; // fallback to first config if no ideal match
    e->config = chosen;
    free(cfgs);
    
    // Create EGL context
    // Request OpenGL ES 3.1 for compute shader support
    EGLint ctx_attr[] = { 
        EGL_CONTEXT_CLIENT_VERSION, 3,  // Request OpenGL ES 3.x
        EGL_CONTEXT_MINOR_VERSION_KHR, 1, // Request OpenGL ES 3.1 specifically
        EGL_NONE 
    };
    e->ctx = eglCreateContext(e->dpy, e->config, EGL_NO_CONTEXT, ctx_attr);
    if (e->ctx == EGL_NO_CONTEXT) {
        LOG_WARN("Failed to create OpenGL ES 3.1 context, falling back to OpenGL ES 2.0");
        
        // Fallback to OpenGL ES 2.0 if 3.1 is not supported
        EGLint fallback_ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        e->ctx = eglCreateContext(e->dpy, e->config, EGL_NO_CONTEXT, fallback_ctx_attr);
        if (e->ctx == EGL_NO_CONTEXT) {
            RETURN_ERROR_EGL("eglCreateContext failed even with fallback");
        }
    } else {
        LOG_INFO("Successfully created OpenGL ES 3.1 context with compute shader support");
    }
    
    // Create window surface
    EGLint win_attrs[] = { EGL_NONE };
    e->surf = eglCreateWindowSurface(e->dpy, e->config, (EGLNativeWindowType)e->gbm_surf, win_attrs);
    
    if (e->surf == EGL_NO_SURFACE) {
        LOG_EGL("eglCreateWindowSurface failed -> trying with alpha config fallback");
        
        // Retry with alpha-enabled config if original lacked alpha
        EGLint retry_attrs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,  // Try to maintain ES3 support
            EGL_CONFORMANT, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE
        };
        
        EGLint n2 = 0;
        if (eglChooseConfig(e->dpy, retry_attrs, &e->config, 1, &n2) && n2 == 1) {
            e->surf = eglCreateWindowSurface(e->dpy, e->config, (EGLNativeWindowType)e->gbm_surf, win_attrs);
        }
        
        if (e->surf == EGL_NO_SURFACE) {
            RETURN_ERROR_EGL("eglCreateWindowSurface still failed after retry");
        }
    }
    
    // Make our context current
    if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
        RETURN_ERROR_EGL("eglMakeCurrent failed");
    }
    
    // Set swap interval to control vsync behavior
    eglSwapInterval(e->dpy, g_vsync_enabled ? 1 : 0);

    // Log GL info
    const char *gl_vendor = (const char*)glGetString(GL_VENDOR);
    const char *gl_renderer = (const char*)glGetString(GL_RENDERER);
    const char *gl_version = (const char*)glGetString(GL_VERSION);
    LOG_GL("VENDOR='%s' RENDERER='%s' VERSION='%s'", 
        gl_vendor ? gl_vendor : "?", 
        gl_renderer ? gl_renderer : "?", 
        gl_version ? gl_version : "?");
    
    // Initialize DMA-BUF support if available
    e->dmabuf_supported = false;
    if (init_dmabuf_extension(e)) {
        LOG_INFO("DMA-BUF zero-copy path initialized successfully");
    } else {
        LOG_ERROR("DMA-BUF support not available, zero-copy disabled");
    }
        
    return true;
}

/**
 * Clean up GBM and EGL resources
 * 
 * @param e Pointer to EGL context structure to clean up
 */
void deinit_gbm_egl(egl_ctx_t *e) {
    if (!e) return;
    
    // Clean up keystone shader resources if initialized
    if (g_keystone_shader_program) {
        cleanup_keystone_shader();
    }
    // Ensure any cached FBO/texture are cleaned even if shader program wasn't created
    if (g_keystone_fbo) { glDeleteFramebuffers(1, &g_keystone_fbo); g_keystone_fbo = 0; }
    if (g_keystone_fbo_texture) { glDeleteTextures(1, &g_keystone_fbo_texture); g_keystone_fbo_texture = 0; }
    g_keystone_fbo_w = g_keystone_fbo_h = 0;
    
    if (e->dpy != EGL_NO_DISPLAY) {
        // Release current context
        eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        
        // Destroy context and surface if they exist
        if (e->ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(e->dpy, e->ctx);
            e->ctx = EGL_NO_CONTEXT;
        }
        
        if (e->surf != EGL_NO_SURFACE) {
            eglDestroySurface(e->dpy, e->surf);
            e->surf = EGL_NO_SURFACE;
        }
        
        // Terminate EGL display
        eglTerminate(e->dpy);
        e->dpy = EGL_NO_DISPLAY;
    }
    
    // Clear DMA-BUF support flags
    e->dmabuf_supported = false;
    g_dmabuf_supported = 0;
    e->eglCreateImageKHR = NULL;
    e->eglDestroyImageKHR = NULL;
    e->glEGLImageTargetTexture2DOES = NULL;
    
    // Destroy GBM resources
    if (e->gbm_surf) {
        gbm_surface_destroy(e->gbm_surf);
        e->gbm_surf = NULL;
    }
    
    if (e->gbm_dev) {
        gbm_device_destroy(e->gbm_dev);
        e->gbm_dev = NULL;
    }
}

/**
 * Check if vsync is currently enabled
 * 
 * @return true if vsync is enabled, false otherwise
 */
bool is_vsync_enabled(void) {
    return g_vsync_enabled != 0;
}

/**
 * Toggle vsync state
 */
void toggle_vsync(void) {
    g_vsync_enabled = !g_vsync_enabled;
    LOG_INFO("VSync %s", g_vsync_enabled ? "enabled" : "disabled");
}

/**
 * Get the next buffer object from the GBM surface
 * 
 * @param e Pointer to EGL context
 * @return The next buffer object
 */
struct gbm_bo* get_next_bo(egl_ctx_t *e) {
    eglSwapBuffers(e->dpy, e->surf);
    return gbm_surface_lock_front_buffer(e->gbm_surf);
}

/**
 * Swap the EGL buffers
 * 
 * @param e Pointer to EGL context
 */
void swap_buffers(egl_ctx_t *e) {
    eglSwapBuffers(e->dpy, e->surf);
}

/**
 * Get (or create) a framebuffer for a buffer object
 * 
 * @param fd DRM file descriptor
 * @param bo Buffer object
 * @return Framebuffer ID
 */
uint32_t get_framebuffer_for_bo(int fd, struct gbm_bo *bo) {
    struct fb_holder {
        uint32_t fb;
        int fd;
    };

    // Check if we already have a framebuffer for this BO
    struct fb_holder *fbh = gbm_bo_get_user_data(bo);
    if (fbh) {
        return fbh->fb;
    }

    // Create a new framebuffer
    uint32_t fb_id = 0;
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t format = gbm_bo_get_format(bo);
    uint32_t handles[4] = {0}, strides[4] = {0}, offsets[4] = {0};
    
    handles[0] = gbm_bo_get_handle(bo).u32;
    strides[0] = gbm_bo_get_stride(bo);
    
    // Try to use addFB2 first for full format support
    int ret = drmModeAddFB2(fd, width, height, format,
                           handles, strides, offsets, &fb_id, 0);
                           
    if (ret) {
        // Fall back to older addFB on failure
        uint8_t depth = 24;
        uint8_t bpp = 32; // XRGB8888
        
        ret = drmModeAddFB(fd, width, height, depth, bpp,
                          strides[0], handles[0], &fb_id);
        if (ret) {
            LOG_ERROR("Failed to create framebuffer: %s", strerror(errno));
            return 0;
        }
    }
    
    // Store the framebuffer ID with the BO
    struct fb_holder *nh = malloc(sizeof(*nh));
    if (nh) {
        nh->fb = fb_id;
        nh->fd = fd;
        gbm_bo_set_user_data(bo, nh, bo_destroy_handler);
    }
    
    return fb_id;
}

/**
 * Wait for a DRM page flip to complete
 * 
 * @param fd DRM file descriptor
 */
void wait_for_flip(int fd) {
    drmEventContext ev = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .page_flip_handler = NULL // We don't need a callback, just wait for completion
    };
    
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    
    // Wait for the flip to complete
    int ret = select(fd + 1, &fds, NULL, NULL, NULL);
    if (ret > 0 && FD_ISSET(fd, &fds)) {
        drmHandleEvent(fd, &ev);
    }
}