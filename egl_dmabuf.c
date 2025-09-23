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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "drm.h"
#include "egl.h"
#include "log.h"

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