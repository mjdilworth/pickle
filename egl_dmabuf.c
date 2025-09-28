#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include <gbm.h>
#include <drm_fourcc.h>
#include <time.h>
#include <sys/time.h>
#include <linux/videodev2.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

#include "drm.h"
#include "egl.h"
#include "log.h"

// EGL DMA-BUF extension function pointers
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

/**
 * Initialize EGL DMA-BUF extension function pointers
 * 
 * @param e Pointer to EGL context
 * @return true on success, false on failure
 */
static bool init_dmabuf_extensions(egl_ctx_t *e) {
    if (!e || eglCreateImageKHR) {
        return true; // Already initialized
    }
    
    // Check for required extensions
    const char *egl_extensions = eglQueryString(e->dpy, EGL_EXTENSIONS);
    if (!egl_extensions || !strstr(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
        LOG_ERROR("EGL_EXT_image_dma_buf_import extension not supported");
        return false;
    }
    
    const char *gl_extensions = (const char*)glGetString(GL_EXTENSIONS);
    if (!gl_extensions || !strstr(gl_extensions, "GL_OES_EGL_image")) {
        LOG_ERROR("GL_OES_EGL_image extension not supported");
        return false;
    }
    
    // Get extension function pointers
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (!eglCreateImageKHR || !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES) {
        LOG_ERROR("Failed to get EGL DMA-BUF extension function pointers");
        return false;
    }
    
    LOG_INFO("EGL DMA-BUF extensions initialized successfully");
    return true;
}

/**
 * Create an OpenGL texture from a V4L2 DMA-BUF file descriptor
 * 
 * @param e Pointer to EGL context
 * @param dmabuf_fd DMA-BUF file descriptor from V4L2
 * @param width Width of the frame
 * @param height Height of the frame
 * @param stride Stride/pitch of the frame in bytes
 * @param format V4L2 pixel format
 * @param out_texture Pointer to store the created OpenGL texture ID
 * @param out_image Pointer to store the EGL image (for cleanup)
 * @return true on success, false on failure
 */
bool create_texture_from_v4l2_dmabuf(egl_ctx_t *e, int dmabuf_fd, uint32_t width, 
                                      uint32_t height, uint32_t stride, uint32_t format,
                                      GLuint *out_texture, EGLImageKHR *out_image) {
    if (!e || dmabuf_fd < 0 || !out_texture || !out_image) {
        return false;
    }
    
    // Initialize DMA-BUF extensions if not already done
    if (!init_dmabuf_extensions(e)) {
        return false;
    }
    
    // Convert V4L2 format to DRM fourcc format
    uint32_t drm_format;
    switch (format) {
        case V4L2_PIX_FMT_NV12:
            drm_format = DRM_FORMAT_NV12;
            break;
        case V4L2_PIX_FMT_YUV420:
            drm_format = DRM_FORMAT_YUV420;
            break;
        case V4L2_PIX_FMT_YUYV:
            drm_format = DRM_FORMAT_YUYV;
            break;
        case V4L2_PIX_FMT_RGB565:
            drm_format = DRM_FORMAT_RGB565;
            break;
        case V4L2_PIX_FMT_XRGB32:
        case V4L2_PIX_FMT_RGB32:
            drm_format = DRM_FORMAT_XRGB8888;
            break;
        default:
            LOG_ERROR("Unsupported V4L2 format for DMA-BUF import: 0x%x", format);
            return false;
    }
    
    // EGL image attributes for DMA-BUF import
    EGLint attribs[] = {
        EGL_WIDTH,                     (EGLint)width,
        EGL_HEIGHT,                    (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT,     (EGLint)drm_format,
        EGL_DMA_BUF_PLANE0_FD_EXT,    dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE
    };
    
    // Create EGL image from DMA-BUF
    EGLImageKHR image = eglCreateImageKHR(e->dpy, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        LOG_ERROR("Failed to create EGL image from V4L2 DMA-BUF fd %d", dmabuf_fd);
        return false;
    }
    
    // Generate OpenGL texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    // Create texture from EGL image
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Check for OpenGL errors
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        LOG_ERROR("OpenGL error creating texture from V4L2 DMA-BUF: 0x%x", gl_error);
        glDeleteTextures(1, &texture);
        eglDestroyImageKHR(e->dpy, image);
        return false;
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    *out_texture = texture;
    *out_image = image;
    
    LOG_DEBUG("Created OpenGL texture %u from V4L2 DMA-BUF fd %d (%ux%u, stride=%u)", 
              texture, dmabuf_fd, width, height, stride);
    
    return true;
}

/**
 * Destroy a texture and EGL image created from V4L2 DMA-BUF
 * 
 * @param e Pointer to EGL context
 * @param texture OpenGL texture ID
 * @param image EGL image handle
 */
void destroy_v4l2_dmabuf_texture(egl_ctx_t *e, GLuint texture, EGLImageKHR image) {
    if (texture != 0) {
        glDeleteTextures(1, &texture);
    }
    
    if (image != EGL_NO_IMAGE_KHR && e && eglDestroyImageKHR) {
        eglDestroyImageKHR(e->dpy, image);
    }
}

/**
 * Create a GBM buffer object with DMA-BUF export capability
 * 
 * @param e Pointer to EGL context
 * @param width Width of the buffer
 * @param height Height of the buffer
 * @param format Format of the buffer (e.g., GBM_FORMAT_XRGB8888)
 * @return GBM buffer object or NULL on failure
 */
struct gbm_bo* create_exportable_bo(egl_ctx_t *e, uint32_t width, uint32_t height, uint32_t format) {
    if (!e || !e->gbm_dev) {
        return NULL;
    }
    
    // Create a GBM buffer object with scanout and linear layout for easy export
    struct gbm_bo *bo = gbm_bo_create(e->gbm_dev, width, height, format,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    
    if (!bo) {
        LOG_ERROR("Failed to create exportable GBM BO: %s", strerror(errno));
        return NULL;
    }
    
    return bo;
}

/**
 * Create a texture for zero-copy rendering using DMA-BUF
 * 
 * @param e Pointer to EGL context
 * @param width Width of the texture
 * @param height Height of the texture
 * @param format Format of the texture (e.g., GBM_FORMAT_XRGB8888)
 * @param dmabuf Pointer to DMA-BUF info structure to initialize
 * @return true on success, false on failure
 */
bool create_dmabuf_texture(egl_ctx_t *e, uint32_t width, uint32_t height, 
                           uint32_t format, dmabuf_info_t *dmabuf) {
    if (!e || !dmabuf || !e->dmabuf_supported) {
        return false;
    }
    
    // Initialize the DMA-BUF info structure
    memset(dmabuf, 0, sizeof(dmabuf_info_t));
    dmabuf->fd = -1;
    dmabuf->image = EGL_NO_IMAGE_KHR;
    
    // Create a GBM buffer object
    struct gbm_bo *bo = create_exportable_bo(e, width, height, format);
    if (!bo) {
        return false;
    }
    
    // Create DMA-BUF from the GBM buffer object
    bool success = create_dmabuf_from_bo(e, bo, dmabuf);
    
    // We can destroy the GBM BO after we've exported it to DMA-BUF
    gbm_bo_destroy(bo);
    
    if (!success) {
        LOG_ERROR("Failed to create DMA-BUF from GBM BO");
        return false;
    }
    
    return true;
}

/**
 * Render video frame to a DMA-BUF texture for zero-copy display
 * 
 * @param e Pointer to EGL context
 * @param dmabuf Pointer to DMA-BUF info structure
 * @param video_texture Video texture to render
 * @param src_rect Source rectangle for the video (normalized 0.0-1.0)
 * @param dst_rect Destination rectangle for the rendered frame (normalized 0.0-1.0)
 * @return true on success, false on failure
 */
bool render_to_dmabuf(egl_ctx_t *e, dmabuf_info_t *dmabuf, GLuint video_texture,
                      const float src_rect[4], const float dst_rect[4]) {
    if (!e || !dmabuf || !e->dmabuf_supported || dmabuf->texture == 0) {
        return false;
    }
    
    // Create and bind a framebuffer object for rendering to the DMA-BUF texture
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    
    // Attach the DMA-BUF texture to the framebuffer
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dmabuf->texture, 0);
    
    // Check framebuffer status
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Framebuffer not complete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return false;
    }
    
    // Set viewport to match the DMA-BUF texture size
    glViewport(0, 0, (GLsizei)dmabuf->width, (GLsizei)dmabuf->height);
    
    // Clear the framebuffer
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Render the video texture to the framebuffer
    render_video_frame(e, video_texture, src_rect, dst_rect);
    
    // Unbind the framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    
    return true;
}

/**
 * Create a DRM framebuffer from a DMA-BUF for display
 * 
 * @param d Pointer to DRM context
 * @param dmabuf Pointer to DMA-BUF info structure
 * @param out_fb Pointer to store the created framebuffer ID
 * @return true on success, false on failure
 */
bool create_fb_from_dmabuf(kms_ctx_t *d, dmabuf_info_t *dmabuf, uint32_t *out_fb) {
    if (!d || !dmabuf || !out_fb || dmabuf->fd < 0) {
        return false;
    }
    
    // Create a DRM framebuffer from the DMA-BUF
    uint32_t fb_id;
    uint32_t handles[4] = {0};
    uint32_t strides[4] = {dmabuf->stride};
    uint32_t offsets[4] = {0};
    
    // Add the DMA-BUF as a prime buffer to the DRM device
    int ret = drmPrimeFDToHandle(d->fd, dmabuf->fd, &handles[0]);
    if (ret) {
        LOG_ERROR("Failed to import DMA-BUF into DRM: %s", strerror(errno));
        return false;
    }
    
    // Create a framebuffer using the imported handle
    ret = drmModeAddFB2(d->fd, dmabuf->width, dmabuf->height, dmabuf->format,
                         handles, strides, offsets, &fb_id, 0);
    
    if (ret) {
        LOG_ERROR("Failed to create DRM framebuffer from DMA-BUF: %s", strerror(errno));
        return false;
    }
    
    *out_fb = fb_id;
    return true;
}