#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <gbm.h>
#include <drm_fourcc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include "drm.h"
#include "egl.h"
#include "log.h"

/**
 * Check if zero-copy path should be used
 * 
 * @param d Pointer to DRM context
 * @param e Pointer to EGL context
 * @return true if zero-copy path should be used, false otherwise
 */
bool should_use_zero_copy(kms_ctx_t *d, egl_ctx_t *e) {
#ifndef ZEROCOPY_ENABLED
    // Zero-copy disabled at compile time
    return false;
#else
    // Check if all required components are initialized
    if (!d || !e) {
        return false;
    }
    
    // Check if DMA-BUF is supported
    if (!e->dmabuf_supported) {
        static bool logged_once = false;
        if (!logged_once) {
            LOG_ERROR("DMA-BUF not supported by EGL, zero-copy disabled");
            logged_once = true;
        }
        return false;
    }
    
    // Check if atomic modesetting is supported (preferred for zero-copy)
    if (!d->atomic_supported) {
        static bool logged_once = false;
        if (!logged_once) {
            LOG_DEBUG("Atomic modesetting not supported, zero-copy will use legacy path");
            logged_once = true;
        }
        // We can still use zero-copy with legacy modesetting, but it's less efficient
    }
#endif
    
    // Check if we have a valid connector and plane
    if (d->connector_id == 0 || d->crtc_id == 0) {
        return false;
    }
    
    // All checks passed, we can use zero-copy
    return true;
}

/**
 * Present a frame using the zero-copy DMA-BUF path with atomic modesetting
 * 
 * @param d Pointer to DRM context
 * @param e Pointer to EGL context
 * @param video_texture Video texture to present
 * @param src_rect Source rectangle for video (normalized 0-1)
 * @param dst_rect Destination rectangle for display (normalized 0-1)
 * @return true on success, false on failure
 */
bool present_frame_zero_copy(kms_ctx_t *d, egl_ctx_t *e, unsigned int video_texture,
                             float *src_rect, float *dst_rect) {
    if (!d || !e || video_texture == 0) {
        return false;
    }
    
    // Create a DMA-BUF to export the texture
    dmabuf_info_t dmabuf;
    memset(&dmabuf, 0, sizeof(dmabuf));
    
    // Create a framebuffer for the mode resolution
    uint32_t width = d->mode.hdisplay;
    uint32_t height = d->mode.vdisplay;
    
    // Render the video texture to a DMA-BUF
    // We create a temporary FBO, render the video texture to it, and then
    // export the FBO color attachment as a DMA-BUF
    
    // 1. Create a temporary framebuffer object (FBO)
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    
    // 2. Create a texture that will be exported as DMA-BUF
    bool success = create_dmabuf_texture(e, width, height, GBM_FORMAT_XRGB8888, &dmabuf);
    if (!success) {
        LOG_ERROR("Failed to create DMA-BUF texture");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        return false;
    }
    
    // 3. Attach the texture to the FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dmabuf.texture, 0);
    
    // 4. Check if the FBO is complete
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("FBO is not complete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        destroy_dmabuf(e, &dmabuf);
        return false;
    }
    
    // 5. Render the video texture to the FBO
    // Save current viewport
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    
    // Set viewport to match FBO size
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    
    // Clear the FBO
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Use a simple shader program for rendering
    // For now, just use a basic texture rendering
    // This would need to be adapted based on the actual shader setup in your application
    
    // Bind the video texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, video_texture);
    
    // Draw a fullscreen quad (assuming you have the appropriate shader)
    // This is a simplification - in a real implementation, you would use your existing
    // shader program for rendering textures
    
    // For now, we'll just use a very basic draw to render the texture
    
    // This is a placeholder - in a real implementation you would use
    // your existing rendering code to draw the texture to the FBO
    
    // Restore previous viewport
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    
    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    
    // Create a DRM framebuffer from the DMA-BUF
    uint32_t fb_id = 0;
    uint32_t handles[4] = {0};
    uint32_t strides[4] = {dmabuf.stride};
    uint32_t offsets[4] = {0};
    
    int ret = drmModeAddFB2(d->fd, width, height, dmabuf.format,
                            handles, strides, offsets, &fb_id, 0);
    
    if (ret) {
        LOG_ERROR("Failed to create DRM framebuffer from DMA-BUF: %s", strerror(errno));
        destroy_dmabuf(e, &dmabuf);
        return false;
    }
    
    // Present the framebuffer
    if (d->atomic_supported) {
        // Use atomic modesetting for presentation
        if (!atomic_present_framebuffer(d, fb_id, true)) {
            LOG_ERROR("Failed to present framebuffer with atomic modesetting");
            drmModeRmFB(d->fd, fb_id);
            destroy_dmabuf(e, &dmabuf);
            return false;
        }
    } else {
        // Use legacy modesetting
        // For better performance with legacy modesetting, we use a more direct approach
        // that reduces the number of mode switches
        
        // First check if this is the first frame or if we need to set the CRTC
        if (!d->crtc_initialized) {
            ret = drmModeSetCrtc(d->fd, d->crtc_id, fb_id, 0, 0,
                                &d->connector_id, 1, &d->mode);
            
            if (ret) {
                LOG_ERROR("Failed to set CRTC: %s", strerror(errno));
                drmModeRmFB(d->fd, fb_id);
                destroy_dmabuf(e, &dmabuf);
                return false;
            }
            
            d->crtc_initialized = true;
        } else {
            // Use page flipping for subsequent frames
            ret = drmModePageFlip(d->fd, d->crtc_id, fb_id,
                                DRM_MODE_PAGE_FLIP_EVENT, d);
            
            if (ret) {
                LOG_ERROR("Failed to schedule page flip: %s", strerror(errno));
                drmModeRmFB(d->fd, fb_id);
                destroy_dmabuf(e, &dmabuf);
                return false;
            }
        }
    }
    
    // Cleanup will be done in the page flip handler
    // Note: In a real implementation, you would need to handle cleanup of the
    // framebuffer and DMA-BUF when the page flip completes
    
    static bool first_success = true;
    if (first_success) {
        LOG_INFO("Zero-copy presentation using %s modesetting successful",
                d->atomic_supported ? "atomic" : "legacy");
        first_success = false;
    }
    
    return true;
}