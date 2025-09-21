#ifndef PICKLE_EGL_H
#define PICKLE_EGL_H

#include <stdbool.h>
#include <stdint.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include "drm.h"

// DMA-BUF support
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif

#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#endif

// Define extension function pointer types if not defined
#ifndef PFNEGLCREATEIMAGEKHRPROC
typedef EGLImageKHR (EGLAPIENTRYP PFNEGLCREATEIMAGEKHRPROC) (EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
#endif

#ifndef PFNEGLDESTROYIMAGEKHRPROC
typedef EGLBoolean (EGLAPIENTRYP PFNEGLDESTROYIMAGEKHRPROC) (EGLDisplay dpy, EGLImageKHR image);
#endif

#ifndef PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) (GLenum target, GLeglImageOES image);
#endif

// EGL context structure
typedef struct egl_ctx {
    struct gbm_device *gbm_dev;
    struct gbm_surface *gbm_surf;
    EGLDisplay dpy;
    EGLConfig config;
    EGLContext ctx;
    EGLSurface surf;
    
    // DMA-BUF support
    bool dmabuf_supported;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
} egl_ctx_t;

// DMA-BUF structure
typedef struct dmabuf_info {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint64_t modifier;
    EGLImageKHR image;
    GLuint texture;
} dmabuf_info_t;

// GBM/EGL initialization and cleanup
bool init_gbm_egl(const kms_ctx_t *d, egl_ctx_t *e);
void deinit_gbm_egl(egl_ctx_t *e);

// Buffer object handler
void bo_destroy_handler(struct gbm_bo *bo, void *data);

// Framebuffer and presentation management
uint32_t get_framebuffer_for_bo(int fd, struct gbm_bo *bo);
struct gbm_bo* get_next_bo(egl_ctx_t *e);
void swap_buffers(egl_ctx_t *e);
void wait_for_flip(int fd);

// DMA-BUF support
bool init_dmabuf_extension(egl_ctx_t *e);
bool create_dmabuf_from_bo(egl_ctx_t *e, struct gbm_bo *bo, dmabuf_info_t *dmabuf);
void destroy_dmabuf(egl_ctx_t *e, dmabuf_info_t *dmabuf);
bool is_dmabuf_supported(void);

// Get the current vsync state
bool is_vsync_enabled(void);

// Toggle vsync state
void toggle_vsync(void);

// EGL vsync control
bool is_vsync_enabled(void);
void set_vsync_enabled(bool enabled);

// Video rendering function
bool render_video_frame(egl_ctx_t *e, GLuint video_texture, const float src_rect[4], const float dst_rect[4]);

// DMA-BUF zero-copy path functions
bool is_dmabuf_supported(void);
bool init_dmabuf_extension(egl_ctx_t *e);
bool create_dmabuf_from_bo(egl_ctx_t *e, struct gbm_bo *bo, dmabuf_info_t *dmabuf);
void destroy_dmabuf(egl_ctx_t *e, dmabuf_info_t *dmabuf);
bool create_dmabuf_texture(egl_ctx_t *e, uint32_t width, uint32_t height, uint32_t format, dmabuf_info_t *dmabuf);
bool render_to_dmabuf(egl_ctx_t *e, dmabuf_info_t *dmabuf, GLuint video_texture, const float src_rect[4], const float dst_rect[4]);

#endif // PICKLE_EGL_H