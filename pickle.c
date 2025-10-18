// Minimal libmpv + DRM/KMS + GBM + EGL video player for Raspberry Pi 4 (VC6 / v3d)
// Hardware accelerated via libmpv's OpenGL rendering API (uses EGL context we supply).
//
// This code is a reference-quality example and omits some production error handling
// (hotplug, dynamic mode changes, HDR metadata, etc.). It focuses on a clear path:
//   1. Open DRM device (card1 preferred on RPi4 for vc4, fallback card0).
//   2. Pick the first connected connector & preferred mode.
//   3. Create GBM device & surface (double-buffered) matching the mode.
//   4. Create EGL display/context bound to the GBM device.
//   5. Initialize mpv, request OpenGL render context (opengl-cb API) or mpv_render_context.
//   6. In loop: handle mpv events, render a frame into current EGL surface, page-flip via DRM.
//   7. Clean shutdown.
//
// Build (example on RPi4 with Mesa vc4):
//   gcc -O2 -Wall -std=c11 pickle.c -o pickle $(pkg-config --cflags --libs mpv gbm egl glesv2 libdrm) -lpthread -lm
// If pkg-config files missing, fallback to manual libs: -lmpv -ldrm -lgbm -lEGL -lGLESv2 -lpthread -lm
//
// Run:
//   sudo ./pickle /path/to/video.mp4
//
// Assumptions / Notes:
// - Requires that the vc4 KMS driver is active (dtoverlay=vc4-kms-v3d in config.txt).
// - Executes fullscreen on the first connected display.
// - Uses DRM master (needs root or CAP_SYS_ADMIN typically unless logind grants).
// - Simplified: No vsync timing logic beyond drmModePageFlip blocking event.
// - Error handling is compressed into CHECK()/RET() helpers for brevity.
//
// If mpv was built without OpenGL/EGL support this will fail.
//

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "drm_keystone.h" // Include the DRM keystone header
#include <sys/epoll.h>
#include <sys/poll.h>

// Include our refactored modules
#include "pickle_globals.h"

// External function declarations
extern bool render_frame_mpv(mpv_handle *mpv, mpv_render_context *mpv_gl, kms_ctx_t *drm, egl_ctx_t *eglc);

// Include our refactored modules
#include "utils.h"
#include "shader.h"
#include "keystone.h"
#include "input.h"

#include "compute_keystone.h"
#include "drm.h"
#include "egl.h"
#include "stats_overlay.h"
#include "gl_optimize.h"
#include "decoder_pacing.h"

// V4L2 demuxer support
#if defined(USE_V4L2_DECODER) && defined(ENABLE_V4L2_DEMUXER)
#include "v4l2_demuxer.h"
#endif

// For event-driven architecture
#ifdef EVENT_DRIVEN_ENABLED
#include "v4l2_player.h"
#include "mpv.h"  
#include "pickle_events.h"
#endif

// FFmpeg V4L2 player support
#ifdef USE_FFMPEG_V4L2_PLAYER
#include "ffmpeg_v4l2_player.h"
#endif

#include <execinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <sys/stat.h>
#include <poll.h>
#include <termios.h>
#include <linux/joystick.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <getopt.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <sys/ioctl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <dlfcn.h>
#include <math.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#ifdef VULKAN_ENABLED
#include "vulkan.h"
#include "error.h"
#endif

// Some minimal systems or older headers might miss certain DRM macros; guard them.
#ifndef DRM_MODE_TYPE_PREFERRED
#define DRM_MODE_TYPE_PREFERRED  (1<<3)
#endif
#ifndef DRM_MODE_PAGE_FLIP_EVENT
#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#endif

// OpenGL compatibility defines for matrix operations (not in GLES3 headers)
#ifndef GL_PROJECTION
#define GL_PROJECTION 0x1701
#endif
#ifndef GL_MODELVIEW
#define GL_MODELVIEW 0x1700
#endif

// Additional logging macros not included in utils.h
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_DRM(fmt, ...)   fprintf(stderr, "[DRM] " fmt "\n", ##__VA_ARGS__)
#define LOG_MPV(fmt, ...)   fprintf(stderr, "[MPV] " fmt "\n", ##__VA_ARGS__)
#define LOG_EGL(fmt, ...)   fprintf(stderr, "[EGL] " fmt "\n", ##__VA_ARGS__)
#define LOG_GL(fmt, ...)    fprintf(stderr, "[GL] " fmt "\n", ##__VA_ARGS__)

// Timing and performance logging - only prints when g_frame_timing_enabled is enabled
#define LOG_TIMING(fmt, ...) do { if (g_frame_timing_enabled) fprintf(stderr, "[TIMING] " fmt "\n", ##__VA_ARGS__); } while(0)

// Stats logging - only prints when g_stats_enabled is enabled
#define LOG_STATS(fmt, ...) do { if (g_stats_enabled) fprintf(stderr, "[STATS] " fmt "\n", ##__VA_ARGS__); } while(0)

// Map mpv end-file reasons to readable strings (see mpv/client.h enum mpv_end_file_reason)
static const char *mpv_end_reason_str(int r) {
	switch (r) {
		case MPV_END_FILE_REASON_EOF: return "eof";
		case MPV_END_FILE_REASON_STOP: return "stop";
		case MPV_END_FILE_REASON_QUIT: return "quit";
		case MPV_END_FILE_REASON_ERROR: return "error";
		case MPV_END_FILE_REASON_REDIRECT: return "redirect";
		default: return "?";
	}
}

// Simple macro error handling
#define CHECK(x, msg) do { if (!(x)) { LOG_ERROR("%s failed (%s) at %s:%d", msg, strerror(errno), __FILE__, __LINE__); goto fail; } } while (0)
#define RET(msg) do { LOG_ERROR("%s at %s:%d", msg, __FILE__, __LINE__); goto fail; } while (0)

// Error handling with return values instead of goto
#define RETURN_ERROR(msg) do { LOG_ERROR("%s", msg); return false; } while (0)
#define RETURN_ERROR_ERRNO(msg) do { LOG_ERROR("%s: %s", msg, strerror(errno)); return false; } while (0)
#define RETURN_ERROR_EGL(msg) do { LOG_ERROR("%s (eglError=0x%04x)", msg, eglGetError()); return false; } while (0)
#define RETURN_ERROR_IF(cond, msg) do { if (cond) { RETURN_ERROR(msg); } } while (0)

// Memory monitoring functions
#ifdef USE_V4L2_DECODER
static void log_memory_usage(const char *context) {
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) return;
    
    char line[256];
    unsigned long vm_size = 0, vm_rss = 0, vm_peak = 0;
    
    while (fgets(line, sizeof(line), status)) {
        if (sscanf(line, "VmSize: %lu kB", &vm_size) == 1) continue;
        if (sscanf(line, "VmRSS: %lu kB", &vm_rss) == 1) continue;
        if (sscanf(line, "VmPeak: %lu kB", &vm_peak) == 1) continue;
    }
    fclose(status);
    
    LOG_INFO("[MEM-%s] VmSize: %lu KB, VmRSS: %lu KB, VmPeak: %lu KB", 
             context, vm_size, vm_rss, vm_peak);
    
    // Warn if memory usage seems excessive
    if (vm_rss > 500000) { // > 500 MB RSS
        LOG_WARN("[MEM-%s] High RSS memory usage: %lu KB", context, vm_rss);
    }
}

static void log_system_memory(void) {
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (!meminfo) return;
    
    char line[256];
    unsigned long mem_total = 0, mem_free = 0, mem_available = 0;
    
    while (fgets(line, sizeof(line), meminfo)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1) continue;
        if (sscanf(line, "MemFree: %lu kB", &mem_free) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1) continue;
    }
    fclose(meminfo);
    
    unsigned long mem_used = mem_total - mem_available;
    LOG_INFO("[SYS-MEM] Total: %lu KB, Used: %lu KB, Available: %lu KB", 
             mem_total, mem_used, mem_available);
    
    // Warn if system memory is low
    if (mem_available < 100000) { // < 100 MB available
        LOG_WARN("[SYS-MEM] Low system memory available: %lu KB", mem_available);
    }
}

// Convert NV12 frame to RGB texture
static bool convert_nv12_to_rgb_texture(v4l2_decoded_frame_t *frame, GLuint *texture) {
    if (!frame || !texture || !frame->data) {
        LOG_ERROR("Invalid frame data for NV12 conversion");
        return false;
    }
    
    // NV12 format: Y plane (width x height) followed by interleaved UV plane (width x height/2)
    uint32_t y_size = frame->width * frame->height;
    uint32_t uv_size = frame->width * frame->height / 2;
    
    if (frame->bytesused < y_size + uv_size) {
        LOG_ERROR("Frame data too small for NV12: %u < %u", frame->bytesused, y_size + uv_size);
        return false;
    }
    
    unsigned char *y_data = (unsigned char *)frame->data;
    unsigned char *uv_data = y_data + y_size;
    
    // Create or reuse texture
    if (*texture == 0) {
        glGenTextures(1, texture);
        LOG_DEBUG("Created new texture %u for NV12 conversion", *texture);
    }
    
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // For now, create a simple RGB texture from NV12 by doing basic YUV->RGB conversion on CPU
    // This is not optimal but will work for testing. GPU conversion would be better.
    unsigned char *rgb_data = malloc(frame->width * frame->height * 3);
    if (!rgb_data) {
        LOG_ERROR("Failed to allocate RGB conversion buffer");
        return false;
    }
    
    // Convert NV12 to RGB
    for (uint32_t y = 0; y < frame->height; y++) {
        for (uint32_t x = 0; x < frame->width; x++) {
            uint32_t y_idx = y * frame->width + x;
            uint32_t uv_idx = (y / 2U) * frame->width + (x & ~1U);
            
            int Y = (int)y_data[y_idx];
            int U = (int)uv_data[uv_idx] - 128;
            int V = (int)uv_data[uv_idx + 1] - 128;
            
            // YUV to RGB conversion (simplified)
            int R = Y + (int)(1.402f * (float)V);
            int G = Y - (int)(0.344f * (float)U) - (int)(0.714f * (float)V);
            int B = Y + (int)(1.772f * (float)U);
            
            // Clamp to valid range
            R = (R < 0) ? 0 : (R > 255) ? 255 : R;
            G = (G < 0) ? 0 : (G > 255) ? 255 : G;
            B = (B < 0) ? 0 : (B > 255) ? 255 : B;
            
            uint32_t rgb_idx = y_idx * 3;
            rgb_data[rgb_idx] = (unsigned char)R;
            rgb_data[rgb_idx + 1] = (unsigned char)G;
            rgb_data[rgb_idx + 2] = (unsigned char)B;
        }
    }
    
    // Upload RGB data to texture
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)frame->width, (GLsizei)frame->height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb_data);
    
    free(rgb_data);
    
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        LOG_ERROR("OpenGL error in NV12 conversion: 0x%x", gl_error);
        return false;
    }
    
    LOG_DEBUG("Successfully converted %dx%d NV12 frame to RGB texture", frame->width, frame->height);
    return true;
}

// Forward declaration for GPU-based NV12 conversion (implemented later after variable declarations)
static bool convert_nv12_to_rgb_texture_gpu(const v4l2_decoded_frame_t *frame, GLuint *rgb_texture);
#endif  // USE_V4L2_DECODER

// Exported globals for use in other modules
volatile sig_atomic_t g_stop = 0;

// Global terminal settings for signal handler restoration
static struct termios g_original_term;
static bool g_terminal_modified = false;

static void restore_terminal_state(void) {
	if (g_terminal_modified) {
		// Try to restore text console mode
		int console_fd = open("/dev/tty", O_RDWR);
		if (console_fd >= 0) {
			// Switch back to text mode
			ioctl(console_fd, KDSETMODE, KD_TEXT);
			close(console_fd);
		}
		
		tcsetattr(STDIN_FILENO, TCSANOW, &g_original_term);
		// Show cursor and reset terminal attributes (but keep output)
		fprintf(stderr, "\033[?25h");  // Show cursor
		fprintf(stderr, "\033[0m");    // Reset all attributes
		fprintf(stderr, "\n");         // Add newline to separate from any previous output
		fflush(stderr);
		g_terminal_modified = false;
	}
}

static void handle_sigint(int s){ 
	(void)s; 
	restore_terminal_state();
	g_stop = 1; 
}

static void handle_sigterm(int s){ 
	(void)s; 
	restore_terminal_state();
	g_stop = 1; 
}

static void handle_sigsegv(int s){
	(void)s;
	restore_terminal_state();
	void *bt[32];
	int n = backtrace(bt, 32);
	fprintf(stderr, "\n*** SIGSEGV captured, backtrace (%d frames):\n", n);
	backtrace_symbols_fd(bt, n, STDERR_FILENO);
	_exit(139);
}

// --- mpv OpenGL proc loader ---
static void *g_libegl = NULL;
static void *g_libgles = NULL;

// Initialize OpenGL library handles once
static void init_gl_proc_resolver(void) {
    if (g_libegl == NULL) {
        g_libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!g_libegl) g_libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        if (!g_libegl) LOG_WARN("Failed to dlopen libEGL.so.1 or libEGL.so");
    }
    
    if (g_libgles == NULL) {
        g_libgles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
        if (!g_libgles) g_libgles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
        if (!g_libgles) LOG_WARN("Failed to dlopen libGLESv2.so.2 or libGLESv2.so");
    }
}

__attribute__((unused)) static void cleanup_gl_proc_resolver(void) {
    if (g_libegl) {
        dlclose(g_libegl);
        g_libegl = NULL;
    }
    
    if (g_libgles) {
        dlclose(g_libgles);
        g_libgles = NULL;
    }
}

static void *mpv_get_proc_address(void *ctx, const char *name) {
    (void)ctx;
    
    // Initialize libraries if needed
    if (!g_libegl && !g_libgles) {
        init_gl_proc_resolver();
    }
    
    // Try to resolve the symbol from loaded libraries
    void *p = NULL;
    if (g_libegl) p = dlsym(g_libegl, name);
    if (!p && g_libgles) p = dlsym(g_libgles, name);
    if (!p) p = (void*)eglGetProcAddress(name);
    
    return p;
}

// Forward declaration for vsync toggle used before its definition
static int g_vsync_enabled;

// Forward declaration for display clearing function
static void clear_display_buffer(kms_ctx_t *d, egl_ctx_t *e);

// --- Preallocated FB ring (optional) ---
struct fb_ring_entry { struct gbm_bo *bo; uint32_t fb_id; };
struct fb_ring {
    struct fb_ring_entry *entries;
    int count;      // allocated entries
    int produced;   // how many unique BOs discovered during prealloc
    int active;     // number of BOs currently in use (for triple buffering)
    int next_index; // next index to use
};

// Typedefs for clarity
typedef struct fb_ring fb_ring_t;
typedef struct fb_ring_entry fb_ring_entry_t;

// Global state 
static fb_ring_t g_fb_ring = {0};
static int g_have_master = 0; // set if we successfully become DRM master
double g_video_fps = 60.0; // Detected video frame rate (default to 60fps)

#ifdef USE_FFMPEG_V4L2_PLAYER
int g_use_ffmpeg_v4l2 = 0; // Use FFmpeg V4L2 M2M decoder
static ffmpeg_v4l2_player_t ffmpeg_v4l2_player = {0}; // FFmpeg V4L2 player instance
static int g_v4l2_fallback_requested = 0; // Global flag for runtime fallback requests
static int g_v4l2_consecutive_failures = 0; // Count consecutive frame retrieval failures
// Increased threshold to handle V4L2 decoder buffering behavior - V4L2 M2M decoders
// may return EAGAIN many times while buffering, especially during seek operations
// or at stream boundaries. 500 failures * ~1-5ms per iteration = ~2.5 seconds max wait.
static const int MAX_V4L2_FAILURES = 500; // Max failures before triggering fallback
#endif

// These keystone settings are defined in keystone.c through pickle_keystone adapter
// Removed static variables and using extern from pickle_keystone.h

static int g_loop_playback = 0; // Whether to loop video playback

// Simple solid-color shader for drawing outlines/borders around keystone quad
static GLuint g_border_shader_program = 0;
static GLuint g_border_vertex_shader = 0;
static GLuint g_border_fragment_shader = 0;
static GLint  g_border_a_position_loc = -1;
static GLint  g_border_u_color_loc = -1;

// GPU-based NV12 to RGB conversion shader
GLuint g_nv12_shader_program = 0;
static GLuint g_nv12_vertex_shader = 0;
static GLuint g_nv12_fragment_shader = 0;
static GLint  g_nv12_a_position_loc = -1;
static GLint  g_nv12_a_tex_coord_loc = -1;
static GLint  g_nv12_u_texture_y_loc = -1;
static GLint  g_nv12_u_texture_uv_loc = -1;
static GLuint g_nv12_vao = 0;
static GLuint g_nv12_vbo = 0;

// Joystick/gamepad support
// Note: All joystick-related variables and functions are now in input.c

// Track Start+Select hold for safe quit
static bool g_js_start_down = false;
static bool g_js_select_down = false;
static struct timeval g_js_start_time = {0};
static struct timeval g_js_select_time = {0};
static bool g_js_quit_fired = false;

// 8BitDo controller button mappings are now defined in input.h
// JS_BUTTON_* and JS_AXIS_* definitions are no longer needed here

// Some controllers (incl. certain 8BitDo modes) report D-Pad as buttons.
// These indices are common but not universal; we handle them opportunistically.
#define JS_BUTTON_DPAD_UP     11
#define JS_BUTTON_DPAD_DOWN   12
#define JS_BUTTON_DPAD_LEFT   13
#define JS_BUTTON_DPAD_RIGHT  14

// 8BitDo controller axis mappings
#define JS_AXIS_LEFT_X     0
#define JS_AXIS_LEFT_Y     1
#define JS_AXIS_RIGHT_X    2
#define JS_AXIS_RIGHT_Y    3
#define JS_AXIS_L2         4
#define JS_AXIS_R2         5
#define JS_AXIS_DPAD_X     6
#define JS_AXIS_DPAD_Y     7

// Optional explicit ABXY mapping has been moved to input.c
// g_help_toggle_request is now defined in pickle_globals.c

// These functions have been moved to input.c

/* setup_label_mapping and label_to_code_default functions have been moved to input.c */

/* configure_special_buttons function has been moved to input.c */

// Removed forward declarations - functions are now in input.c module

bool ensure_drm_master(int fd) __attribute__((weak)); 
bool ensure_drm_master(int fd) {
	// Attempt to become DRM master; non-fatal if it fails (we may still page flip if compositor allows)
	if (drmSetMaster(fd) == 0) {
		LOG_DRM("Acquired master");
		g_have_master = 1;
		return true;
	}
	LOG_DRM("drmSetMaster failed (%s) – another process may own the display. Modeset might fail.", strerror(errno));
	g_have_master = 0;
	return false;
}

/**
 * Initialize DRM by scanning available cards and finding one with a connected display
 * 
 * @param d Pointer to kms_ctx structure to initialize
 * @return true on success, false on failure
 */
bool init_drm(kms_ctx_t *d) __attribute__((weak));
bool init_drm(kms_ctx_t *d) {
	memset(d, 0, sizeof(*d));
	d->fd = -1; // Initialize to invalid

	// Enumerate potential /dev/dri/card* nodes (0-15) to find one with resources + connected connector.
	// On Raspberry Pi 4 with full KMS (dtoverlay=vc4-kms-v3d), the primary render/display node
	// is typically card1 (card0 often firmware emulation or simpledrm early driver). We scan
	// all to stay generic across distro kernels.
	char path[32];
	bool found_card = false;
	
	for (int idx=0; idx<16; ++idx) {
		snprintf(path, sizeof(path), "/dev/dri/card%d", idx);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			continue; // skip silently; permission or non-existent
		}
		
		drmModeRes *res = drmModeGetResources(fd);
		if (!res) {
			LOG_DRM("card%d: drmModeGetResources failed: %s", idx, strerror(errno));
			close(fd);
			continue;
		}
		
		// Scan for a connected connector
		drmModeConnector *chosen = NULL;
		for (int i=0; i<res->count_connectors; ++i) {
			drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
			if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
				chosen = conn;
				break;
			}
			if (conn) drmModeFreeConnector(conn);
		}
		
		if (!chosen) {
			drmModeFreeResources(res);
			close(fd);
			continue; // try next card
		}
		
		// Found a suitable device
		d->fd = fd;
		d->res = res;
		d->connector = chosen;
		d->connector_id = chosen->connector_id;
		
		// Pick preferred mode if flagged, else first.
		d->mode = chosen->modes[0];
		for (int mi = 0; mi < chosen->count_modes; ++mi) {
			if (chosen->modes[mi].type & DRM_MODE_TYPE_PREFERRED) { 
				d->mode = chosen->modes[mi]; 
				break; 
			}
		}
		
		LOG_DRM("Selected card path %s", path);
		ensure_drm_master(fd);
		found_card = true;
		break;
	}
	
	if (!found_card || d->fd < 0 || !d->connector) {
		LOG_ERROR("Failed to locate a usable DRM device");
		LOG_ERROR("Troubleshooting: Ensure vc4 KMS overlay enabled and you have permission (try sudo or be in 'video' group)");
		return false;
	}

	// Find encoder for connector
	if (d->connector->encoder_id) {
		d->encoder = drmModeGetEncoder(d->fd, d->connector->encoder_id);
	}
	
	if (!d->encoder) {
		for (int i=0; i<d->connector->count_encoders; ++i) {
			d->encoder = drmModeGetEncoder(d->fd, d->connector->encoders[i]);
			if (d->encoder) break;
		}
	}
	
	if (!d->encoder) {
		LOG_ERROR("No encoder found for connector %u", d->connector_id);
		return false;
	}
	
	d->crtc_id = d->encoder->crtc_id;
	d->orig_crtc = drmModeGetCrtc(d->fd, d->crtc_id);
	
	if (!d->orig_crtc) {
		LOG_ERROR("Failed to get original CRTC (%s)", strerror(errno));
		return false;
	}
	
	LOG_DRM("Using card with fd=%d connector=%u mode=%s %ux%u@%u", 
		d->fd, d->connector_id, d->mode.name, 
		d->mode.hdisplay, d->mode.vdisplay, d->mode.vrefresh);

	return true;
}

/**
 * Clean up DRM resources and restore original CRTC state
 * 
 * @param d Pointer to kms_ctx structure to clean up
 */
void deinit_drm(kms_ctx_t *d) __attribute__((weak));
void deinit_drm(kms_ctx_t *d) {
	if (!d) return;
	
	if (d->orig_crtc) {
		// Restore original CRTC configuration
		drmModeSetCrtc(d->fd, d->orig_crtc->crtc_id, d->orig_crtc->buffer_id,
					   d->orig_crtc->x, d->orig_crtc->y, &d->connector_id, 1, &d->orig_crtc->mode);
		drmModeFreeCrtc(d->orig_crtc);
		d->orig_crtc = NULL;
	}
	
	if (d->encoder) {
		drmModeFreeEncoder(d->encoder);
		d->encoder = NULL;
	}
	
	if (d->connector) {
		drmModeFreeConnector(d->connector);
		d->connector = NULL;
	}
	
	if (d->res) {
		drmModeFreeResources(d->res);
		d->res = NULL;
	}
	
	if (d->fd >= 0) {
		// Drop DRM master if we had it
		if (g_have_master) {
			drmDropMaster(d->fd);
			g_have_master = 0;
		}
		close(d->fd);
		d->fd = -1;
	}
}

/**
 * Initialize GBM and EGL for OpenGL ES rendering
 * 
 * @param d Pointer to initialized DRM context
 * @param e Pointer to EGL context structure to initialize
 * @return true on success, false on failure
 */
bool init_gbm_egl(const kms_ctx_t *d, egl_ctx_t *e) __attribute__((weak));
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
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 0,
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
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	e->ctx = eglCreateContext(e->dpy, e->config, EGL_NO_CONTEXT, ctx_attr);
	if (e->ctx == EGL_NO_CONTEXT) {
		RETURN_ERROR_EGL("eglCreateContext failed");
	}
	
	// Create window surface
	EGLint win_attrs[] = { EGL_NONE };
	e->surf = eglCreateWindowSurface(e->dpy, e->config, (EGLNativeWindowType)e->gbm_surf, win_attrs);
	
	if (e->surf == EGL_NO_SURFACE) {
		LOG_EGL("eglCreateWindowSurface failed -> trying with alpha config fallback");
		
		// Retry with alpha-enabled config if original lacked alpha
		EGLint retry_attrs[] = {
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
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
		
	return true;
}

/**
 * Clean up GBM and EGL resources
 * 
 * @param e Pointer to EGL context structure to clean up
 */
void deinit_gbm_egl(egl_ctx_t *e) __attribute__((weak));
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

// MPV rendering integration
#ifndef EVENT_DRIVEN_ENABLED
typedef struct {
	mpv_handle *mpv;             // MPV API handle
	mpv_render_context *rctx;    // MPV render context for OpenGL rendering
	int using_libmpv;            // Flag indicating fallback to vo=libmpv occurred
} mpv_player_t;

#ifdef USE_FFMPEG_V4L2_PLAYER
// Forward declaration for init_mpv function
static bool init_mpv(mpv_player_t *p, const char *file);

// Runtime fallback from FFmpeg V4L2 to MPV
static bool fallback_to_mpv(const char *file, mpv_player_t *player) {
	LOG_WARN("FFmpeg V4L2 decoder failed during playback, falling back to MPV");
	
	// Clean up FFmpeg V4L2 resources
	cleanup_ffmpeg_v4l2_player(&ffmpeg_v4l2_player);
	memset(&ffmpeg_v4l2_player, 0, sizeof(ffmpeg_v4l2_player));
	g_use_ffmpeg_v4l2 = 0;
	
	// Initialize MPV
	memset(player, 0, sizeof(*player));
	if (!init_mpv(player, file)) {
		LOG_ERROR("MPV fallback initialization failed");
		return false;
	}
	
	g_mpv_wakeup = 1;
	LOG_INFO("Successfully fell back to MPV decoder");
	return true;
}
#endif

// V4L2 decoder integration is defined in v4l2_player.h
#include "v4l2_player.h"
#endif // EVENT_DRIVEN_ENABLED

// Wakeup callback sets a flag so main loop knows mpv wants processing.
volatile int g_mpv_wakeup = 0;
int g_mpv_pipe[2] = {-1,-1}; // pipe to integrate mpv wakeups into poll loop
static void mpv_wakeup_cb(void *ctx) {
	(void)ctx;
	g_mpv_wakeup = 1;
	if (g_mpv_pipe[1] >= 0) {
		unsigned char b = 0;
		if (write(g_mpv_pipe[1], &b, 1) < 0) { /* ignore EAGAIN */ }
	}
}
volatile uint64_t g_mpv_update_flags = 0; // bitmask from mpv_render_context_update
volatile int g_clear_display = 0; // Flag to clear display on next render
static void on_mpv_events(void *data) { (void)data; g_mpv_wakeup = 1; }

// Performance controls
static int g_triple_buffer = 1;         // Enable triple buffering by default
static int g_vsync_enabled = 1;         // Enable vsync by default
static int g_frame_timing_enabled = 0;  // Detailed frame timing metrics (when PICKLE_TIMING=1)

// Texture orientation controls (used in keystone pass only)
static int g_tex_flip_x = 0; // 1 = mirror horizontally (left/right)
static int g_tex_flip_y = 0; // 1 = flip vertically (top/bottom)
int g_help_visible = 0; // toggle state for help overlay

// --- Statistics ---
static int g_stats_enabled = 0;
static double g_stats_interval_sec = 2.0; // default
static uint64_t g_stats_frames = 0;
static struct timeval g_stats_start = {0};
static struct timeval g_stats_last = {0};
static uint64_t g_stats_last_frames = 0;
// Program start (for watchdogs)
static struct timeval g_prog_start = {0};
// Playback monitoring
static struct timeval g_last_frame_time = {0};
static int g_stall_reset_count = 0;
static int g_max_stall_resets = 3; // Maximum stall recovery attempts before giving up
// Watchdog timeouts
static int g_wd_first_ms = 1500; // 1.5s
static int g_wd_ongoing_ms = 3000; // 3s max between frames during playback (adjustable for looping)

// Frame timing/pacing metrics
static struct timeval g_last_flip_submit = {0};
static struct timeval g_last_flip_complete = {0};
static double g_min_flip_time = 1000.0;
static double g_max_flip_time = 0.0;
static double g_avg_flip_time = 0.0;
static int g_flip_count = 0;
static int g_pending_flips = 0;  // Track number of page flips in flight

// Saved OSD settings for help overlay placement
static struct {
	int saved;
	int64_t font_size;
	int64_t margin_x;
	int64_t margin_y;
	char align_x[8];
	char align_y[8];
} g_osd_saved = {0};

// We're using the tv_diff from utils.h now instead of this version
// tv_diff is defined in utils.c

static void stats_log_periodic(mpv_player_t *p) {
	if (!g_stats_enabled) return;
	struct timeval now; gettimeofday(&now, NULL);
	double since_last = tv_diff(&g_stats_last, &now);
	if (since_last < g_stats_interval_sec) return;
	double total = tv_diff(&g_stats_start, &now);
	uint64_t frames_now = g_stats_frames;
	uint64_t delta_frames = frames_now - g_stats_last_frames;
	double inst_fps = (since_last > 0.0) ? (double)delta_frames / since_last : 0.0;
	double avg_fps  = (total > 0.0) ? (double)frames_now / total : 0.0;
	// Query mpv drop stats if possible
	int64_t drop_dec = 0, drop_vo = 0;
	if (p && p->mpv) {
		mpv_get_property(p->mpv, "drop-frame-count", MPV_FORMAT_INT64, &drop_dec);
		mpv_get_property(p->mpv, "vo-drop-frame-count", MPV_FORMAT_INT64, &drop_vo);
	}
	fprintf(stderr, "[stats] total=%.2fs frames=%llu avg_fps=%.2f inst_fps=%.2f dropped_dec=%lld dropped_vo=%lld\n",
			total, (unsigned long long)frames_now, avg_fps, inst_fps,
			(long long)drop_dec, (long long)drop_vo);
	
	// Update adaptive frame pacing
	decoder_pacing_update(avg_fps, (uint64_t)drop_vo);
	
	g_stats_last = now;
	g_stats_last_frames = frames_now;
}

static void stats_log_final(mpv_player_t *p) {
	if (!g_stats_enabled) return;
	struct timeval now; gettimeofday(&now, NULL);
	double total = tv_diff(&g_stats_start, &now);
	double avg_fps = (total > 0.0) ? (double)g_stats_frames / total : 0.0;
	int64_t drop_dec = 0, drop_vo = 0;
	if (p && p->mpv) {
		mpv_get_property(p->mpv, "drop-frame-count", MPV_FORMAT_INT64, &drop_dec);
		mpv_get_property(p->mpv, "vo-drop-frame-count", MPV_FORMAT_INT64, &drop_vo);
	}
	fprintf(stderr, "[stats-final] duration=%.2fs frames=%llu avg_fps=%.2f dropped_dec=%lld dropped_vo=%lld\n",
			total, (unsigned long long)g_stats_frames, avg_fps, (long long)drop_dec, (long long)drop_vo);
	
	// Print frame timing stats if enabled
	if (g_frame_timing_enabled && g_flip_count > 0) {
		fprintf(stderr, "[timing-final] flip_time: min=%.2fms avg=%.2fms max=%.2fms count=%d\n",
			g_min_flip_time * 1000.0, g_avg_flip_time * 1000.0, g_max_flip_time * 1000.0, g_flip_count);
	}
}

// Display a help overlay using mpv's built-in OSD
void show_help_overlay(mpv_handle *mpv) {
	if (!mpv) return;
	const char *text =
		"Pickle controls:\n"
		"  q: quit    h: help overlay\n"
		"  k: toggle keystone    1-4: select corner\n"
		"  arrows: move point\n"
		"  +/-: step    r: reset\n"
		"  b: toggle border    [ / ]: border width\n"
	"  c: toggle corner markers\n"
		"  o: flip X (mirror)  p: flip Y (invert)\n"
		"  m: mesh mode (experimental)\n"
		"  s/S: save keystone\n"
		"\nGamepad:\n"
		"  START: toggle keystone mode\n"
		"  SELECT: reset keystone to defaults\n"
		"  X button: cycle corners (TL -> TR -> BR -> BL)\n"
		"  B button: toggle border/help display\n"
		"  HOME/Guide: toggle border display\n"
		"  Left stick: move selected corner\n"
		"  L1/R1 together: toggle keystone mode\n"
		"  L1/R1 individual: adjust step size\n"
		"  START+SELECT (hold 2s): quit application\n";
	// Reduce font size and move OSD to top-left with small margins so it fits better
	if (!g_osd_saved.saved) {
		int64_t v=0; char *s=NULL;
		if (mpv_get_property(mpv, "osd-font-size", MPV_FORMAT_INT64, &v) >= 0) g_osd_saved.font_size = v; else g_osd_saved.font_size = 36;
		if (mpv_get_property(mpv, "osd-margin-x", MPV_FORMAT_INT64, &v) >= 0) g_osd_saved.margin_x = v; else g_osd_saved.margin_x = 10;
		if (mpv_get_property(mpv, "osd-margin-y", MPV_FORMAT_INT64, &v) >= 0) g_osd_saved.margin_y = v; else g_osd_saved.margin_y = 10;
		if (mpv_get_property(mpv, "osd-align-x", MPV_FORMAT_STRING, &s) >= 0 && s) {
			strncpy(g_osd_saved.align_x, s, sizeof(g_osd_saved.align_x)-1);
			g_osd_saved.align_x[sizeof(g_osd_saved.align_x)-1]='\0';
			mpv_free(s);
		} else strcpy(g_osd_saved.align_x, "center");
		s = NULL;
		if (mpv_get_property(mpv, "osd-align-y", MPV_FORMAT_STRING, &s) >= 0 && s) {
			strncpy(g_osd_saved.align_y, s, sizeof(g_osd_saved.align_y)-1);
			g_osd_saved.align_y[sizeof(g_osd_saved.align_y)-1]='\0';
			mpv_free(s);
		} else strcpy(g_osd_saved.align_y, "center");
		g_osd_saved.saved = 1;
	}

	int64_t small = 20;
	int64_t mx = 12;
	int64_t my = 12;
	const char *ax = "left";
	const char *ay = "top";
	mpv_set_property(mpv, "osd-font-size", MPV_FORMAT_INT64, &small);
	mpv_set_property(mpv, "osd-margin-x", MPV_FORMAT_INT64, &mx);
	mpv_set_property(mpv, "osd-margin-y", MPV_FORMAT_INT64, &my);
	// Use mpv_set_property_string for string properties to avoid char** misuse
	mpv_set_property_string(mpv, "osd-align-x", ax);
	mpv_set_property_string(mpv, "osd-align-y", ay);

	const char *cmd[] = { "show-text", text, "600000", NULL }; // long duration; we'll clear on toggle
	mpv_command(mpv, cmd);
}

void hide_help_overlay(mpv_handle *mpv) {
	if (!mpv) return;
	// Clear by showing empty text for 1ms
	const char *cmd[] = { "show-text", "", "1", NULL };
	mpv_command(mpv, cmd);
	// Restore previous OSD settings if we saved them
	if (g_osd_saved.saved) {
		mpv_set_property(mpv, "osd-font-size", MPV_FORMAT_INT64, &g_osd_saved.font_size);
		mpv_set_property(mpv, "osd-margin-x", MPV_FORMAT_INT64, &g_osd_saved.margin_x);
		mpv_set_property(mpv, "osd-margin-y", MPV_FORMAT_INT64, &g_osd_saved.margin_y);
		mpv_set_property_string(mpv, "osd-align-x", g_osd_saved.align_x);
		mpv_set_property_string(mpv, "osd-align-y", g_osd_saved.align_y);
		g_osd_saved.saved = 0;
	}
}

// Using log_opt_result from utils.c now instead of this static version

static bool init_mpv(mpv_player_t *p, const char *file) {
	memset(p,0,sizeof(*p));
	const char *no_mpv = getenv("PICKLE_NO_MPV");
	if (no_mpv && *no_mpv) {
		fprintf(stderr, "[mpv] Skipping mpv initialization (PICKLE_NO_MPV set)\n");
		return true;
	}
	p->mpv = mpv_create();
	if (!p->mpv) { fprintf(stderr, "mpv_create failed\n"); return false; }
	const char *want_debug = getenv("PICKLE_LOG_MPV");
	if (want_debug && *want_debug) mpv_request_log_messages(p->mpv, "debug"); else mpv_request_log_messages(p->mpv, "warn");

	// Production defaults: vo=gpu, no forced custom gpu-context unless explicitly requested.
	int r=0;
	const char *deprecated_force = getenv("PICKLE_FORCE_LIBMPV");
	const char *deprecated_no_custom = getenv("PICKLE_NO_CUSTOM_CTX");
	if (deprecated_force && *deprecated_force)
		fprintf(stderr, "[mpv] WARNING: PICKLE_FORCE_LIBMPV deprecated; use PICKLE_VO=libmpv if required.\n");
	if (deprecated_no_custom && *deprecated_no_custom)
		fprintf(stderr, "[mpv] WARNING: PICKLE_NO_CUSTOM_CTX deprecated; custom context disabled by default now.\n");

	const char *vo_req = getenv("PICKLE_VO");
	if (!vo_req || !*vo_req) vo_req = "libmpv"; // Changed default from "gpu" to avoid conflicts
	r = mpv_set_option_string(p->mpv, "vo", vo_req);
	if (r < 0) {
		fprintf(stderr, "[mpv] vo=%s failed (%d); falling back to vo=libmpv\n", vo_req, r);
		vo_req = "libmpv";
		r = mpv_set_option_string(p->mpv, "vo", "libmpv");
		log_opt_result("vo=libmpv", r);
	}
	const char *vo_used = vo_req;
	const char *hwdec_pref = getenv("PICKLE_HWDEC");
	if (!hwdec_pref || !*hwdec_pref) hwdec_pref = "auto-safe";
	r = mpv_set_option_string(p->mpv, "hwdec", hwdec_pref); log_opt_result("hwdec", r);
	r = mpv_set_option_string(p->mpv, "opengl-es", "yes"); log_opt_result("opengl-es=yes", r);
	
	// Video sync mode for better frame timing
	const char *video_sync = g_vsync_enabled ? "display-resample" : "audio";
	r = mpv_set_option_string(p->mpv, "video-sync", video_sync);
	log_opt_result("video-sync", r);
	
	// Optimize queue size based on performance mode
	const char *queue_size = g_vsync_enabled ? "4" : "1";  // Reduce queue for lower latency when vsync off
	r = mpv_set_option_string(p->mpv, "vo-queue-size", queue_size);
	log_opt_result("vo-queue-size", r);
	
	// Configure loop behavior if enabled
	if (g_loop_playback) {
		r = mpv_set_option_string(p->mpv, "loop-file", "inf");
		log_opt_result("loop-file", r);
		r = mpv_set_option_string(p->mpv, "loop-playlist", "inf");
		log_opt_result("loop-playlist", r);
	}
	
	// Performance-based cache settings
	if (g_vsync_enabled) {
		// Larger cache for quality when vsync enabled
		r = mpv_set_option_string(p->mpv, "demuxer-max-bytes", "64MiB");
		log_opt_result("demuxer-max-bytes", r);
		r = mpv_set_option_string(p->mpv, "cache-secs", "10");
		log_opt_result("cache-secs", r);
	} else {
		// Smaller cache for lower latency when performance mode
		r = mpv_set_option_string(p->mpv, "demuxer-max-bytes", "8MiB");
		log_opt_result("demuxer-max-bytes (perf)", r);
		r = mpv_set_option_string(p->mpv, "cache-secs", "1");
		log_opt_result("cache-secs (perf)", r);
		
		// Limit decoder queue to prevent overruns
		r = mpv_set_option_string(p->mpv, "vd-queue-max-bytes", "4MiB");
		log_opt_result("vd-queue-max-bytes", r);
		r = mpv_set_option_string(p->mpv, "vd-queue-max-samples", "3");
		log_opt_result("vd-queue-max-samples", r);
		
		// Use smart frame dropping - only VO drops, not decoder drops
		// This reduces waste while maintaining performance
		r = mpv_set_option_string(p->mpv, "framedrop", "vo");
		log_opt_result("framedrop (smart)", r);
		
		// Optimize threading for performance
		r = mpv_set_option_string(p->mpv, "vd-lavc-threads", "4");
		log_opt_result("vd-lavc-threads", r);
		r = mpv_set_option_string(p->mpv, "ad-lavc-threads", "2");
		log_opt_result("ad-lavc-threads", r);
	}
	
	// Set a larger audio buffer for smoother audio output
	r = mpv_set_option_string(p->mpv, "audio-buffer", "0.2");  // 200ms audio buffer
	log_opt_result("audio-buffer", r);
	
	// Prefer using MPV_RENDER_PARAM_FLIP_Y during rendering instead of global rotation

	const char *ctx_override = getenv("PICKLE_GPU_CONTEXT");
	int forced_headless = getenv("PICKLE_FORCE_HEADLESS") ? 1 : 0;
	int headless_attempted = 0;
	if (ctx_override && *ctx_override && strcmp(vo_used, "gpu") == 0) {
		int rc = mpv_set_option_string(p->mpv, "gpu-context", ctx_override);
		log_opt_result("gpu-context (override)", rc);
	} else if (strcmp(vo_used, "gpu") == 0) {
		// Always try to avoid DRM contexts that conflict with our own DRM usage
		const char *try_contexts[] = {"x11egl", "waylandvk", "wayland", "x11vk", "displayvk", NULL};
		int ctx_set = 0;
		for (int i = 0; try_contexts[i] && !ctx_set; i++) {
			int rc = mpv_set_option_string(p->mpv, "gpu-context", try_contexts[i]);
			if (rc >= 0) {
				fprintf(stderr, "[mpv] Using gpu-context=%s to avoid DRM conflicts\n", try_contexts[i]);
				ctx_set = 1;
				break;
			}
		}
		if (!ctx_set && (forced_headless || (!g_have_master && !getenv("PICKLE_DISABLE_HEADLESS")))) {
			int rc = mpv_set_option_string(p->mpv, "gpu-context", "headless");
			if (rc < 0) {
				fprintf(stderr, "[mpv] gpu-context=headless unsupported (%d); will proceed without it.\n", rc);
			} else {
				fprintf(stderr, "[mpv] Using gpu-context=headless (%s).\n", forced_headless?"forced":"auto");
				headless_attempted = 1;
			}
		}
	}
	if (strcmp(vo_used, "gpu") == 0) {
		mpv_set_option_string(p->mpv, "terminal", "no");
		mpv_set_option_string(p->mpv, "input-default-bindings", "no");
		mpv_set_option_string(p->mpv, "input-vo-keyboard", "no");  // Disable mpv's keyboard handling
		mpv_set_option_string(p->mpv, "input-cursor", "no");       // Disable cursor handling
		mpv_set_option_string(p->mpv, "input-media-keys", "no");   // Disable media key handling
		// Prevent mpv from attempting any DRM/KMS operations since we handle display ourselves
		if (!getenv("PICKLE_KEEP_ATOMIC")) {
			mpv_set_option_string(p->mpv, "drm-atomic", "no");
			mpv_set_option_string(p->mpv, "drm-mode", "");
			mpv_set_option_string(p->mpv, "drm-connector", "");
			mpv_set_option_string(p->mpv, "drm-device", "");
		}
	}

	int use_adv = 0;
	const char *adv_env = getenv("PICKLE_GL_ADV");
	if (adv_env && *adv_env && strcmp(vo_used, "gpu") == 0) use_adv = 1;
	fprintf(stderr, "[mpv] Advanced control %s (PICKLE_GL_ADV=%s vo=%s)\n", use_adv?"ENABLED":"disabled", adv_env?adv_env:"unset", vo_used);

	int disable_audio = 0;
	if (getenv("PICKLE_NO_AUDIO")) { fprintf(stderr, "[mpv] Disabling audio (PICKLE_NO_AUDIO set)\n"); disable_audio = 1; }
	if (!disable_audio && !getenv("PICKLE_FORCE_AUDIO")) {
		if (getuid() == 0) {
			const char *xdg = getenv("XDG_RUNTIME_DIR");
			if (!xdg || !*xdg) { fprintf(stderr, "[mpv] XDG_RUNTIME_DIR missing under root; disabling audio (set PICKLE_FORCE_AUDIO=1 to override)\n"); disable_audio = 1; }
		}
	}
	if (disable_audio) mpv_set_option_string(p->mpv, "audio", "no");
	if (mpv_initialize(p->mpv) < 0) { fprintf(stderr, "mpv_initialize failed\n"); return false; }

	mpv_opengl_init_params gl_init = { .get_proc_address = mpv_get_proc_address, .get_proc_address_ctx = NULL };
	mpv_render_param params[4]; memset(params,0,sizeof(params)); int pi=0;
	params[pi].type = MPV_RENDER_PARAM_API_TYPE; params[pi++].data = (void*)MPV_RENDER_API_TYPE_OPENGL;
	params[pi].type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS; params[pi++].data = &gl_init;
	if (use_adv) { params[pi].type = MPV_RENDER_PARAM_ADVANCED_CONTROL; params[pi++].data = (void*)1; }
	params[pi].type = 0;
	fprintf(stderr, "[mpv] Creating render context (advanced_control=%d vo=%s) ...\n", use_adv, vo_used);
	int cr = mpv_render_context_create(&p->rctx, p->mpv, params);
	if (cr < 0 && strcmp(vo_used, "gpu") == 0 && !forced_headless && !headless_attempted) {
		// Retry once with libmpv fallback for compatibility
		fprintf(stderr, "[mpv] render context create failed (%d); retrying with vo=libmpv\n", cr);
		mpv_terminate_destroy(p->mpv); p->mpv=NULL; p->rctx=NULL;
		p->mpv = mpv_create();
		if (!p->mpv) { fprintf(stderr, "mpv_create (retry) failed\n"); return false; }
		if (want_debug && *want_debug) mpv_request_log_messages(p->mpv, "debug"); else mpv_request_log_messages(p->mpv, "warn");
		mpv_set_option_string(p->mpv, "vo", "libmpv");
		mpv_set_option_string(p->mpv, "hwdec", hwdec_pref);
		if (disable_audio) mpv_set_option_string(p->mpv, "audio", "no");
		if (mpv_initialize(p->mpv) < 0) { fprintf(stderr, "mpv_initialize (libmpv retry) failed\n"); return false; }
		p->using_libmpv = 1;
		cr = mpv_render_context_create(&p->rctx, p->mpv, params);
	}
	if (cr < 0) { fprintf(stderr, "mpv_render_context_create failed (%d)\n", cr); return false; }
	fprintf(stderr, "[mpv] Render context OK\n");
	
	// Initialize GL state cache for optimizations
	gl_state_init();
	
	// Initialize adaptive frame pacing
	double initial_fps = g_vsync_enabled ? 30.0 : 50.0; // Conservative targets
	decoder_pacing_init(initial_fps);
	
	mpv_render_context_set_update_callback(p->rctx, on_mpv_events, NULL);
	mpv_set_wakeup_callback(p->mpv, mpv_wakeup_cb, NULL);
	const char *cmd[] = {"loadfile", file, NULL};
	if (mpv_command(p->mpv, cmd) < 0) { fprintf(stderr, "Failed to load file %s\n", file); return false; }
	
	// Signal that display should be cleared before next render
	g_clear_display = 1;
	
	fprintf(stderr, "[mpv] Initialized successfully (vo=%s)\n", vo_used);
	return true;
}

/**
 * Clean up MPV resources
 * 
 * @param p Pointer to MPV player structure to clean up
 */
static void destroy_mpv(mpv_player_t *p) {
	if (!p) return;
	
	if (p->rctx) {
		mpv_render_context_free(p->rctx);
		p->rctx = NULL;
	}
	
	if (p->mpv) {
		mpv_terminate_destroy(p->mpv);
		p->mpv = NULL;
	}
}

#ifdef USE_V4L2_DECODER
/**
 * Initialize the V4L2 decoder
 * 
 * @param p Pointer to V4L2 player structure
 * @param file Path to the video file to play
 * @return true if initialization succeeded, false otherwise
 */
static bool init_v4l2_decoder(v4l2_player_t *p, const char *file) {
	if (!p) return false;
	
	// Check file extension and demuxer availability
	const char *ext = strrchr(file, '.');
	if (ext && (strcasecmp(ext, ".mp4") == 0 || strcasecmp(ext, ".mkv") == 0 || 
	            strcasecmp(ext, ".avi") == 0 || strcasecmp(ext, ".mov") == 0 || 
	            strcasecmp(ext, ".webm") == 0)) {
		// Container format detected - check if demuxer is available
#if defined(USE_V4L2_DECODER) && defined(ENABLE_V4L2_DEMUXER)
		if (v4l2_demuxer_is_available()) {
			LOG_INFO("Container format detected (%s), V4L2 demuxer available - proceeding with hardware decode", ext);
		} else {
			LOG_WARN("Container format detected (%s), V4L2 demuxer not available - falling back to MPV", ext);
			return false;
		}
#else
		LOG_INFO("Container format detected (%s), V4L2 demuxer disabled - using MPV directly", ext);
		return false; // Demuxer not compiled in, use MPV
#endif
	}
	
	// Check if V4L2 decoder is supported
	if (!v4l2_decoder_is_supported()) {
		LOG_ERROR("V4L2 decoder is not supported on this platform");
		return false;
	}
	
	// Allocate the decoder
	p->decoder = malloc(sizeof(v4l2_decoder_t));
	if (!p->decoder) {
		LOG_ERROR("Failed to allocate V4L2 decoder");
		return false;
	}
	
#ifdef USE_V4L2_DECODER
	// V4L2 demuxer temporarily disabled - need packet callback integration
	p->use_demuxer = false;
	LOG_INFO("V4L2 demuxer integration not yet complete - using raw stream mode");
	
	// TODO: Implement packet callback and full demuxer integration
	// The demuxer requires a callback function to deliver packets
	// This needs more integration work with the frame processing pipeline
	
#if 0 && defined(ENABLE_V4L2_DEMUXER)
	// This will be enabled once we implement the packet callback
	if (v4l2_demuxer_is_available()) {
		// Need to create a callback function first
		p->demuxer = v4l2_demuxer_create(file, packet_callback_func, p);
		if (p->demuxer) {
			p->use_demuxer = true;
			LOG_INFO("V4L2 demuxer initialized successfully for container format");
		} else {
			LOG_WARN("V4L2 demuxer failed to initialize, falling back to raw stream");
			p->use_demuxer = false;
		}
	} else {
		p->use_demuxer = false;
	}
#endif
	
	if (0) { // Old demuxer code disabled
		/*
		// Try to initialize MP4 demuxer first
		if (g_debug) {
			fprintf(stderr, "[DEBUG] Attempting MP4 demuxer init for file: %s\n", file);
		}
		if (mp4_demuxer_init(&p->demuxer, file)) {
		if (g_debug) {
			fprintf(stderr, "[DEBUG] MP4 demuxer init SUCCESS\n");
		}
		p->use_demuxer = true;
		
		// Get stream information from demuxer
		const char *codec_name;
		double fps;
		if (!mp4_demuxer_get_stream_info(&p->demuxer, &p->width, &p->height, &fps, &codec_name)) {
			LOG_ERROR("Failed to get stream info from MP4 demuxer");
			mp4_demuxer_cleanup(&p->demuxer);
			free(p->decoder);
			p->decoder = NULL;
			return false;
		}
		
		// Convert FFmpeg codec ID to V4L2 codec
		if (!mp4_demuxer_is_codec_supported(&p->demuxer)) {
			LOG_ERROR("Codec %s not supported by V4L2 hardware decoder", codec_name);
			mp4_demuxer_cleanup(&p->demuxer);
			free(p->decoder);
			p->decoder = NULL;
			return false;
		}
		
		// Map codec to V4L2 codec enum
		switch (p->demuxer.codec_id) {
			case AV_CODEC_ID_H264:
				p->codec = V4L2_CODEC_H264;
				break;
			case AV_CODEC_ID_HEVC:
				p->codec = V4L2_CODEC_HEVC;
				break;
			case AV_CODEC_ID_VP8:
				p->codec = V4L2_CODEC_VP8;
				break;
			case AV_CODEC_ID_VP9:
				p->codec = V4L2_CODEC_VP9;
				break;
			default:
				LOG_ERROR("Unsupported codec ID: %d", p->demuxer.codec_id);
				mp4_demuxer_cleanup(&p->demuxer);
				free(p->decoder);
				p->decoder = NULL;
				return false;
		}
		
		// Store the detected FPS globally for adaptive frame timing
		g_video_fps = fps;
		LOG_INFO("Using MP4 demuxer: %s %dx%d @ %.2f fps", codec_name, p->width, p->height, fps);
		*/
	} else {
#endif
		if (g_debug) {
			fprintf(stderr, "[DEBUG] MP4 demuxer init FAILED, falling back to raw file\n");
		}
		// Fallback to raw file reading
#ifdef USE_V4L2_DECODER
		p->use_demuxer = false;
#endif
		p->input_file = fopen(file, "rb");
		if (!p->input_file) {
			LOG_ERROR("Failed to open input file: %s", file);
			free(p->decoder);
			p->decoder = NULL;
			return false;
		}
		
		// For raw streams, assume H.264 codec
		p->codec = V4L2_CODEC_H264;
		p->width = 1920;  // Default values
		p->height = 1080;
		
		LOG_INFO("Using raw stream mode (no demuxer)");
#ifdef USE_V4L2_DECODER
	}
#endif
	
	LOG_INFO("Initializing V4L2 decoder for file: %s (codec: H.264, initial size: %dx%d)",
		file, p->width, p->height);
	
	// Initialize the decoder
	if (!v4l2_decoder_init(p->decoder, p->codec, p->width, p->height)) {
		LOG_ERROR("Failed to initialize V4L2 decoder");
		fclose(p->input_file);
		// Don't free the decoder memory here - v4l2_decoder_init already frees it on failure
		p->decoder = NULL;
		return false;
	}
	
	// Set up DMA-BUF for zero-copy
	if (!v4l2_decoder_use_dmabuf(p->decoder)) {
		LOG_WARN("DMA-BUF not supported, falling back to memory copy");
	}
	
	LOG_INFO("Initialized V4L2 decoder for codec: %s, resolution: %dx%d", 
		p->codec == V4L2_CODEC_H264 ? "H.264" :
		p->codec == V4L2_CODEC_HEVC ? "HEVC" :
		p->codec == V4L2_CODEC_VP8 ? "VP8" :
		p->codec == V4L2_CODEC_VP9 ? "VP9" : "Unknown",
		p->width, p->height);
	
	// Allocate buffers (configurable via environment variables)
	int input_buffers = 8, output_buffers = 8;
	const char *env_input_bufs = getenv("PICKLE_V4L2_INPUT_BUFFERS");
	const char *env_output_bufs = getenv("PICKLE_V4L2_OUTPUT_BUFFERS");
	if (env_input_bufs) {
		int val = atoi(env_input_bufs);
		if (val > 0 && val <= 32) input_buffers = val; // Reasonable bounds
	}
	if (env_output_bufs) {
		int val = atoi(env_output_bufs);
		if (val > 0 && val <= 32) output_buffers = val; // Reasonable bounds
	}
	
	if (!v4l2_decoder_allocate_buffers(p->decoder, input_buffers, output_buffers)) {
		LOG_ERROR("Failed to allocate V4L2 decoder buffers");
		v4l2_decoder_destroy(p->decoder);
		fclose(p->input_file);
		// Don't free the decoder memory - v4l2_decoder_destroy already does this
		p->decoder = NULL;
		return false;
	}
	
	// Start the decoder
	if (!v4l2_decoder_start(p->decoder)) {
		LOG_ERROR("Failed to start V4L2 decoder");
		v4l2_decoder_destroy(p->decoder);
		fclose(p->input_file);
		// Don't free the decoder memory - v4l2_decoder_destroy already does this
		p->decoder = NULL;
		return false;
	}
	
	// Allocate read buffer (configurable via environment variable)
	p->buffer_size = 1024 * 1024;  // Default 1MB buffer
	const char *env_buffer_size = getenv("PICKLE_V4L2_BUFFER_SIZE_MB");
	if (env_buffer_size) {
		int mb = atoi(env_buffer_size);
		if (mb > 0 && mb <= 64) { // Reasonable bounds: 1-64MB
			p->buffer_size = (size_t)mb * 1024 * 1024;
		}
	}
	p->buffer = malloc(p->buffer_size);
	if (!p->buffer) {
		LOG_ERROR("Failed to allocate read buffer");
		v4l2_decoder_destroy(p->decoder);
		fclose(p->input_file);
		// Don't free the decoder memory - v4l2_decoder_destroy already does this
		p->decoder = NULL;
		return false;
	}
	
	p->timestamp = 0;
	p->is_active = 1;
	LOG_INFO("V4L2 decoder initialized successfully");
	
	return true;
}
#endif  // USE_V4L2_DECODER

#ifdef USE_V4L2_DECODER
/**
 * Clean up V4L2 decoder resources
 * 
 * @param p Pointer to V4L2 player structure to clean up
 */
static void destroy_v4l2_decoder(v4l2_player_t *p, egl_ctx_t *e) {
	if (!p) return;
	
	// Clean up current DMA-BUF texture if it exists
	if (p->current_frame.is_dmabuf_texture && p->current_frame.texture != 0) {
		destroy_v4l2_dmabuf_texture(e, p->current_frame.texture, p->current_frame.egl_image);
		p->current_frame.texture = 0;
		p->current_frame.egl_image = EGL_NO_IMAGE_KHR;
		p->current_frame.is_dmabuf_texture = false;
	}
	
	if (p->buffer) {
		free(p->buffer);
		p->buffer = NULL;
	}

#ifdef USE_V4L2_DECODER
	if (p->use_demuxer && p->demuxer) {
		// v4l2_demuxer_destroy(p->demuxer); // Will implement later
	}
#endif
	
	if (p->input_file) {
		fclose(p->input_file);
		p->input_file = NULL;
	}
	
	if (p->decoder) {
		v4l2_decoder_stop(p->decoder);
		v4l2_decoder_destroy(p->decoder);
		free(p->decoder);
		p->decoder = NULL;
	}
	
	p->is_active = 0;
}
#endif  // USE_V4L2_DECODER

#ifdef USE_V4L2_DECODER
/**
 * Process one frame from the V4L2 decoder
 * 
 * @param p Pointer to V4L2 player structure
 * @return true if processing should continue, false to stop playback
 */
static bool process_v4l2_frame(v4l2_player_t *p, egl_ctx_t *e) {
	if (!p || !p->is_active) {
		return false;
	}
	
	// Check for stop signal immediately
	if (g_stop) {
		LOG_INFO("Stop signal received, exiting frame processing");
		return false;
	}
	
	// Log memory usage at start of frame processing
	static int frame_count = 0;
	frame_count++;
	if (frame_count % 1000 == 1) { // Log every 1000 frames to avoid spam
		log_memory_usage("V4L2-FRAME");
		log_system_memory();
	}
	
	// Frame rate limiting to prevent overwhelming the decoder
	struct timeval current_time;
	gettimeofday(&current_time, NULL);

#ifdef USE_V4L2_DECODER
	if (p->use_demuxer && 0) { // p->demuxer.fps > 0) {
		// Calculate target frame interval based on demuxer FPS (disabled)
		double target_interval_ms = 1000.0 / 30.0; // p->demuxer.fps;
		
		if (g_last_frame_time.tv_sec != 0) {
			// Calculate time since last frame
			double elapsed_ms = (double)(current_time.tv_sec - g_last_frame_time.tv_sec) * 1000.0 +
								(double)(current_time.tv_usec - g_last_frame_time.tv_usec) / 1000.0;
			
			// If we're processing too fast, add a small delay
			if (elapsed_ms < target_interval_ms) {
				double sleep_ms = target_interval_ms - elapsed_ms;
				if (sleep_ms > 0 && sleep_ms < 100) { // Cap to avoid excessive delays
					struct timespec sleep_time;
					sleep_time.tv_sec = 0;
					sleep_time.tv_nsec = (long)(sleep_ms * 1000000);
					nanosleep(&sleep_time, NULL);
					
					if (frame_count % 50 == 0) {
						LOG_DEBUG("Frame rate limiting: slept %.2f ms (target: %.2f ms)", sleep_ms, target_interval_ms);
					}
				}
			}
		}
		
		g_last_frame_time = current_time;
	} else {
	// Add minimum delay to prevent overwhelming the terminal/system
	struct timespec min_sleep = {0, 5000000}; // 5ms minimum (reduced for better responsiveness)
	nanosleep(&min_sleep, NULL);
	}
#endif

	// Check for stop signal periodically and flush output
	if (frame_count % 10 == 0) {
		if (g_stop) {
			LOG_INFO("Stop signal received, exiting V4L2 frame processing");
			return false;
		}
		// Flush stdout/stderr to keep terminal responsive
		fflush(stdout);
		fflush(stderr);
	}
	
	// Emergency timeout - if we've been running too long without user interaction, exit
	static struct timespec start_time = {0, 0};
	if (start_time.tv_sec == 0) {
		clock_gettime(CLOCK_MONOTONIC, &start_time);
	}
	struct timespec current_time_check;
	clock_gettime(CLOCK_MONOTONIC, &current_time_check);
	double elapsed_seconds = (double)(current_time_check.tv_sec - start_time.tv_sec);
	if (elapsed_seconds > 30.0) {  // Auto-exit after 30 seconds for testing
		LOG_INFO("Emergency timeout reached (%.1f seconds), exiting V4L2 frame processing", elapsed_seconds);
		return false;
	}
	
	// Track consecutive cycles with no frames to prevent infinite loops
	static int no_frame_cycles = 0;
	bool got_frame_this_cycle = false;
	
	// Feed data to the decoder if needed
#ifdef USE_V4L2_DECODER
	if (p->use_demuxer) {
		// Limit packets per frame to prevent buffer overrun
		static int packets_this_frame = 0;
		static int frame_number = 0;
		
		if (frame_count != frame_number) {
			// New frame, reset packet counter
			frame_number = frame_count;
			packets_this_frame = 0;
		}
		
		// Demuxer packet processing disabled for now - skip to fallback mode
		// (old demuxer code commented out)
		
		// Check for stop signal during packet processing
		if (g_stop) {
			LOG_INFO("Stop signal received during packet processing");
			return false;
		}
	} else {
#endif
		// Fallback: raw file reading for elementary streams
		if (p->input_file && !feof(p->input_file)) {
		// Use configurable chunk size for V4L2 decoder
		size_t max_chunk_size = 65536;  // Default 64KB - safe for most V4L2 drivers
		const char *env_chunk_size = getenv("PICKLE_V4L2_CHUNK_SIZE_KB");
		if (env_chunk_size) {
			int kb = atoi(env_chunk_size);
			if (kb > 0 && kb <= 1024) { // Reasonable bounds: 1KB-1MB
				max_chunk_size = (size_t)kb * 1024;
			}
		}
		
		static int chunk_log_counter = 0;
		if (++chunk_log_counter % 1000 == 1) {
			LOG_DEBUG("Using V4L2 chunk size: %zu bytes (logged every 1000 reads)", max_chunk_size);
		}
			// Read a chunk of data, but limit to safe chunk size
			size_t bytes_to_read = max_chunk_size < p->buffer_size ? max_chunk_size : p->buffer_size;
			size_t bytes_read = fread(p->buffer, 1, bytes_to_read, p->input_file);
			
			if (bytes_read > 0) {
				// Send to decoder
				if (!v4l2_decoder_decode(p->decoder, p->buffer, bytes_read, p->timestamp)) {
					LOG_ERROR("V4L2 decoder decode failed");
					log_memory_usage("V4L2-DECODE-FAIL");
				} else {
					static int send_log_counter = 0;
					if (++send_log_counter % 100 == 1) {
						LOG_DEBUG("Successfully sent %zu bytes to V4L2 decoder (logged every 100 sends)", bytes_read);
					}
				}
				// Increment timestamp based on detected video FPS (adaptive timing)
				double frame_interval_us = 1000000.0 / g_video_fps;
				int64_t interval_us = (int64_t)frame_interval_us;
				p->timestamp += interval_us;
			}
		}
#ifdef USE_V4L2_DECODER
	}
#endif
	
	// Remove redundant frame check - we'll check for frames in the poll section below
	// This prevents unnecessary buffer thrashing that can cause starvation
	
	// Poll for decoded frames with timeout and signal checking
	static int poll_log_counter = 0;
	if (++poll_log_counter % 200 == 1) {
		LOG_DEBUG("Polling V4L2 decoder for frames... (logged every 200 calls)");
	}
	int poll_result = v4l2_decoder_poll(p->decoder, 0);  // Non-blocking poll
	
	if (poll_result > 0) {
		// Process events
		v4l2_decoder_process_events(p->decoder);
		
		// Check for stop signal after processing events
		if (g_stop) {
			LOG_INFO("Stop signal received after processing events");
			return false;
		}
		
		// Try to get a decoded frame
		v4l2_decoded_frame_t decoded_frame;
		if (v4l2_decoder_get_frame(p->decoder, &decoded_frame)) {
			// Store frame information in the player struct
            p->current_frame.valid = true;
            p->current_frame.dmabuf_fd = decoded_frame.dmabuf_fd;
            p->current_frame.width = decoded_frame.width;
            p->current_frame.height = decoded_frame.height;
            p->current_frame.format = decoded_frame.format;
            p->current_frame.stride = p->decoder->stride;
            p->current_frame.buf_index = decoded_frame.buf_index;
            p->current_frame.egl_image = EGL_NO_IMAGE_KHR;
            p->current_frame.is_dmabuf_texture = false;
            
            LOG_INFO("Got frame: %dx%d timestamp: %ld, dmabuf_fd: %d", 
                     decoded_frame.width, decoded_frame.height, decoded_frame.timestamp, decoded_frame.dmabuf_fd);
            
            // Log memory usage when successfully getting frames
			static int successful_frame_count = 0;
			successful_frame_count++;
			if (successful_frame_count % 50 == 1) {
				log_memory_usage("V4L2-FRAME-SUCCESS");
			}
            
            // Create/update OpenGL texture with frame data
            if (decoded_frame.dmabuf_fd >= 0) {
                LOG_INFO("V4L2: Processing DMA-BUF frame (fd=%d)", decoded_frame.dmabuf_fd);
                
                // Create OpenGL texture directly from V4L2 DMA-BUF (zero-copy)
                GLuint dmabuf_texture = 0;
                EGLImageKHR dmabuf_image = EGL_NO_IMAGE_KHR;
                
                if (create_texture_from_v4l2_dmabuf(e, decoded_frame.dmabuf_fd,
                                                    decoded_frame.width, decoded_frame.height,
                                                    p->decoder->stride, decoded_frame.format,
                                                    &dmabuf_texture, &dmabuf_image)) {
                    // Successfully created zero-copy texture from DMA-BUF
                    static int zero_copy_success_counter = 0;
                    if (++zero_copy_success_counter % 100 == 1) {
                        LOG_INFO("V4L2: Created zero-copy DMA-BUF texture %u from fd %d (count: %d)",
                                 dmabuf_texture, decoded_frame.dmabuf_fd, zero_copy_success_counter);
                    }
                    
                    // Clean up previous texture if it was a DMA-BUF texture
                    if (p->current_frame.is_dmabuf_texture && p->current_frame.texture != 0) {
                        destroy_v4l2_dmabuf_texture(e, p->current_frame.texture, p->current_frame.egl_image);
                    }
                    
                    p->current_frame.texture = dmabuf_texture;
                    p->current_frame.egl_image = dmabuf_image;
                    p->current_frame.is_dmabuf_texture = true;
                } else {
                    LOG_ERROR("V4L2: Failed to create texture from DMA-BUF fd %d, falling back to memory copy", decoded_frame.dmabuf_fd);
                    // Fall back to memory-mapped data if available
                    if (decoded_frame.data) {
                        if (convert_nv12_to_rgb_texture_gpu(&decoded_frame, &p->texture)) {
                            p->current_frame.texture = p->texture;
                            p->current_frame.is_dmabuf_texture = false;
                        }
                    }
                }
            } else if (decoded_frame.data) {
                LOG_INFO("V4L2: Processing memory-mapped NV12 frame (data=%p, size=%u)", decoded_frame.data, decoded_frame.bytesused);
                
                // Convert NV12 to RGB texture using GPU
                if (convert_nv12_to_rgb_texture_gpu(&decoded_frame, &p->texture)) {
                    p->current_frame.texture = p->texture;
                    static int convert_log_counter = 0;
                    if (++convert_log_counter % 100 == 1) {
                        LOG_DEBUG("Successfully converted NV12 frame to RGB texture %u (logged every 100 conversions)", p->texture);
                    }
                } else {
                    LOG_ERROR("Failed to convert NV12 frame to RGB texture");
                }
            }
            
            // Return the buffer back to the decoder for reuse
            if (!v4l2_decoder_return_frame(p->decoder, &decoded_frame)) {
                LOG_ERROR("Failed to return frame buffer to decoder");
            }
            
			return true;
		} else {
			static int no_frame_log_counter = 0;
			if (++no_frame_log_counter % 500 == 1) {
				LOG_DEBUG("No decoded frame available from V4L2 decoder (logged every 500 calls)");
			}
		}
	}
	
	// Check if we've reached the end of input and decoder is empty
#ifdef USE_V4L2_DECODER
	bool input_finished = p->use_demuxer ? false : (p->input_file && feof(p->input_file)); // p->demuxer.eof_reached
#else
	bool input_finished = (p->input_file && feof(p->input_file));
#endif
	
	if (input_finished && p->decoder) {
		// Flush the decoder to get any remaining frames
        v4l2_decoder_flush(p->decoder);
	}
	
	// Track cycles with no frames to detect potential infinite loops
	if (!got_frame_this_cycle) {
		no_frame_cycles++;
		if (no_frame_cycles > 1000) {  // Exit if no frames for 1000 consecutive cycles
			LOG_WARN("No frames received for %d consecutive cycles, V4L2 decoder appears to have failed", no_frame_cycles);
			LOG_INFO("Requesting fallback to MPV decoder");
			extern int g_v4l2_fallback_requested;
			g_v4l2_fallback_requested = 1;
			return false;
		}
		if (no_frame_cycles % 200 == 0) {  // Log every 200 cycles (reduced frequency)
			LOG_DEBUG("No frames for %d consecutive cycles", no_frame_cycles);
		}
	}
	
	return true;
}
#endif  // USE_V4L2_DECODER

void drain_mpv_events(mpv_handle *h) {
	while (1) {
		mpv_event *ev = mpv_wait_event(h, 0);
		if (ev->event_id == MPV_EVENT_NONE) break;
		if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
			if (g_debug) fprintf(stderr, "[mpv] VIDEO_RECONFIG\n");
		}
		if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
			mpv_event_log_message *lm = ev->data;
			// Only print warnings/errors by default; set PICKLE_LOG_MPV for full detail earlier.
			if (lm->level && (strstr(lm->level, "error") || strstr(lm->level, "warn"))) {
				fprintf(stderr, "[mpv-log] %s: %s", lm->level, lm->text ? lm->text : "\n");
			}
			continue;
		}
		if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) {
			// This event can indicate that playback is resuming after a pause
			// Mark it as activity to prevent stall detection from triggering
			if (g_debug) fprintf(stderr, "[mpv] PLAYBACK_RESTART\n");
			gettimeofday(&g_last_frame_time, NULL);
		}
		if (ev->event_id == MPV_EVENT_END_FILE) {
			const mpv_event_end_file *ef = ev->data;
			fprintf(stderr, "End of file (reason=%d:%s)\n", ef->reason, mpv_end_reason_str(ef->reason));
			if (ef->error < 0) {
				const char *err = mpv_error_string(ef->error);
				fprintf(stderr, "[mpv] end-file error detail: %s (%d)\n", err, ef->error);
			}
			
			// Implement looping if enabled and normal EOF
			if (g_loop_playback && ef->reason == MPV_END_FILE_REASON_EOF) {
				// Reset playback position to beginning
				int64_t pos = 0;
				mpv_set_property(h, "time-pos", MPV_FORMAT_INT64, &pos);
				
				// Ensure playback is not paused
				int flag = 0;
				mpv_set_property(h, "pause", MPV_FORMAT_FLAG, &flag);
				
				// Reset stall detection counter when looping
				g_stall_reset_count = 0;
				
				// Update last frame time to avoid false stall detection during loop transition
				gettimeofday(&g_last_frame_time, NULL);
				
				// Force a frame update at loop points
				g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
				
				// Restart playback directly using a command
				const char *cmd[] = {"loadfile", mpv_get_property_string(h, "path"), "replace", NULL};
				mpv_command_async(h, 0, cmd);
				
				fprintf(stderr, "Looping playback (restarting file)...\n");
				// Continue event processing, don't set g_stop flag
			} else {
				// Normal end-of-file behavior: exit
				g_stop = 1;
			}
		}
	}
}

// Note: Removed earlier experimental render_frame() that used a non-standard C++ lambda.
// The fixed implementation below (render_frame_fixed) is the one actually used.

// Because we attempted a C++-style lambda above (invalid in C), we need a proper handler
// but we also must know the gbm_bo to release. We'll store a static reference to egl_ctx
// and pass the bo as user data.
static struct egl_ctx *g_egl_for_handler;
// Forward state needed by page flip handler (must appear before handler definition)
static struct gbm_bo *g_first_frame_bo = NULL; // BO used for initial modeset, released after second frame
int g_pending_flip = 0; // set after scheduling page flip until event handler fires
void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data) {
	(void)fd; (void)frame;
	struct gbm_bo *old = data;
	if (g_debug) fprintf(stderr, "[DRM] Page flip completed, pending_flip was %d\n", g_pending_flip);
	if (g_egl_for_handler && old) gbm_surface_release_buffer(g_egl_for_handler->gbm_surf, old);
	g_pending_flip = 0; // flip completed
	g_pending_flips--; // decrement pending flip count
	
	if (g_first_frame_bo && g_first_frame_bo != old) {
		gbm_surface_release_buffer(g_egl_for_handler->gbm_surf, g_first_frame_bo);
		g_first_frame_bo = NULL;
	}
	
	// Update last frame time on successful page flip
	struct timeval now; gettimeofday(&now, NULL);
	g_last_frame_time = now;
	g_last_flip_complete = now;
	
	// Update flip timing metrics
	if (g_frame_timing_enabled) {
		double flip_time = tv_diff(&now, &g_last_flip_submit);
		if (flip_time < g_min_flip_time) g_min_flip_time = flip_time;
		if (flip_time > g_max_flip_time) g_max_flip_time = flip_time;
		g_avg_flip_time = (g_avg_flip_time * g_flip_count + flip_time) / (g_flip_count + 1);
		g_flip_count++;
		
		if (g_debug && (g_flip_count % 60 == 0)) {
			fprintf(stderr, "[timing] flip min=%.2fms avg=%.2fms max=%.2fms count=%d\n",
				g_min_flip_time * 1000.0, g_avg_flip_time * 1000.0, g_max_flip_time * 1000.0, g_flip_count);
		}
	}
}

/**
 * Initialize keystone correction with default values (no correction)
 */
/**
 * Load keystone configuration from a specified file path
 * 
 * @param path The file path to load the configuration from
 * @return true if the configuration was loaded successfully, false otherwise
 */
/* Moved to keystone.c module */
/*
static bool keystone_load_config(const char* path) {
    ...
}
*/

/* Moved to keystone.c module */
/*
static void keystone_init(void) {
    ...
}
*/

/**
 * Calculate the perspective transformation matrix based on the corner points
 * Updates the vertex coordinates used for rendering with the shader
 */
/* Moved to keystone.c module */
/*
static void keystone_update_matrix(void) {
    ...
}
*/

/**
 * Adjust a specific corner of the keystone correction
 * 
 * @param corner Corner index (0-3)
 * @param x_delta X adjustment (positive = right)
 * @param y_delta Y adjustment (positive = down)
 */
// Adjust a mesh point position
/* Moved to keystone.c module */
/*
static void keystone_adjust_mesh_point(int row, int col, float x_delta, float y_delta) {
    ...
}
*/

// Toggle pinning status of a corner
/* Moved to keystone.c module */
/*
static void keystone_toggle_pin(int corner) {
    ...
}
*/

/* Moved to keystone.c module */
/*
static void keystone_adjust_corner(int corner, float x_delta, float y_delta) {
    ...
}
*/

// Forward declaration for border shader initialization
static bool init_border_shader();
static bool init_nv12_shader();

static bool init_border_shader() {
    g_border_vertex_shader = compile_shader(GL_VERTEX_SHADER, g_border_vs_src);
    if (!g_border_vertex_shader) return false;
    g_border_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, g_border_fs_src);
    if (!g_border_fragment_shader) { glDeleteShader(g_border_vertex_shader); g_border_vertex_shader = 0; return false; }
    g_border_shader_program = glCreateProgram();
    if (!g_border_shader_program) { glDeleteShader(g_border_vertex_shader); glDeleteShader(g_border_fragment_shader); return false; }
    glAttachShader(g_border_shader_program, g_border_vertex_shader);
    glAttachShader(g_border_shader_program, g_border_fragment_shader);
    glLinkProgram(g_border_shader_program);
    GLint linked = 0; glGetProgramiv(g_border_shader_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len=0; glGetProgramiv(g_border_shader_program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len>1) { char* buf = malloc((size_t)info_len); glGetProgramInfoLog(g_border_shader_program, info_len, NULL, buf); LOG_ERROR("Border shader link: %s", buf); free(buf);}        
        glDeleteProgram(g_border_shader_program); g_border_shader_program=0;
        glDeleteShader(g_border_vertex_shader); g_border_vertex_shader=0;
        glDeleteShader(g_border_fragment_shader); g_border_fragment_shader=0;
        return false;
    }
    g_border_a_position_loc = glGetAttribLocation(g_border_shader_program, "a_position");
    g_border_u_color_loc = glGetUniformLocation(g_border_shader_program, "u_color");
    return true;
}

// Initialize NV12 to RGB conversion shader
static bool __attribute__((unused)) init_nv12_shader() {
    g_nv12_vertex_shader = compile_shader(GL_VERTEX_SHADER, g_nv12_vs_src);
    if (!g_nv12_vertex_shader) return false;
    
    g_nv12_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, g_nv12_fs_src);
    if (!g_nv12_fragment_shader) {
        glDeleteShader(g_nv12_vertex_shader);
        g_nv12_vertex_shader = 0;
        return false;
    }
    
    g_nv12_shader_program = glCreateProgram();
    if (!g_nv12_shader_program) {
        glDeleteShader(g_nv12_vertex_shader);
        glDeleteShader(g_nv12_fragment_shader);
        g_nv12_vertex_shader = 0;
        g_nv12_fragment_shader = 0;
        return false;
    }
    
    glAttachShader(g_nv12_shader_program, g_nv12_vertex_shader);
    glAttachShader(g_nv12_shader_program, g_nv12_fragment_shader);
    glLinkProgram(g_nv12_shader_program);
    
    GLint linked = 0;
    glGetProgramiv(g_nv12_shader_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(g_nv12_shader_program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            char* buf = malloc((size_t)info_len);
            glGetProgramInfoLog(g_nv12_shader_program, info_len, NULL, buf);
            LOG_ERROR("NV12 shader link: %s", buf);
            free(buf);
        }
        glDeleteProgram(g_nv12_shader_program);
        g_nv12_shader_program = 0;
        glDeleteShader(g_nv12_vertex_shader);
        g_nv12_vertex_shader = 0;
        glDeleteShader(g_nv12_fragment_shader);
        g_nv12_fragment_shader = 0;
        return false;
    }
    
    // Get attribute and uniform locations
    g_nv12_a_position_loc = glGetAttribLocation(g_nv12_shader_program, "a_position");
    g_nv12_a_tex_coord_loc = glGetAttribLocation(g_nv12_shader_program, "a_tex_coord");
    g_nv12_u_texture_y_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_y");
    g_nv12_u_texture_uv_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_uv");
    
    // Create VAO and VBO for fullscreen quad
    glGenVertexArrays(1, &g_nv12_vao);
    glGenBuffers(1, &g_nv12_vbo);
    
    // Setup fullscreen quad vertices (position + texture coordinates)
    float quad_vertices[] = {
        // Position   // TexCoord
        -1.0f, -1.0f, 0.0f, 1.0f,  // Bottom-left
         1.0f, -1.0f, 1.0f, 1.0f,  // Bottom-right
        -1.0f,  1.0f, 0.0f, 0.0f,  // Top-left
         1.0f,  1.0f, 1.0f, 0.0f,  // Top-right
    };
    
    glBindVertexArray(g_nv12_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_nv12_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    
    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Texture coordinate attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
    
    return true;
}

// GPU-based NV12 to RGB conversion using shaders (high performance)
static bool __attribute__((unused)) convert_nv12_to_rgb_texture_gpu(const v4l2_decoded_frame_t *frame, GLuint *rgb_texture) {
    if (!frame || !frame->data || !rgb_texture) return false;
    
    // Initialize NV12 shader if needed
    if (g_nv12_shader_program == 0) {
        if (!init_nv12_shader()) {
            LOG_ERROR("Failed to initialize NV12 shader, falling back to CPU conversion");
            return false;  // No CPU fallback available in this context
        }
        LOG_INFO("GPU-based NV12 conversion initialized");
    }
    
    // Create Y and UV textures if needed
    static GLuint y_texture = 0, uv_texture = 0;
    static GLuint rgb_fbo = 0;
    static uint32_t prev_width = 0, prev_height = 0;
    
    if (y_texture == 0 || prev_width != frame->width || prev_height != frame->height) {
        // Clean up old textures if size changed
        if (y_texture) {
            glDeleteTextures(1, &y_texture);
            glDeleteTextures(1, &uv_texture);
            glDeleteFramebuffers(1, &rgb_fbo);
        }
        
        // Create Y plane texture (full resolution, single channel)
        glGenTextures(1, &y_texture);
        glBindTexture(GL_TEXTURE_2D, y_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        // Create UV plane texture (half resolution, two channel)
        glGenTextures(1, &uv_texture);
        glBindTexture(GL_TEXTURE_2D, uv_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        // Create output RGB texture
        if (*rgb_texture == 0) {
            glGenTextures(1, rgb_texture);
        }
        glBindTexture(GL_TEXTURE_2D, *rgb_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)frame->width, (GLsizei)frame->height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        
        // Create framebuffer for rendering to RGB texture
        glGenFramebuffers(1, &rgb_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, rgb_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *rgb_texture, 0);
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG_ERROR("Failed to create framebuffer for NV12 conversion");
            return false;
        }
        
        prev_width = frame->width;
        prev_height = frame->height;
    }
    
    // Upload Y plane data (full resolution)
    const unsigned char *y_data = (const unsigned char *)frame->data;
    glBindTexture(GL_TEXTURE_2D, y_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, (GLsizei)frame->width, (GLsizei)frame->height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, y_data);
    
    // Upload UV plane data (half resolution interleaved)
    const unsigned char *uv_data = y_data + (frame->width * frame->height);
    glBindTexture(GL_TEXTURE_2D, uv_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, (GLsizei)(frame->width / 2U), (GLsizei)(frame->height / 2U), 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv_data);
    
    // Set up render state for conversion
    glBindFramebuffer(GL_FRAMEBUFFER, rgb_fbo);
    glViewport(0, 0, (GLsizei)frame->width, (GLsizei)frame->height);
    
    glUseProgram(g_nv12_shader_program);
    
    // Bind Y and UV textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_texture);
    glUniform1i(g_nv12_u_texture_y_loc, 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uv_texture);
    glUniform1i(g_nv12_u_texture_uv_loc, 1);
    
    // Render fullscreen quad to perform conversion
    glBindVertexArray(g_nv12_vao);
    glBindAttribLocation(g_nv12_shader_program, 0, "a_position");
    glBindAttribLocation(g_nv12_shader_program, 1, "a_tex_coord");
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    // Restore default framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        LOG_ERROR("OpenGL error in GPU NV12 conversion: 0x%x", gl_error);
        return false;
    }
    
    return true;
}

// Initialize keystone shader program
/* Moved to keystone.c module */
/*
static bool init_keystone_shader(void) {
    ...
}
*/

// Free allocated mesh resources
/* Moved to keystone.c module */
/*
static void cleanup_mesh_resources(void) {
    ...
}
*/

// Cleanup keystone shader resources
/* Moved to keystone.c module */
/*
static void cleanup_keystone_shader(void) {
    ...
}
*/

/**
 * Process keystone adjustment key commands
 * 
 * @param key The key character pressed
 * @return true if the key was handled, false otherwise
 */
// Arrow key escape sequences
#define ESC_CHAR 27
#define ARROW_UP 'A'
#define ARROW_DOWN 'B'
#define ARROW_RIGHT 'C'
#define ARROW_LEFT  'D'

// Key sequence state for handling escape sequences
static struct {
    bool in_escape_seq;
    bool in_bracket_seq;
    char last_char;
} g_key_seq_state = {false, false, 0};

/* Moved to keystone.c module */
/*
static bool keystone_handle_key(char key) {
    ...
}
*/

/**
 * Save keystone configuration to a specified file path
 * 
 * @param path The file path to save the configuration to
 * @return true if the configuration was saved successfully, false otherwise
 */
/* Moved to keystone.c module */
/*
static bool keystone_save_config(const char* path) {
    ...
}
*/

/**
 * Initialize joystick/gamepad support
 * Attempts to open the first joystick device and set up event handling
 *
 * @return true if a joystick was found and initialized
 */
/* init_joystick function has been moved to input.c */

/**
 * Clean up joystick resources
 */
/* cleanup_joystick function has been moved to input.c */

// We now use the implementation of handle_joystick_event from input.c// Removed const from drm_ctx parameter because drmModeSetCrtc expects a non-const drmModeModeInfoPtr.
// We cache framebuffer IDs per gbm_bo to avoid per-frame AddFB/RmFB churn.
// user data holds a small struct with fb id + drm fd and a destroy handler.
int g_scanout_disabled = 0; // when set, we skip page flips/modeset and just let mpv decode & render offscreen
struct fb_holder { uint32_t fb; int fd; };
void bo_destroy_handler(struct gbm_bo *bo, void *data) __attribute__((weak));
void bo_destroy_handler(struct gbm_bo *bo, void *data) {
	(void)bo;
	struct fb_holder *h = data;
	if (h) {
		if (h->fb) drmModeRmFB(h->fd, h->fb);
		free(h);
	}
}

/**
 * Present GBM surface buffer to screen with page flip
 * Called after eglSwapBuffers() to actually display the frame
 * Handles both initial modeset and subsequent page flips
 */
bool present_gbm_surface(kms_ctx_t *d, egl_ctx_t *e) {
	// Get front buffer after eglSwapBuffers
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(e->gbm_surf);
	if (!bo) {
		fprintf(stderr, "gbm_surface_lock_front_buffer failed\n");
		return false;
	}
	
	// Get or create framebuffer ID
	struct fb_holder *h = gbm_bo_get_user_data(bo);
	uint32_t fb_id = h ? h->fb : 0;
	
	if (!fb_id) {
		uint32_t handle = gbm_bo_get_handle(bo).u32;
		uint32_t pitch = gbm_bo_get_stride(bo);
		uint32_t width = gbm_bo_get_width(bo);
		uint32_t height = gbm_bo_get_height(bo);
		
		if (!g_scanout_disabled && drmModeAddFB(d->fd, width, height, 24, 32, pitch, handle, &fb_id)) {
			fprintf(stderr, "drmModeAddFB failed (w=%u h=%u pitch=%u handle=%u err=%s)\n", width, height, pitch, handle, strerror(errno));
			gbm_surface_release_buffer(e->gbm_surf, bo);
			return false;
		}
		
		struct fb_holder *nh = calloc(1, sizeof(*nh));
		if (!nh) {
			fprintf(stderr, "Out of memory allocating fb_holder\n");
			gbm_surface_release_buffer(e->gbm_surf, bo);
			return false;
		}
		nh->fb = fb_id;
		nh->fd = d->fd;
		gbm_bo_set_user_data(bo, nh, bo_destroy_handler);
	}
	
	// Check if this is the first frame
	static bool first_frame = true;
	
	if (!g_scanout_disabled && first_frame) {
		// Initial modeset
		bool success = false;
		
		if (d->atomic_supported) {
			success = atomic_present_framebuffer(d, fb_id, g_vsync_enabled);
		} else {
			success = (drmModeSetCrtc(d->fd, d->crtc_id, fb_id, 0, 0, &d->connector_id, 1, &d->mode) == 0);
		}
		
		if (!success) {
			fprintf(stderr, "[ERROR] Initial modeset failed: %s\n", strerror(errno));
			gbm_surface_release_buffer(e->gbm_surf, bo);
			return false;
		}
		
		first_frame = false;
		g_first_frame_bo = bo;  // Retain this BO until first page flip completes
		return true;  // Do not release now - buffer needed for scanout
	} else if (!g_scanout_disabled) {
		// Subsequent page flips
		g_egl_for_handler = e;  // Set for page flip handler
		
		// Wait for pending flip if needed
		int max_pending = g_triple_buffer ? 2 : 1;
		
		if (g_pending_flips >= max_pending) {
			// Wait for a pending flip to complete
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(d->fd, &fds);
			
			struct timeval timeout = {0, 100000}; // 100ms
			
			if (select(d->fd + 1, &fds, NULL, NULL, &timeout) <= 0) {
				// Timeout - force reset
				if (g_debug) fprintf(stderr, "[buffer] Page flip wait timeout, resetting state\n");
				g_pending_flip = 0;
			} else if (FD_ISSET(d->fd, &fds)) {
				// Handle the page flip event
				drmEventContext ev = {
					.version = DRM_EVENT_CONTEXT_VERSION,
					.page_flip_handler = page_flip_handler
				};
				drmHandleEvent(d->fd, &ev);
			}
		}
		
		// Perform page flip
		if (d->atomic_supported) {
			if (!atomic_present_framebuffer(d, fb_id, g_vsync_enabled)) {
				gbm_surface_release_buffer(e->gbm_surf, bo);
				return false;
			}
		} else {
			if (drmModePageFlip(d->fd, d->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, bo)) {
				if (g_debug) fprintf(stderr, "[ERROR] drmModePageFlip failed: %s\n", strerror(errno));
				gbm_surface_release_buffer(e->gbm_surf, bo);
				return false;
			}
		}
		
		if (g_debug) fprintf(stderr, "[DRM] Setting g_pending_flip = 1 (location 1)\n");
		g_pending_flip = 1;
		g_pending_flips++;
	} else {
		// Offscreen mode - just release buffer
		gbm_surface_release_buffer(e->gbm_surf, bo);
	}
	
	return true;
}

#ifdef USE_V4L2_DECODER

/**
 * Render texture directly to screen without keystone correction
 */
static void render_direct_quad(GLuint texture) {
    if (texture == 0) return;
    
    // Simple direct texture rendering to fullscreen quad
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    // Disable depth testing and enable blending for video
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Define fullscreen quad vertices (clip space coordinates)
    float vertices[] = {
        -1.0f, -1.0f,  // Bottom left
         1.0f, -1.0f,  // Bottom right  
        -1.0f,  1.0f,  // Top left
         1.0f,  1.0f   // Top right
    };
    
    // Define texture coordinates
    float texcoords[] = {
        0.0f, 1.0f,  // Bottom left (flipped Y)
        1.0f, 1.0f,  // Bottom right (flipped Y)
        0.0f, 0.0f,  // Top left (flipped Y)
        1.0f, 0.0f   // Top right (flipped Y)
    };
    
    // Use basic shader program (assume it's already set up)
    // Render as triangle strip: 4 vertices forming 2 triangles
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
}

/**
 * Render texture with keystone correction
 */
static void render_keystone_quad(GLuint texture) {
    if (texture == 0) return;
    
    // First copy the texture to the keystone FBO, then render with keystone correction
    // This integrates with the existing keystone correction pipeline
    
    // Set the texture as the keystone FBO texture (this is a simplification)
    // In a full implementation, we'd copy the V4L2 texture to the keystone FBO first
    g_keystone_fbo_texture = texture;
    
    // Now render using the existing keystone pipeline
    // The keystone rendering code will use g_keystone_fbo_texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    LOG_DEBUG("V4L2 texture set for keystone rendering: %u", texture);
}

/**
 * Render a frame using the V4L2 decoder
 * 
 * @param d Pointer to KMS context
 * @param e Pointer to EGL context
 * @param p Pointer to V4L2 player structure
 * @return true if rendering succeeded, false otherwise
 */
bool render_v4l2_frame(kms_ctx_t *d, egl_ctx_t *e, v4l2_player_t *p) {
	// Start stats timing (was missing for V4L2 path)
	stats_overlay_render_frame_start(&g_stats_overlay);
	
	if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
		fprintf(stderr, "eglMakeCurrent failed\n"); return false; 
	}
	
	// Check if we need to clear the display (when new file is loaded)
	if (g_clear_display) {
		clear_display_buffer(d, e);
		g_clear_display = 0; // Reset the flag
	}
	
	// Initialize keystone shader if needed and enabled
	if (g_keystone.enabled && g_keystone_shader_program == 0) {
		if (!init_keystone_shader()) {
			LOG_ERROR("Failed to initialize keystone shader, disabling keystone correction");
			g_keystone.enabled = false;
		}
	}
	// Initialize border shader lazily if needed for border rendering
	if (g_show_border && g_border_shader_program == 0) {
		if (!init_border_shader()) {
			LOG_WARN("Failed to initialize border shader; border will be disabled");
			g_show_border = false;
		}
	}
	
	// Background is always black
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	
	// Process a frame from the V4L2 decoder
	if (!process_v4l2_frame(p, e)) {
        // No new frame available - this is normal, just return success
        // so the main loop continues with proper timeout handling
        return true;
    }
    
    // Check for active frame
    if (!p->current_frame.valid) {
        return true; // Continue normally, frames will come later
    }
    
    // Create/update OpenGL texture from V4L2 decoded frame
    if (p->current_frame.dmabuf_fd >= 0) {
        // We have a DMA-BUF frame, use zero-copy path if possible
        if (!g_scanout_disabled && should_use_zero_copy(d, e)) {
            // Setup source and destination rectangles
            float src_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // Full texture
            float dst_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // Full screen
            
            // Use zero-copy path to present the frame
            if (present_frame_zero_copy(d, e, p->current_frame.texture, src_rect, dst_rect)) {
                // Successfully presented using zero-copy path
                static bool first_frame = true;
                if (g_debug && first_frame) {
                    fprintf(stderr, "[debug] Using zero-copy DMA-BUF path with %s modesetting\n", 
                            d->atomic_supported ? "atomic" : "legacy");
                    first_frame = false;
                }
                return true;
            }
        }
    }
	
	// Debug: Check if we have a valid frame and texture
	if (p->current_frame.valid) {
		if (g_debug) {
			fprintf(stderr, "[DEBUG] V4L2: Valid frame available, texture=%u\n", p->texture);
		}
		if (p->texture > 0) {
			// Render the V4L2 texture to screen using the appropriate pipeline
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, p->texture);
			if (g_debug) {
				fprintf(stderr, "[DEBUG] V4L2: Texture %u bound for rendering\n", p->texture);
			}
			
			// Use keystone correction if enabled, otherwise render directly
			if (g_keystone.enabled && g_keystone_shader_program > 0) {
				render_keystone_quad(p->texture);
				LOG_DEBUG("V4L2 frame rendered with keystone correction");
			} else {
				render_direct_quad(p->texture);
				LOG_DEBUG("V4L2 frame rendered directly");
			}
		}
	} else {
		if (g_debug) {
			fprintf(stderr, "[DEBUG] V4L2: No valid frame available\n");
		}
	}
	
	// Swap buffers
	if (!eglSwapBuffers(e->dpy, e->surf)) {
		int err = eglGetError();
		fprintf(stderr, "eglSwapBuffers failed (0x%x)\n", err); 
		return false;
	}
	
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(e->gbm_surf);
	if (!bo) {
		fprintf(stderr, "gbm_surface_lock_front_buffer failed\n");
		return false;
	}
	
	// Get framebuffer ID associated with the buffer object
	uint32_t fb_id = 0;
	for (int i = 0; i < g_fb_ring.count; i++) {
		if (g_fb_ring.entries[i].bo == bo) {
			fb_id = g_fb_ring.entries[i].fb_id;
			break;
		}
	}
	
	if (fb_id == 0) {
		if (g_debug) {
			fprintf(stderr, "[DEBUG] Failed to find framebuffer for BO (ring count=%d)\n", g_fb_ring.count);
		}
		gbm_surface_release_buffer(e->gbm_surf, bo);
		return false;
	}
	
	if (g_debug) {
		fprintf(stderr, "[DEBUG] Using framebuffer ID %u for page flip\n", fb_id);
	}
	
	// Present the framebuffer using KMS
	bool ret = true;
	if (d->atomic_supported) {
		ret = atomic_present_framebuffer(d, fb_id, g_vsync_enabled);
	} else {
		int flip_ret = drmModePageFlip(d->fd, d->crtc_id, fb_id, g_vsync_enabled ? DRM_MODE_PAGE_FLIP_EVENT : 0, d);
		if (flip_ret != 0) {
			fprintf(stderr, "[ERROR] drmModePageFlip failed: %d (%s)\n", flip_ret, strerror(errno));
			ret = false;
		}
	}
	
	// Wait for page flip to complete if using vsync
	if (g_vsync_enabled) {
		wait_for_flip(d->fd);
	}
	
	// Release the buffer
	gbm_surface_release_buffer(e->gbm_surf, bo);
	
	// End stats timing (was missing for V4L2 path)
	stats_overlay_render_frame_end(&g_stats_overlay);
	
	return ret;
}
#endif  // USE_V4L2_DECODER

/**
 * Clear the display to black and force a buffer swap
 * Used when loading new videos to prevent old content from showing
 *
 * @param d Pointer to initialized DRM context 
 * @param e Pointer to initialized EGL context
 */
static void clear_display_buffer(kms_ctx_t *d, egl_ctx_t *e) {
	if (!d || !e) return;
	
	// Make sure EGL context is current
	if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
		return; // Can't clear if context setup fails
	}
	
	// Clear to black
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	
	// Force buffer swap to make the clear visible
	if (eglSwapBuffers(e->dpy, e->surf)) {
		struct gbm_bo *bo = gbm_surface_lock_front_buffer(e->gbm_surf);
		if (bo) {
			struct fb_holder *h = gbm_bo_get_user_data(bo);
			uint32_t fb_id = h ? h->fb : 0;
			if (fb_id) {
				// Present the cleared framebuffer
				if (d->atomic_supported) {
					atomic_present_framebuffer(d, fb_id, false); // No vsync for clearing
				} else {
					drmModePageFlip(d->fd, d->crtc_id, fb_id, 0, d);
				}
			}
			gbm_surface_release_buffer(e->gbm_surf, bo);
		}
	}
}

static bool render_frame_fixed(kms_ctx_t *d, egl_ctx_t *e, mpv_player_t *p) {
	// Start stats timing
	stats_overlay_render_frame_start(&g_stats_overlay);
	
	if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
		fprintf(stderr, "eglMakeCurrent failed\n"); return false; 
	}
	
	// Check if we need to clear the display (when new file is loaded)
	if (g_clear_display) {
		clear_display_buffer(d, e);
		g_clear_display = 0; // Reset the flag
	}
	
	// Check if we should use hardware-accelerated DRM keystone instead of software keystone
	// Force use of OpenGL ES keystone instead of DRM keystone (which has framebuffer issues)
	bool use_drm_keystone = false; // Disabled until framebuffer integration is fixed
	
	// Initialize keystone shader if needed and enabled (only for software keystone)
	// Skip keystone in performance mode if configured to do so
	bool skip_keystone = should_skip_feature_for_performance("keystone");
	if (g_keystone.enabled && !skip_keystone && !use_drm_keystone && g_keystone_shader_program == 0) {
		if (!init_keystone_shader()) {
			LOG_ERROR("Failed to initialize keystone shader, disabling keystone correction");
			g_keystone.enabled = false;
		}
	}
	// Initialize border shader lazily if needed for border rendering
	if (g_show_border && g_border_shader_program == 0) {
		if (!init_border_shader()) {
			LOG_WARN("Failed to initialize border shader; border will be disabled");
			g_show_border = false;
		}
	}
	
	// Background is always black
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	
	// Ensure reusable FBO exists when software keystone is enabled, sized to current mode
	// Skip FBO management if keystone is disabled for performance
	if (g_keystone.enabled && !skip_keystone && !use_drm_keystone) {
		int want_w = (int)d->mode.hdisplay;
		int want_h = (int)d->mode.vdisplay;
		bool need_recreate = (g_keystone_fbo == 0) || (g_keystone_fbo_w != want_w) || (g_keystone_fbo_h != want_h);
		if (need_recreate) {
			// Destroy previous if any
			if (g_keystone_fbo) {
				glDeleteFramebuffers(1, &g_keystone_fbo);
				g_keystone_fbo = 0;
			}
			if (g_keystone_fbo_texture) {
				glDeleteTextures(1, &g_keystone_fbo_texture);
				g_keystone_fbo_texture = 0;
			}
			// Create texture with proper configuration for video
			glGenTextures(1, &g_keystone_fbo_texture);
			glBindTexture(GL_TEXTURE_2D, g_keystone_fbo_texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			
			// Use RGBA format with proper alpha handling
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, want_w, want_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			
			// Set proper blend mode to ensure texture can be displayed
			glBindTexture(GL_TEXTURE_2D, 0);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			
			// Check for errors in texture creation
			GLenum tex_error = glGetError();
			if (tex_error != GL_NO_ERROR) {
				LOG_ERROR("Failed to create keystone texture: GL error %d", tex_error);
				glBindTexture(GL_TEXTURE_2D, 0);
				glDeleteTextures(1, &g_keystone_fbo_texture);
				g_keystone_fbo_texture = 0;
				return false;
			}
			// Create FBO
			glGenFramebuffers(1, &g_keystone_fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, g_keystone_fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_keystone_fbo_texture, 0);
			
			// Check for errors during FBO configuration
			GLenum fbo_error = glGetError();
			if (fbo_error != GL_NO_ERROR) {
				LOG_ERROR("Error configuring FBO: GL error %d", fbo_error);
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glDeleteFramebuffers(1, &g_keystone_fbo);
				glDeleteTextures(1, &g_keystone_fbo_texture);
				g_keystone_fbo = 0;
				g_keystone_fbo_texture = 0;
			} else {
				g_keystone_fbo_w = want_w;
				g_keystone_fbo_h = want_h;
			}
		}
	}
	
	// Render MPV frame either to our FBO or directly to screen
	mpv_opengl_fbo mpv_fbo;
	int mpv_flip_y = 0; // default: no flip (handled in final pass if needed)
	if (g_keystone.enabled && !use_drm_keystone && g_keystone_fbo) {
		// Clear the FBO first to ensure proper rendering
		glBindFramebuffer(GL_FRAMEBUFFER, g_keystone_fbo);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Clear with opaque black
		glClear(GL_COLOR_BUFFER_BIT);
		
		// Force default viewport settings for the FBO rendering (cached)
		gl_viewport_cached(0, 0, g_keystone_fbo_w, g_keystone_fbo_h);
		
		mpv_fbo = (mpv_opengl_fbo){ .fbo = (int)g_keystone_fbo, .w = g_keystone_fbo_w, .h = g_keystone_fbo_h, .internal_format = 0 };
	} else {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		mpv_fbo = (mpv_opengl_fbo){ .fbo = 0, .w = (int)d->mode.hdisplay, .h = (int)d->mode.vdisplay, .internal_format = 0 };
		// When rendering directly to the default framebuffer (no keystone), mpv should flip vertically
		mpv_flip_y = 1;
	}
	
	mpv_render_param r_params[] = {
		(mpv_render_param){MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
		(mpv_render_param){MPV_RENDER_PARAM_FLIP_Y, &mpv_flip_y},
		(mpv_render_param){0}
	};
	
	if (!p->rctx) {
		fprintf(stderr, "mpv render context NULL\n");
		return false;
	}
	
	// Ensure proper OpenGL state before rendering
	if (g_keystone.enabled && !use_drm_keystone) {
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	
	// Render the mpv frame
	if (g_keystone.enabled && !use_drm_keystone && g_keystone_fbo) {
		// First let MPV render to its own texture via render_frame_mpv
		bool mpv_render_ok = render_frame_mpv(p->mpv, p->rctx, d, e);
		if (!mpv_render_ok) {
			if (g_debug) fprintf(stderr, "[DEBUG] Keystone: MPV render failed\n");
			return false;
		}
		
		// Get the MPV texture
		GLuint mpv_texture = get_mpv_texture();
		if (mpv_texture == 0) {
			if (g_debug) fprintf(stderr, "[DEBUG] Keystone: No MPV texture available\n");
			return false;
		}
		
		// Bind keystone FBO for rendering
		glBindFramebuffer(GL_FRAMEBUFFER, g_keystone_fbo);
		
		// Copy MPV texture to keystone FBO using a simple blit
		glBindFramebuffer(GL_FRAMEBUFFER, g_keystone_fbo);
		glViewport(0, 0, g_keystone_fbo_w, g_keystone_fbo_h);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		
		// Simple full-screen blit of MPV texture to keystone FBO
		// This uses the existing video rendering infrastructure
		float src_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // Full MPV texture
		float dst_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // Full keystone FBO
		
		// Render MPV texture to keystone FBO
		bool blit_ok = render_video_frame(e, mpv_texture, src_rect, dst_rect);
		if (!blit_ok && g_debug) {
			fprintf(stderr, "[DEBUG] Keystone: Failed to copy MPV texture %u to FBO\n", mpv_texture);
		}
		
		// Bind framebuffer back to default after copy
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		
		// Verification complete - FBO ready for keystone rendering
	} else {
		// Use our video pipeline for non-keystone rendering
		bool mpv_render_ok = render_frame_mpv(p->mpv, p->rctx, d, e);
		if (!mpv_render_ok) {
			// Fallback to direct rendering if our pipeline fails
			mpv_render_context_render(p->rctx, r_params);
		}
	}
	
	// If software keystone is enabled, render the FBO texture with our shader
	if (g_keystone.enabled && !use_drm_keystone && g_keystone_fbo && g_keystone_fbo_texture) {
		// Render keystone corrected video using shaders
		
		// Switch back to default framebuffer
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		
		// Reset viewport to match screen size (cached)
		gl_viewport_cached(0, 0, (int)d->mode.hdisplay, (int)d->mode.vdisplay);
		
		// Use our shader program (cached)
		gl_use_program_cached(g_keystone_shader_program);
		
		// Check for shader attribute locations before using them
		if (g_keystone_a_position_loc < 0 || g_keystone_a_texcoord_loc < 0 || g_keystone_u_texture_loc < 0) {
			// Re-acquire attribute locations
			g_keystone_a_position_loc = glGetAttribLocation(g_keystone_shader_program, "a_position");
			g_keystone_a_texcoord_loc = glGetAttribLocation(g_keystone_shader_program, "a_texCoord");
			g_keystone_u_texture_loc = glGetUniformLocation(g_keystone_shader_program, "u_texture");
			
			if (g_debug) {
				fprintf(stderr, "DEBUG: Reacquired shader attributes: pos=%d tex=%d u_tex=%d\n", 
					g_keystone_a_position_loc, g_keystone_a_texcoord_loc, g_keystone_u_texture_loc);
			}
				
			if (g_keystone_a_position_loc < 0 || g_keystone_a_texcoord_loc < 0 || g_keystone_u_texture_loc < 0) {
				fprintf(stderr, "ERROR: Failed to get shader attributes\n");
				return false; // Can't continue with invalid attributes
			}
		}
		
		// Set up texture (cached)
		glActiveTexture(GL_TEXTURE0);
		gl_bind_texture_cached(GL_TEXTURE_2D, g_keystone_fbo_texture);
		glUniform1i(g_keystone_u_texture_loc, 0);
		
		// Always enable blending for proper rendering regardless of border state (cached)
		gl_enable_blend_cached(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		
		// Ensure alpha blending is properly handled for video content
		glDisable(GL_DEPTH_TEST);
		
		// Clear any previous OpenGL errors
		while (glGetError() != GL_NO_ERROR);
		
		// For video content rendered through keystone, we need to flip Y coordinates
		// because MPV renders to FBO without flipping (mpv_flip_y = 0 when keystone enabled)
		g_tex_flip_y = 1;
		
		// Correct warping approach: Draw a warped quad where vertices match the keystone corners
		// Convert keystone points from normalized [0,1] space to clip space [-1,1]
		float vertices[] = {
			g_keystone.points[0][0] * 2.0f - 1.0f, 1.0f - (g_keystone.points[0][1] * 2.0f),  // Top left 
			g_keystone.points[1][0] * 2.0f - 1.0f, 1.0f - (g_keystone.points[1][1] * 2.0f),  // Top right
			g_keystone.points[3][0] * 2.0f - 1.0f, 1.0f - (g_keystone.points[3][1] * 2.0f),  // Bottom left 
			g_keystone.points[2][0] * 2.0f - 1.0f, 1.0f - (g_keystone.points[2][1] * 2.0f)   // Bottom right
		};
		
		// Texture coordinates with optional flips
		float u0 = g_tex_flip_x ? 1.0f : 0.0f;
		float u1 = g_tex_flip_x ? 0.0f : 1.0f;
		float v0 = g_tex_flip_y ? 1.0f : 0.0f;
		float v1 = g_tex_flip_y ? 0.0f : 1.0f;
		float texcoords[] = {
			u0, v0,  // Top left
			u1, v0,  // Top right
			u0, v1,  // Bottom left
			u1, v1   // Bottom right
		};
		
		// Enable vertex arrays
		glEnableVertexAttribArray((GLuint)g_keystone_a_position_loc);
		glEnableVertexAttribArray((GLuint)g_keystone_a_texcoord_loc);
		
		// Initialize buffers if needed
		if (g_keystone_vertex_buffer == 0) {
			glGenBuffers(1, &g_keystone_vertex_buffer);
		}
		if (g_keystone_texcoord_buffer == 0) {
			glGenBuffers(1, &g_keystone_texcoord_buffer);
		}
		
		// Bind and set vertex positions
		glBindBuffer(GL_ARRAY_BUFFER, g_keystone_vertex_buffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
		glVertexAttribPointer((GLuint)g_keystone_a_position_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
		
		// Bind and set texture coordinates
		glBindBuffer(GL_ARRAY_BUFFER, g_keystone_texcoord_buffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_DYNAMIC_DRAW);
		glVertexAttribPointer((GLuint)g_keystone_a_texcoord_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
		
		// Prepare a cached index buffer for two triangles
		if (g_keystone_index_buffer == 0) {
			GLushort indices[] = {0, 1, 2, 2, 1, 3};
			glGenBuffers(1, &g_keystone_index_buffer);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_keystone_index_buffer);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
		} else {
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_keystone_index_buffer);
		}
		
	// Draw using indexed triangles
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
	
	// Unbind buffers
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	
	// Disable attribute arrays
	glDisableVertexAttribArray((GLuint)g_keystone_a_position_loc);
	glDisableVertexAttribArray((GLuint)g_keystone_a_texcoord_loc);
	
	// Reset texture binding
	glBindTexture(GL_TEXTURE_2D, 0);
	
	// Check for OpenGL errors after drawing
	GLenum error = glGetError();
	if (error != GL_NO_ERROR) {
		fprintf(stderr, "ERROR: OpenGL error after drawing keystone texture: %d\n", error);
		// Print more details about the error
		switch (error) {
			case GL_INVALID_ENUM: fprintf(stderr, "GL_INVALID_ENUM: An unacceptable value is specified for an enumerated argument\n"); break;
			case GL_INVALID_VALUE: fprintf(stderr, "GL_INVALID_VALUE: A numeric argument is out of range\n"); break;
			case GL_INVALID_OPERATION: fprintf(stderr, "GL_INVALID_OPERATION: The specified operation is not allowed in the current state\n"); break;
			case GL_INVALID_FRAMEBUFFER_OPERATION: fprintf(stderr, "GL_INVALID_FRAMEBUFFER_OPERATION: The framebuffer object is not complete\n"); break;
			case GL_OUT_OF_MEMORY: fprintf(stderr, "GL_OUT_OF_MEMORY: There is not enough memory left to execute the command\n"); break;
			default: fprintf(stderr, "Unknown OpenGL error\n");
		}
	}
		
		// Clean up
		glDisableVertexAttribArray((GLuint)g_keystone_a_position_loc);
		glDisableVertexAttribArray((GLuint)g_keystone_a_texcoord_loc);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		gl_disable_blend_cached(); // Disable blending after drawing the video (cached)
		gl_use_program_cached(0);
		
		// Reset texture flip flag after keystone rendering to avoid affecting other renders
		g_tex_flip_y = 0;
	}
	
	// Draw border around the keystone quad if enabled (software keystone only)
	if (g_show_border && !use_drm_keystone) {
		// Determine quad positions in clip space matching the keystone corners
		float vx = g_keystone.points[0][0]*2.0f-1.0f;
		float vy = 1.0f-(g_keystone.points[0][1]*2.0f);
		float v0[2] = { vx, vy }; // TL
		float v1[2] = { g_keystone.points[1][0]*2.0f-1.0f, 1.0f-(g_keystone.points[1][1]*2.0f) }; // TR
		float v2[2] = { g_keystone.points[3][0]*2.0f-1.0f, 1.0f-(g_keystone.points[3][1]*2.0f) }; // BL
		float v3[2] = { g_keystone.points[2][0]*2.0f-1.0f, 1.0f-(g_keystone.points[2][1]*2.0f) }; // BR
		float lines[] = {
			v0[0], v0[1], v1[0], v1[1], // top edge
			v1[0], v1[1], v3[0], v3[1], // right edge
			v3[0], v3[1], v2[0], v2[1], // bottom edge
			v2[0], v2[1], v0[0], v0[1], // left edge
		};
		// Use border shader (cached)
		gl_use_program_cached(g_border_shader_program);
		glUniform4f(g_border_u_color_loc, 1.0f, 1.0f, 0.0f, 1.0f); // Yellow
	// Upload a tiny VBO on existing vertex buffer to avoid creating a new one
		if (g_keystone_vertex_buffer == 0) glGenBuffers(1, &g_keystone_vertex_buffer);
		glBindBuffer(GL_ARRAY_BUFFER, g_keystone_vertex_buffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(lines), lines, GL_DYNAMIC_DRAW);
		glEnableVertexAttribArray((GLuint)g_border_a_position_loc);
		glVertexAttribPointer((GLuint)g_border_a_position_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
	// Set line width (may be clamped to 1 on some GLES drivers)
	glLineWidth((GLfloat)g_border_width);
	// Draw 4 line segments (each pair of vertices forms a segment)
		glDrawArrays(GL_LINES, 0, 8);
		// Cleanup
		glDisableVertexAttribArray((GLuint)g_border_a_position_loc);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glUseProgram(0);
	}
	
	// Draw corner markers for keystone adjustment if enabled (software keystone only)
	// This is simplistic and would need a shader-based approach for proper GLES implementation
	if (g_keystone.enabled && !use_drm_keystone && g_show_corner_markers) {
		// Draw colored markers at each corner position to show their current locations
		int corner_size = 10;
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		
		int w = d->mode.hdisplay;
		int h = d->mode.vdisplay;
		
		for (int i = 0; i < 4; i++) {
			// Calculate screen position from normalized coordinates
			int x = (int)((float)g_keystone.points[i][0] * (float)w);
			int y = (int)((float)g_keystone.points[i][1] * (float)h);
			
			// Set color: active corner is red, others are green
			if (i == g_keystone.active_corner) {
				glClearColor(1.0f, 0.0f, 0.0f, 0.8f); // Red
			} else {
				glClearColor(0.0f, 1.0f, 0.0f, 0.8f); // Green
			}
			
			// Ensure marker stays visible within screen bounds (integer clamps)
			x = x - corner_size/2;
			y = y - corner_size/2;
			if (x < 0) x = 0; else if (x > w - corner_size) x = w - corner_size;
			if (y < 0) y = 0; else if (y > h - corner_size) y = h - corner_size;
			
			// Draw the corner marker
			glScissor(x, h - y - corner_size, corner_size, corner_size);
			glEnable(GL_SCISSOR_TEST);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
	}
	
	// Render stats overlay if enabled (before buffer swap)
	// Skip stats overlay in performance mode if configured to do so
	if (g_show_stats_overlay && !should_skip_feature_for_performance("stats_overlay")) {
		static int debug_stats_counter = 0;
		if (++debug_stats_counter % 60 == 0) { // Every 60 frames (~1 second)
			fprintf(stderr, "[STATS-DEBUG] Rendering stats overlay: enabled=%d, screen=%dx%d\n", 
					g_show_stats_overlay, (int)d->mode.hdisplay, (int)d->mode.vdisplay);
		}
		stats_overlay_update(&g_stats_overlay);
		stats_overlay_render_text(&g_stats_overlay, (int)d->mode.hdisplay, (int)d->mode.vdisplay);
	} else {
		static int debug_skip_counter = 0;
		if (g_show_stats_overlay && ++debug_skip_counter % 60 == 0) { // Every 60 frames (~1 second)
			fprintf(stderr, "[STATS-DEBUG] Skipping stats overlay: enabled=%d, should_skip=%d\n", 
					g_show_stats_overlay, should_skip_feature_for_performance("stats_overlay"));
		}
	}
	
	// Swap buffers to display the rendered frame
	eglSwapBuffers(e->dpy, e->surf);

	// If we're using DRM keystone, apply the transformation after the OpenGL rendering is done
	if (use_drm_keystone) {
		// Get the current DRM framebuffer ID
		GLint current_fb;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fb);
		
		// Update the DRM keystone transformation for this frame
		if (!drm_keystone_display_frame(NULL, 0, 0, 0, 0)) {
			LOG_WARN("Failed to apply DRM keystone transformation for this frame");
		}
	}

	static bool first_frame = true;

	// Check if we should use zero-copy path
	if (!g_scanout_disabled && should_use_zero_copy(d, e)) {
		// We'd need a way to get the MPV rendered texture, but that's not directly exposed
		// This is something to implement in the future when we can extract the texture from MPV
		GLuint video_texture = 0; 
		
		// Setup source and destination rectangles
		float src_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // Full texture
		float dst_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // Full screen
		
		// Use zero-copy path to present the frame
		if (present_frame_zero_copy(d, e, video_texture, src_rect, dst_rect)) {
			// Successfully presented using zero-copy path
			if (g_debug && first_frame) {
				fprintf(stderr, "[debug] Using zero-copy DMA-BUF path with %s modesetting\n", 
						d->atomic_supported ? "atomic" : "legacy");
			}
			return true;
		}
		
		// If zero-copy fails, fall back to standard path
		if (g_debug && first_frame) {
			fprintf(stderr, "[debug] Zero-copy path failed, falling back to standard path\n");
		}
	}

	// Standard (non-zero-copy) path
	// Get front buffer for rendering
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(e->gbm_surf);
	if (!bo) { fprintf(stderr, "gbm_surface_lock_front_buffer failed\n"); return false; }
	struct fb_holder *h = gbm_bo_get_user_data(bo);
	uint32_t fb_id = h ? h->fb : 0;
	if (!fb_id) {
		uint32_t handle = gbm_bo_get_handle(bo).u32;
		uint32_t pitch  = gbm_bo_get_stride(bo);
		uint32_t width  = gbm_bo_get_width(bo);
		uint32_t height = gbm_bo_get_height(bo);
		if (!g_scanout_disabled && drmModeAddFB(d->fd, width, height, 24, 32, pitch, handle, &fb_id)) {
			fprintf(stderr, "drmModeAddFB failed (w=%u h=%u pitch=%u handle=%u err=%s)\n", width, height, pitch, handle, strerror(errno));
			gbm_surface_release_buffer(e->gbm_surf, bo);
			return false;
		}
		struct fb_holder *nh = calloc(1, sizeof(*nh));
		if (nh) { nh->fb = fb_id; nh->fd = d->fd; gbm_bo_set_user_data(bo, nh, bo_destroy_handler); }
	}
	static bool first=true;
	if (g_debug) fprintf(stderr, "[DEBUG] MPV: first=%d, g_scanout_disabled=%d\n", first, g_scanout_disabled);
	if (!g_scanout_disabled && first) {
		if (g_debug) fprintf(stderr, "[DEBUG] MPV: Performing initial modeset with fb_id=%u\n", fb_id);
		// Initial modeset; retain BO until next successful page flip to avoid premature release while scanning out.
		bool success = false;
		
		if (d->atomic_supported) {
			// Use atomic modesetting for tear-free updates
			success = atomic_present_framebuffer(d, fb_id, g_vsync_enabled);
		} else {
			// Legacy modesetting
			success = (drmModeSetCrtc(d->fd, d->crtc_id, fb_id, 0, 0, &d->connector_id, 1, &d->mode) == 0);
		}
		
		if (!success) {
			int err = errno;
			fprintf(stderr, "[ERROR] MPV: %s failed (%s)\n", 
				d->atomic_supported ? "atomic_present_framebuffer" : "drmModeSetCrtc", 
				strerror(err));
				
			if (err == EACCES || err == EPERM) {
				fprintf(stderr, "[DRM] Permission denied on modeset – entering NO-SCANOUT fallback (offscreen decode).\n");
				g_scanout_disabled = 1;
				// Release this BO immediately; no page flip path.
				gbm_surface_release_buffer(e->gbm_surf, bo);
				return true;
			}
			return false;
		}
		if (g_debug) fprintf(stderr, "[DEBUG] MPV: Initial modeset successful, setting first=false\n");
		first=false;
		g_first_frame_bo = bo; // retain; release after we have performed one page flip on a new frame
		return true; // do not release now
	}
	if (!g_scanout_disabled) {
		g_egl_for_handler = e;
		gettimeofday(&g_last_flip_submit, NULL); // Record time of submission
		
		// For triple buffering, allow up to 2 page flips in flight
		// If we already have max pending flips, wait for one to complete
		int max_pending = g_triple_buffer ? 2 : 1;
		
		if (g_pending_flips >= max_pending) {
			if (g_debug) fprintf(stderr, "[buffer] Waiting for page flip to complete (pending=%d)\n", g_pending_flips);
			
			// Wait for a pending flip to complete before scheduling another
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(d->fd, &fds);
			
			// Set reasonable timeout to avoid indefinite wait
			struct timeval timeout = {0, 100000}; // 100ms
			
			if (select(d->fd + 1, &fds, NULL, NULL, &timeout) <= 0) {
				// Timeout or error occurred, force reset pending flip state
				if (g_debug) fprintf(stderr, "[buffer] Page flip wait timeout, resetting state\n");
				g_pending_flip = 0;
			} else if (FD_ISSET(d->fd, &fds)) {
				// Handle the page flip event
				drmEventContext ev = { .version = DRM_EVENT_CONTEXT_VERSION, .page_flip_handler = page_flip_handler };
				drmHandleEvent(d->fd, &ev);
			}
		}
		
		if (d->atomic_supported) {
			// Use atomic modesetting for page flip
			if (!atomic_present_framebuffer(d, fb_id, g_vsync_enabled)) {
				gbm_surface_release_buffer(e->gbm_surf, bo);
				return false;
			}
		} else {
			// Legacy page flip
			if (g_debug) fprintf(stderr, "[DEBUG] MPV: Calling drmModePageFlip with fb_id=%u\n", fb_id);
			if (drmModePageFlip(d->fd, d->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, bo)) {
				if (g_debug) fprintf(stderr, "[DEBUG] MPV: drmModePageFlip failed: %s\n", strerror(errno));
				gbm_surface_release_buffer(e->gbm_surf, bo);
				return false;
			} else {
				if (g_debug) fprintf(stderr, "[DEBUG] MPV: drmModePageFlip succeeded\n");
			}
		}
		if (g_debug) fprintf(stderr, "[DRM] Setting g_pending_flip = 1 (location 2)\n");
		g_pending_flip = 1; // will release in handler
		g_pending_flips++;  // increment pending flip count
	} else {
		// Offscreen mode: just release BO immediately (no scanout usage).
		if (g_debug) fprintf(stderr, "[DEBUG] MPV: In offscreen mode - g_scanout_disabled=%d\n", g_scanout_disabled);
		gbm_surface_release_buffer(e->gbm_surf, bo);
	}
	
	// End stats timing
	stats_overlay_render_frame_end(&g_stats_overlay);
	
	// No need to remove FB each frame; retained until BO destroyed.
	return true;
}

// Preallocate (discover) up to ring_size unique GBM BOs + FB IDs by performing dummy swaps.
/**
 * Pre-allocate framebuffer objects for triple-buffering
 * 
 * @param d Pointer to DRM context
 * @param e Pointer to EGL context
 * @param ring_size Number of framebuffers to pre-allocate (typically 3 for triple-buffering)
 */
static void preallocate_fb_ring(kms_ctx_t *d, egl_ctx_t *e, int ring_size) {
	if (ring_size <= 0) return;
	if (g_fb_ring.entries) return; // already done
	g_fb_ring.entries = calloc((size_t)ring_size, sizeof(*g_fb_ring.entries));
	if (!g_fb_ring.entries) { fprintf(stderr, "[fb-ring] allocation failed\n"); return; }
	g_fb_ring.count = ring_size;
	fprintf(stderr, "[fb-ring] Preallocating up to %d framebuffers...\n", ring_size);
	// We intentionally do NOT modeset here; we only want FB IDs ready so first real frame
	// can modeset without incurring drmModeAddFB latency.
	for (int i=0; i<ring_size; ++i) {
		// Simple clear to ensure back buffer considered updated.
		glClearColor(0.f,0.f,0.f,1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		eglSwapBuffers(e->dpy, e->surf);
		struct gbm_bo *bo = gbm_surface_lock_front_buffer(e->gbm_surf);
		if (!bo) { fprintf(stderr, "[fb-ring] lock_front_buffer failed at %d\n", i); break; }
		// Check if this BO already seen (gbm may recycle quickly for small surfaces)
		int seen = 0;
		for (int j=0; j<g_fb_ring.produced; ++j) {
			if (g_fb_ring.entries[j].bo == bo) { seen = 1; break; }
		}
		if (!seen) {
			uint32_t fb_id = 0;
			struct fb_holder *h = gbm_bo_get_user_data(bo);
			if (h) fb_id = h->fb;
			if (!fb_id) {
				uint32_t handle = gbm_bo_get_handle(bo).u32;
				uint32_t pitch  = gbm_bo_get_stride(bo);
				uint32_t width  = gbm_bo_get_width(bo);
				uint32_t height = gbm_bo_get_height(bo);
				if (drmModeAddFB(d->fd, width, height, 24, 32, pitch, handle, &fb_id)) {
					fprintf(stderr, "[fb-ring] drmModeAddFB failed (%s)\n", strerror(errno));
					gbm_surface_release_buffer(e->gbm_surf, bo);
					break;
				}
				struct fb_holder *nh = calloc(1, sizeof(*nh));
				if (nh) { nh->fb = fb_id; nh->fd = d->fd; gbm_bo_set_user_data(bo, nh, bo_destroy_handler); }
			}
			if (g_fb_ring.produced < g_fb_ring.count) {
				g_fb_ring.entries[g_fb_ring.produced].bo = bo;
				g_fb_ring.entries[g_fb_ring.produced].fb_id = fb_id;
				g_fb_ring.produced++;
			}
		}
		// Release immediately so normal rendering path can reacquire; we only needed FB ID.
		gbm_surface_release_buffer(e->gbm_surf, bo);
		if (g_fb_ring.produced >= g_fb_ring.count) break;
	}
	fprintf(stderr, "[fb-ring] Prepared %d unique framebuffer(s)\n", g_fb_ring.produced);
}

int main(int argc, char **argv) {
	// Parse command line options
	static struct option long_options[] = {
		{"loop", no_argument, NULL, 'l'},
		{"help", no_argument, NULL, 'h'},
		{"no-v4l2", no_argument, NULL, 'm'},
		{"no-vsync", no_argument, NULL, 'n'},
		{"high-performance", no_argument, NULL, 'p'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "lhmnp", long_options, NULL)) != -1) {
		switch (opt) {
			case 'l':
				g_loop_playback = 1;
				break;
			case 'm':
				// Option removed - old V4L2 decoder no longer supported
				break;
			case 'n':
				g_vsync_enabled = 0;
				fprintf(stderr, "VSync disabled for maximum framerate\n");
				break;
			case 'p':
				g_vsync_enabled = 0;
				g_triple_buffer = 0;  // Reduce latency
				setenv("PICKLE_FORCE_RENDER_LOOP", "1", 1);  // Force continuous rendering
				gl_optimize_for_performance();  // Enable GL optimizations
				fprintf(stderr, "High-performance mode: VSync off, continuous rendering, GL optimizations enabled\n");
				break;
			case 'h':
				fprintf(stderr, "Usage: %s [options] <video-file>\n", argv[0]);
				fprintf(stderr, "Options:\n");
				fprintf(stderr, "  -l, --loop            Loop playback continuously\n");
				fprintf(stderr, "  -m, --no-v4l2         Force MPV decoder (disable V4L2 hardware acceleration)\n");
				fprintf(stderr, "  -n, --no-vsync        Disable VSync for maximum framerate\n");
				fprintf(stderr, "  -p, --high-performance Enable high-performance mode (no VSync, continuous render)\n");
				fprintf(stderr, "  -h, --help            Show this help message\n");
				fprintf(stderr, "\nNote: V4L2 hardware acceleration is used automatically when available.\n");
				return 0;
			default:
				fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
				return 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Error: No input file specified\n");
		fprintf(stderr, "Usage: %s [options] <video-file>\n", argv[0]);
		return 1;
	}

	const char *file = argv[optind];
	
	// Store filename in environment variable for codec selection
	setenv("PICKLE_FILENAME", file, 1);
	
	// Environment variable override
	const char *loop_env = getenv("PICKLE_LOOP");
	if (loop_env && *loop_env) {
		g_loop_playback = atoi(loop_env);
	}
	
	// If looping is enabled, set a longer stall detection threshold
	// This helps prevent false stalls during loop transitions
	if (g_loop_playback) {
		const int LOOP_STALL_MS = 5000; // 5s for loop transitions
		g_wd_ongoing_ms = LOOP_STALL_MS;
		fprintf(stderr, "Looping playback enabled (stall threshold: %dms)\n", LOOP_STALL_MS);
	}

	signal(SIGINT, handle_sigint);
	signal(SIGTERM, handle_sigterm);
	signal(SIGSEGV, handle_sigsegv);

	if (getenv("PICKLE_DEBUG")) g_debug = 1;
	gettimeofday(&g_prog_start, NULL);
	
	// Allow customization of max stall resets
	const char *max_resets = getenv("PICKLE_MAX_STALL_RESETS");
	if (max_resets) {
		int val = atoi(max_resets);
		if (val >= 0) g_max_stall_resets = val;
	}
	
	// Buffer mode configuration
	const char *disable_triple = getenv("PICKLE_NO_TRIPLE_BUFFER");
	if (disable_triple && *disable_triple) g_triple_buffer = 0;
	
	// Vsync control
	const char *disable_vsync = getenv("PICKLE_NO_VSYNC");
	if (disable_vsync && *disable_vsync) g_vsync_enabled = 0;
	
	// Frame timing diagnostics
	const char *timing = getenv("PICKLE_TIMING");
	if (timing && *timing) g_frame_timing_enabled = 1;
	
	// Consider setting a conservative timeout value
	const char *no_stall_check = getenv("PICKLE_NO_STALL_CHECK");
	if (no_stall_check && *no_stall_check) {
		g_max_stall_resets = 0; // Disable stall recovery
	}

	struct kms_ctx drm = {0};
	struct egl_ctx eglc = {0};
	mpv_player_t player = {0};

	// Parse stats env
	const char *stats_env = getenv("PICKLE_STATS");
	if (stats_env && *stats_env && strcmp(stats_env, "0") != 0 && strcasecmp(stats_env, "off") != 0) {
		g_stats_enabled = 1;
		const char *ival = getenv("PICKLE_STATS_INTERVAL");
		if (ival && *ival) {
			double v = atof(ival);
			if (v > 0.05) g_stats_interval_sec = v; // clamp minimal interval
		}
		gettimeofday(&g_stats_start, NULL);
		g_stats_last = g_stats_start;
		fprintf(stderr, "[stats] enabled interval=%.2fs\n", g_stats_interval_sec);
	}

	if (!init_drm(&drm)) RET("init_drm");

	// Initialize stats overlay
	stats_overlay_init(&g_stats_overlay);

	// Check for render backend selection via environment
	const char *backend_env = getenv("PICKLE_BACKEND");
	bool use_vulkan = false;
	
#ifdef VULKAN_ENABLED
	if (backend_env && strcasecmp(backend_env, "vulkan") == 0) {
		fprintf(stderr, "[INFO] Using Vulkan render backend\n");
		use_vulkan = true;
	} else {
		fprintf(stderr, "[INFO] Using OpenGL ES render backend (default)\n");
	}
#else
	if (backend_env && strcasecmp(backend_env, "vulkan") == 0) {
		fprintf(stderr, "[WARN] Vulkan backend requested but not compiled in, falling back to OpenGL ES\n");
	} else {
		fprintf(stderr, "[INFO] Using OpenGL ES render backend\n");
	}
#endif

	if (use_vulkan) {
#ifdef VULKAN_ENABLED
		// Initialize Vulkan
		vulkan_ctx_t vulkan_ctx = {0};
		pickle_result_t vk_result = vulkan_init(&vulkan_ctx, &drm);
		if (vk_result != PICKLE_OK) {
			fprintf(stderr, "[ERROR] Failed to initialize Vulkan, falling back to OpenGL ES\n");
			if (!init_gbm_egl(&drm, &eglc)) RET("init_gbm_egl");
		} else {
			fprintf(stderr, "[INFO] Vulkan initialized successfully\n");
			// Store Vulkan context for later use
			// TODO: Implement Vulkan rendering loop
			// For now, still initialize OpenGL ES for compatibility
			if (!init_gbm_egl(&drm, &eglc)) RET("init_gbm_egl");
		}
#endif
	} else {
		if (!init_gbm_egl(&drm, &eglc)) RET("init_gbm_egl");
	}
	// Optional preallocation of FB ring (env PICKLE_FB_RING, default 3)
	int fb_ring_n = 3; {
		const char *re = getenv("PICKLE_FB_RING");
		if (re && *re) {
			int v = atoi(re); if (v > 0 && v < 16) fb_ring_n = v; }
	}
	preallocate_fb_ring(&drm, &eglc, fb_ring_n);
	
	// Initialize keystone correction
	keystone_init();
	
	// Initialize compute shader keystone if supported
	if (compute_keystone_is_supported()) {
		if (compute_keystone_init()) {
			LOG_INFO("Compute shader keystone initialized successfully");
		} else {
			LOG_WARN("Failed to initialize compute shader keystone, falling back to fragment shader implementation");
		}
	} else {
		LOG_INFO("Compute shader keystone not supported on this platform, using fragment shader implementation");
	}
	
	// Load keystone configuration from file if available
	bool config_loaded = keystone_load_config("./keystone.conf");
    if (!config_loaded) {
        // Try loading from alternative path
        const char *config_path = getenv("PICKLE_KEYSTONE_CONFIG");
        if (config_path) {
            if (keystone_load_config(config_path)) {
                LOG_INFO("Loaded keystone configuration from %s", config_path);
            }
        }
    }
    
    // Ensure the keystone rendering is properly initialized
    // Don't force border visibility, respect the loaded setting
    if (g_keystone.enabled) {
        g_show_border = g_keystone.border_visible ? 1 : 0;
        
        // Make sure shader will be initialized on first render
        if (g_keystone_shader_program == 0) {
            LOG_INFO("Keystone enabled at startup, initializing shader");
            if (!init_keystone_shader()) {
                LOG_ERROR("Failed to initialize keystone shader at startup");
            } else {
                LOG_INFO("Keystone shader initialized successfully");
            }
        }
    }

	
	// Try hardware decoders first (if built with support), fallback to MPV
	
#ifdef USE_FFMPEG_V4L2_PLAYER
	// Check for environment variable override to force MPV decoder
	const char *force_mpv = getenv("PICKLE_FORCE_MPV");
	bool skip_ffmpeg = force_mpv && (*force_mpv == '1' || strcasecmp(force_mpv, "true") == 0);
	
	// Priority 1: Try FFmpeg V4L2 M2M decoder (most reliable for MP4/container formats)
	bool ffmpeg_v4l2_attempted = false;
	
	// Auto-enable FFmpeg V4L2 when available (unless forced to use MPV)
	if (!skip_ffmpeg && ffmpeg_v4l2_is_supported()) {
		LOG_INFO("FFmpeg V4L2 M2M decoder available, using it for better performance");
		g_use_ffmpeg_v4l2 = 1;
	} else if (skip_ffmpeg) {
		LOG_INFO("FFmpeg V4L2 decoder disabled by PICKLE_FORCE_MPV environment variable");
		g_use_ffmpeg_v4l2 = 0;
	}
	
	// Enhanced decoder initialization with better fallback handling
	bool decoder_initialized = false;
	const char *decoder_name = "Unknown";

	if (g_use_ffmpeg_v4l2) {
		ffmpeg_v4l2_attempted = true;
		LOG_INFO("Attempting FFmpeg V4L2 M2M hardware decoder initialization...");
		
		// Store filename in environment for codec selection
		setenv("PICKLE_FILENAME", file, 1);
		
		// Clear any previous state
		memset(&ffmpeg_v4l2_player, 0, sizeof(ffmpeg_v4l2_player));
		
		if (init_ffmpeg_v4l2_player(&ffmpeg_v4l2_player, file)) {
			// Successfully initialized FFmpeg V4L2
			g_video_fps = ffmpeg_v4l2_player.fps;
			decoder_initialized = true;
			decoder_name = "FFmpeg V4L2 M2M";
			
			LOG_INFO("FFmpeg V4L2 decoder initialized successfully: %ux%u @ %.2f fps", 
			         ffmpeg_v4l2_player.width, ffmpeg_v4l2_player.height, g_video_fps);
		} else {
			LOG_WARN("FFmpeg V4L2 decoder initialization failed, cleaning up...");
			// Ensure clean state before fallback
			cleanup_ffmpeg_v4l2_player(&ffmpeg_v4l2_player);
			memset(&ffmpeg_v4l2_player, 0, sizeof(ffmpeg_v4l2_player));
			g_use_ffmpeg_v4l2 = 0;
		}
	}

	// If FFmpeg V4L2 failed or wasn't used, fall back to MPV
	if (!decoder_initialized) {
		if (ffmpeg_v4l2_attempted) {
			LOG_INFO("Falling back to MPV decoder...");
		} else {
			LOG_INFO("Initializing MPV decoder...");
		}
		memset(&player, 0, sizeof(player));
		
		if (init_mpv(&player, file)) {
			decoder_initialized = true;
			decoder_name = "MPV";
			g_mpv_wakeup = 1;
			LOG_INFO("MPV decoder initialized successfully");
		} else {
			LOG_ERROR("MPV decoder initialization failed");
			RET("Failed to initialize any decoder - both FFmpeg V4L2 and MPV failed");
		}
	}
	
#else
	// No FFmpeg V4L2 support compiled in, use MPV directly
	LOG_INFO("Initializing MPV decoder (FFmpeg V4L2 support not compiled in)");
	memset(&player, 0, sizeof(player));
	
	if (init_mpv(&player, file)) {
		decoder_initialized = true;
		decoder_name = "MPV";
		g_mpv_wakeup = 1;
		LOG_INFO("MPV decoder initialized successfully");
	} else {
		LOG_ERROR("MPV decoder initialization failed");
		RET("Failed to initialize MPV decoder");
	}
#endif

	// Verify that we have a working decoder
	if (!decoder_initialized) {
		LOG_ERROR("No decoder could be initialized");
		RET("No working decoder available");
	}

	// Enhanced decoder status reporting
	const char *decoder_details = "";
#ifdef USE_FFMPEG_V4L2_PLAYER
	if (g_use_ffmpeg_v4l2) {
		decoder_details = " (hardware accelerated)";
	} else {
		decoder_details = player.using_libmpv ? " (libmpv)" : " (gpu output)";
	}
#else
	decoder_details = player.using_libmpv ? " (libmpv)" : " (gpu output)";
#endif

	fprintf(stderr, "Playing %s at %dx%d %.2f Hz using %s%s\n", 
		file, drm.mode.hdisplay, drm.mode.vdisplay,
		(drm.mode.vrefresh ? (double)drm.mode.vrefresh : (double)drm.mode.clock / (drm.mode.htotal * drm.mode.vtotal)),
		decoder_name, decoder_details);
	
	// Print keystone control instructions
	if (g_keystone.enabled) {
		fprintf(stderr, "\nKeystone correction enabled. Controls:\n");
	} else {
		fprintf(stderr, "\nKeystone correction available. Controls:\n");
	}
	fprintf(stderr, "  q - Quit application\n");
	fprintf(stderr, "  k - Toggle keystone mode\n");
	fprintf(stderr, "  1-4 - Select corner to adjust\n");
	fprintf(stderr, "  Arrow keys - Move selected corner up/down/left/right\n");
	fprintf(stderr, "  +/- - Increase/decrease adjustment step size\n");
	fprintf(stderr, "  r - Reset keystone to default\n");
	fprintf(stderr, "  b - Toggle border around video\n");
	fprintf(stderr, "  [/] - Decrease/increase border width\n");
	fprintf(stderr, "  v - Toggle performance stats overlay (FPS, CPU, GPU, RAM)\n");
	fprintf(stderr, "  (border draws around keystone quad; background is always black)\n\n");

	// Watchdog: if no frame submitted within WD_FIRST_MS, force a render attempt even if mpv flags missing.
	int frames = 0;
	int force_loop = getenv("PICKLE_FORCE_RENDER_LOOP") ? 1 : 0;
	
	// Hardware decoders require continuous polling, so always enable force_loop mode
#ifdef USE_FFMPEG_V4L2_PLAYER
	if (g_use_ffmpeg_v4l2) {
		force_loop = 1;
		LOG_DEBUG("FFmpeg V4L2 decoder enabled, forcing continuous render loop for active polling");
	}
#endif
	struct timeval wd_last_activity; gettimeofday(&wd_last_activity, NULL);
	gettimeofday(&g_last_frame_time, NULL); // Initialize last frame time
	int wd_forced_first = 0;
	// Create wakeup pipe (non-blocking) to integrate mpv callback into poll
	if (g_mpv_pipe[0] < 0) {
		if (pipe(g_mpv_pipe) == 0) {
			int fl = fcntl(g_mpv_pipe[0], F_GETFL, 0); fcntl(g_mpv_pipe[0], F_SETFL, fl | O_NONBLOCK);
			fl = fcntl(g_mpv_pipe[1], F_GETFL, 0); fcntl(g_mpv_pipe[1], F_SETFL, fl | O_NONBLOCK);
		} else {
			fprintf(stderr, "[mpv] pipe() failed (%s)\n", strerror(errno));
		}
	}
	
	// Configure terminal for raw input mode to capture keystrokes
	struct termios new_term;
	tcgetattr(STDIN_FILENO, &g_original_term);
	new_term = g_original_term;
	new_term.c_lflag &= (tcflag_t)~(ICANON | ECHO); // Disable canonical mode and echo
	tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
	g_terminal_modified = true;
	
	// Register terminal restoration on exit
	atexit(restore_terminal_state);
	
	// Set stdin to non-blocking mode
	int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
	
	// Initialize joystick/gamepad support for 8BitDo controller
	if (init_joystick()) {
		LOG_INFO("8BitDo controller detected and enabled for keystone adjustment");
		LOG_INFO("Controller mappings:");
		LOG_INFO("  START = Toggle keystone mode");
		LOG_INFO("  SELECT = Reset keystone to defaults");
		LOG_INFO("  X button = Cycle corners (TL->TR->BR->BL)");
		LOG_INFO("  B button = Toggle border/help display");
		LOG_INFO("  HOME/Guide = Toggle border display");
		LOG_INFO("  Left stick = Move selected corner");
		LOG_INFO("  L1+R1 together = Toggle keystone mode");
		LOG_INFO("  START+SELECT (hold 2s) = Quit application");
	}
	
#ifdef EVENT_DRIVEN_ENABLED
	// Initialize the event-driven architecture
	event_ctx_t *event_ctx = pickle_event_init(&drm, &player, NULL);
	if (!event_ctx) {
		LOG_ERROR("Failed to initialize event system");
		goto cleanup;
	}
	LOG_INFO("Event-driven architecture initialized");

	// Main loop using event-driven architecture
	while (!g_stop) {
		if (!pickle_event_process_and_render(event_ctx, &drm, &eglc, &player, 
		                                   NULL, 100)) {
			break;
		}
		stats_log_periodic(&player);
	}
	
	// Clean up event system
	pickle_event_cleanup(event_ctx);
#else
	// Original polling-based main loop
	if (g_debug) {
		fprintf(stderr, "[MAIN] Entering main loop, force_loop=%d\n", force_loop);
		fflush(stderr);
	}
	while (!g_stop) {
		if (g_debug) {
			fprintf(stderr, "[MAIN] Top of loop iteration\n");
			fflush(stderr);
		}
		// MPV event handling
		if (g_mpv_wakeup && player.mpv) {
			g_mpv_wakeup = 0;
			drain_mpv_events(player.mpv);
			if (player.rctx) {
				uint64_t flags = mpv_render_context_update(player.rctx);
				g_mpv_update_flags |= flags;
			}
		}
		
		// Periodically update video FPS from MPV
		static struct timeval last_fps_query = {0, 0};
		struct timeval now_fps;
		gettimeofday(&now_fps, NULL);
	if (last_fps_query.tv_sec == 0) {
		last_fps_query = now_fps;
	}
	double fps_elapsed = tv_diff(&last_fps_query, &now_fps);
	if (fps_elapsed > 5.0) { // Query every 5 seconds
		double container_fps = 0.0;
		if (player.mpv && mpv_get_property(player.mpv, "container-fps", MPV_FORMAT_DOUBLE, &container_fps) >= 0 && container_fps > 0) {
			static double last_reported_fps = 0.0;
			if (g_video_fps != container_fps) { // Only log when FPS actually changes
				g_video_fps = container_fps;
				if (container_fps != last_reported_fps) {
					LOG_INFO("Updated video FPS from MPV: %.2f fps", g_video_fps);
					last_reported_fps = container_fps;
				}
			}
		}
		last_fps_query = now_fps;
	}		// Check controller quit combo (START+SELECT 2s)
		if (is_joystick_enabled()) {
			// Polling cadence: on every loop iteration; guarded by state flags
			struct timeval now; gettimeofday(&now, NULL);
			(void)now; // currently unused beyond gettimeofday side effect
			// Reuse debounce window — quit independent of debounce thresholds
			if (g_js_start_down && g_js_select_down) {
				long ms_start = (now.tv_sec - g_js_start_time.tv_sec) * 1000 + (now.tv_usec - g_js_start_time.tv_usec) / 1000;
				long ms_select = (now.tv_sec - g_js_select_time.tv_sec) * 1000 + (now.tv_usec - g_js_select_time.tv_usec) / 1000;
				long held_ms = (ms_start < ms_select) ? ms_start : ms_select;
				if (!g_js_quit_fired && held_ms >= 2000) {
					LOG_INFO("Quit via controller: START+SELECT held for %ld ms", held_ms);
					g_stop = 1;
					g_js_quit_fired = true;
				}
			} else {
				g_js_quit_fired = false;
			}
		}

		// Process help toggle request from controller
		if (g_help_toggle_request) {
			g_help_toggle_request = 0;
			if (player.mpv) {
				if (!g_help_visible) {
					show_help_overlay(player.mpv);
					g_help_visible = 1;
				} else {
					hide_help_overlay(player.mpv);
					g_help_visible = 0;
				}
				g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
			}
		}
		// Prepare pollfds: DRM fd (for page flip events) + mpv wakeup pipe + stdin for keyboard + joystick
		struct pollfd pfds[4]; int n=0;
		if (!g_scanout_disabled) { pfds[n].fd = drm.fd; pfds[n].events = POLLIN; pfds[n].revents = 0; n++; }
		if (g_mpv_pipe[0] >= 0) { pfds[n].fd = g_mpv_pipe[0]; pfds[n].events = POLLIN; pfds[n].revents = 0; n++; }
		
		// Add stdin to the poll set to capture keyboard input
		pfds[n].fd = STDIN_FILENO; pfds[n].events = POLLIN; pfds[n].revents = 0; n++;
		
		// Add joystick to poll set if available
		if (is_joystick_enabled() && get_joystick_fd() >= 0) {
			pfds[n].fd = get_joystick_fd(); pfds[n].events = POLLIN; pfds[n].revents = 0; n++;
		}
		int timeout_ms = -1;
		
		// Calculate appropriate poll timeout based on frame rate and vsync
		if (force_loop || (g_mpv_update_flags & MPV_RENDER_UPDATE_FRAME)) {
			// Use minimal timeout even in continuous mode to allow DRM events to be processed
			timeout_ms = g_pending_flip ? 5 : 1; // 5ms if waiting for flip, 1ms otherwise
		} else if (!g_vsync_enabled) {
			// When vsync is disabled, use very short timeout for maximum fps
			timeout_ms = 1; // 1ms for aggressive polling (up to 1000fps theoretical)
		} else if (frames > 0 && g_vsync_enabled) {
			// Estimate appropriate timeout based on refresh rate for vsync
			double refresh_rate = drm.mode.vrefresh ? 
				(double)drm.mode.vrefresh : 
				(double)drm.mode.clock / (drm.mode.htotal * drm.mode.vtotal);
			
			// Use adaptive frame pacing for optimal timeout
			timeout_ms = decoder_pacing_get_timeout_ms();
			
			// Legacy fallback if adaptive pacing not available
			if (timeout_ms <= 0) {
				double fps_for_timing = (g_video_fps > 0) ? g_video_fps : refresh_rate;
				if (fps_for_timing <= 0) fps_for_timing = 60.0;
				timeout_ms = (int)(500.0 / fps_for_timing);
				if (timeout_ms < 1) timeout_ms = 1;
				if (timeout_ms > 33) timeout_ms = 33;
			}
		}
		
		// Add a max timeout to avoid being stuck in poll forever even if no events come
		// This allows our watchdog logic to run periodically
		if (timeout_ms < 0) timeout_ms = 100; // max 100ms poll timeout

	int pr = poll(pfds, (nfds_t)n, timeout_ms);
		if (pr < 0) { 
			if (errno == EINTR) {
				// Signal interrupted poll - check if we should stop
				if (g_stop) break;
				continue;
			}
			fprintf(stderr, "poll failed (%s)\n", strerror(errno)); 
			break; 
		}
		// Check g_stop even when poll succeeds/times out, in case signal came during processing
		if (g_stop) break;
		for (int i=0;i<n;i++) {
			if (!(pfds[i].revents & POLLIN)) continue;
			if (pfds[i].fd == drm.fd) {
				if (g_debug) fprintf(stderr, "[DRM] Processing DRM event, pending_flip was %d\n", g_pending_flip);
				drmEventContext ev = { .version = DRM_EVENT_CONTEXT_VERSION, .page_flip_handler = page_flip_handler };
				drmHandleEvent(drm.fd, &ev);
			} else if (pfds[i].fd == g_mpv_pipe[0]) {
				unsigned char buf[64]; while (read(g_mpv_pipe[0], buf, sizeof(buf)) > 0) { /* drain */ }
				g_mpv_wakeup = 1;
			} else if (pfds[i].fd == STDIN_FILENO) {
				// Handle keyboard input
				char c;
				if (read(STDIN_FILENO, &c, 1) > 0) {
				// Log keypress for debugging (quiet by default)
				LOG_DEBUG("Key pressed: %d (0x%02x) '%c'", (int)c, (int)c, (c >= 32 && c < 127) ? c : '?');
				
				// Handle Ctrl+C (ASCII 3) and Ctrl+D (ASCII 4) to quit
				// Note: Don't handle ESC (27) here as it's part of arrow key sequences
				if (c == 3 || c == 4) {
					LOG_INFO("Exit key pressed (Ctrl+C or Ctrl+D), stopping...");
					g_stop = 1;
					break;
				}					// Special case: Force keystone mode with 'K' (capital K)
					if (c == 'K') {
						LOG_INFO("Force enabling keystone mode with capital K");
						g_keystone.enabled = true;
						g_keystone.active_corner = 0;
						keystone_update_matrix();
						LOG_INFO("Keystone correction FORCE enabled, adjusting corner %d", g_keystone.active_corner + 1);
						fprintf(stderr, "\rKeystone correction FORCE enabled, use arrow keys to adjust corner %d", 
								g_keystone.active_corner + 1);
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
						continue;
					}
					
				// Help overlay
				if (c == 'h' && !g_key_seq_state.in_escape_seq) {
					if (player.mpv) {
						if (!g_help_visible) {
							show_help_overlay(player.mpv);
							g_help_visible = 1;
						} else {
							hide_help_overlay(player.mpv);
							g_help_visible = 0;
						}
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
					}
					continue;
				}					// Handle keystone adjustment keys first (to avoid 'q' conflict)
					bool keystone_handled = handle_keyboard_input(c);
					LOG_DEBUG("Keystone handler returned: %d", keystone_handled);
					if (keystone_handled) {
						// Force a redraw when keystone parameters change
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
						continue;
					}
					// If not handled by keystone, allow 'q' to quit
					if (c == 'q' && !g_key_seq_state.in_escape_seq) {
						LOG_INFO("Quit requested by user");
						g_stop = 1;
						break;
					}
				}
			} else if (is_joystick_enabled() && pfds[i].fd == get_joystick_fd()) {
				// Handle joystick input
				struct js_event event;
				int events_read = 0;
				while (read(get_joystick_fd(), &event, sizeof(event)) > 0) {
					events_read++;
					if (handle_joystick_event(&event)) {
						// Force a redraw when keystone parameters change
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
					}
				}

			}
		}
		
		// Periodic gamepad connection check (low resource)
		check_gamepad_connection();
		
		// Process held D-pad buttons for continuous movement
		process_dpad_movement();
		if (g_mpv_wakeup && player.mpv) {
			g_mpv_wakeup = 0;
			drain_mpv_events(player.mpv);
			if (player.rctx) {
				uint64_t flags = mpv_render_context_update(player.rctx);
				g_mpv_update_flags |= flags;
			}
		}
		if (g_stop) break;
		int need_frame = 0;
		if (frames == 0 && !g_pending_flip) need_frame = 1; // guarantee first frame submission
		else if (force_loop && !g_pending_flip) need_frame = 1; // continuous mode
		else if ((g_mpv_update_flags & MPV_RENDER_UPDATE_FRAME) && !g_pending_flip) need_frame = 1;
		
		if (g_debug) {
			fprintf(stderr, "[LOOP] frames=%d, force_loop=%d, pending_flip=%d, need_frame=%d\n", 
					frames, force_loop, g_pending_flip, need_frame);
		}

		// Watchdog: if still no frame after WD_FIRST_MS since start, force once.
		if (!frames && !need_frame && !wd_forced_first) {
			struct timeval now; gettimeofday(&now, NULL);
			double since = tv_diff(&now, &g_prog_start) * 1000.0; // ms
			if (since > g_wd_first_ms) {
				if (g_debug) fprintf(stderr, "[wd] forcing first frame after %.1f ms inactivity\n", since);
				need_frame = 1; wd_forced_first = 1;
			}
		}
		
		// Ongoing playback stall detection
		if (frames > 0 && !need_frame && !g_pending_flip) {
			struct timeval now; gettimeofday(&now, NULL);
			double since_last_frame = tv_diff(&now, &g_last_frame_time) * 1000.0; // ms
			
			// If we haven't rendered a frame in g_wd_ongoing_ms, try to recover
			if (since_last_frame > g_wd_ongoing_ms && g_stall_reset_count < g_max_stall_resets) {
				fprintf(stderr, "[wd] playback stall detected - no frames for %.1f ms, attempting recovery (attempt %d/%d)\n", 
					since_last_frame, g_stall_reset_count+1, g_max_stall_resets);
				
				// Reset potential stuck state
				g_pending_flip = 0;
				g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME; // Force frame rendering
				need_frame = 1;
			g_stall_reset_count++;
			
			// Try to get mpv back on track by forcing an update
			if (player.rctx && player.mpv) {
				uint64_t flags = mpv_render_context_update(player.rctx);
				g_mpv_update_flags |= flags;					// Reset decoder if needed (for more aggressive recovery)
					if (g_stall_reset_count > 1) {
						// When looping, first check if we're at the end of the file
						if (g_loop_playback) {
							// Get current position and duration
							double pos = 0, duration = 0;
							mpv_get_property(player.mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
							mpv_get_property(player.mpv, "duration", MPV_FORMAT_DOUBLE, &duration);
							
							// If we're near the end, force a restart
							if (duration > 0 && pos > (duration - 1.0)) {
								fprintf(stderr, "[wd] near end of file (%.1f/%.1f), forcing restart for loop\n", pos, duration);
								const char *cmd[] = {"loadfile", mpv_get_property_string(player.mpv, "path"), "replace", NULL};
								mpv_command_async(player.mpv, 0, cmd);
							} else {
								// Just try to step forward
								const char *cmd[] = {"frame-step", NULL};
								mpv_command_async(player.mpv, 0, cmd);
								fprintf(stderr, "[wd] requesting explicit frame-step for recovery\n");
							}
						}
						
						// If we're still having issues, try cycling the hardware decoder
						if (g_stall_reset_count > 2) {
							const char *cmd[] = {"cycle-values", "hwdec", "auto-safe", "no", NULL};
							mpv_command_async(player.mpv, 0, cmd);
							fprintf(stderr, "[wd] cycling hwdec as part of recovery\n");
						}
					}
				}
			}
		}
		if (need_frame) {
			if (g_debug && frames < 10) fprintf(stderr, "[debug] rendering frame #%d flags=0x%llx pending_flip=%d\n", frames, (unsigned long long)g_mpv_update_flags, g_pending_flip);
			
			if (g_debug) {
				fprintf(stderr, "[MAIN] need_frame=1, frames=%d, pending_flip=%d\n", frames, g_pending_flip);
			}
			
		bool render_success = false;
		
#ifdef USE_FFMPEG_V4L2_PLAYER
		if (g_use_ffmpeg_v4l2) {
			// FFmpeg V4L2 M2M decoder rendering path
			if (g_debug) fprintf(stderr, "[MAIN] Calling ffmpeg_v4l2_get_frame...\n");
			if (ffmpeg_v4l2_get_frame(&ffmpeg_v4l2_player)) {
				// Success - reset failure counter
				g_v4l2_consecutive_failures = 0;
				if (g_debug) fprintf(stderr, "[MAIN] Got frame, calling upload_to_gl...\n");
				if (ffmpeg_v4l2_upload_to_gl(&ffmpeg_v4l2_player)) {
					if (g_debug) fprintf(stderr, "[MAIN] Uploaded, calling render...\n");
					render_success = render_ffmpeg_v4l2_frame(&drm, &eglc, &ffmpeg_v4l2_player);
					if (g_debug) fprintf(stderr, "[MAIN] Render returned: %d\n", render_success);
				} else {
					fprintf(stderr, "[MAIN] Upload FAILED\n");
					render_success = false;  // Upload failure should trigger fallback
				}
			} else {
				// Frame retrieval failed - check why
				
				// Check for EOF first (before incrementing failure counter)
				if (ffmpeg_v4l2_player.eof_reached && g_loop_playback) {
					// Reset for loop playback
					fprintf(stderr, "[MAIN] EOF reached in loop mode, resetting...\n");
					ffmpeg_v4l2_reset(&ffmpeg_v4l2_player);
					g_v4l2_consecutive_failures = 0;  // Reset failure counter
					render_success = true;  // Continue with loop playback
				} else if (ffmpeg_v4l2_player.eof_reached) {
					// End of video in non-loop mode - this is normal, not a failure
					fprintf(stderr, "[MAIN] EOF reached, exiting normally\n");
					LOG_INFO("End of video reached");
					g_stop = 1;
					break;
				} else if (ffmpeg_v4l2_player.fatal_error) {
					fprintf(stderr, "[MAIN] FFmpeg V4L2 reported fatal error, requesting fallback\n");
					render_success = false;
					g_v4l2_fallback_requested = 1;
				} else {
					// Not EOF, not fatal - this is a real failure, increment counter
					g_v4l2_consecutive_failures++;
					if (g_debug && g_v4l2_consecutive_failures % 10 == 0) {
						fprintf(stderr, "[MAIN] Get frame failed %d times (not EOF/fatal)\n", 
						       g_v4l2_consecutive_failures);
					}
					
					if (g_v4l2_consecutive_failures >= MAX_V4L2_FAILURES) {
						// Too many consecutive failures - trigger fallback
						fprintf(stderr, "[MAIN] Too many consecutive failures (%d), triggering fallback\n", 
							   g_v4l2_consecutive_failures);
						render_success = false;
						g_v4l2_fallback_requested = 1;  // Request fallback to MPV
					} else {
						// Normal EAGAIN case - don't render, don't increment frames, just continue loop
						// This allows poll() to be called and signals to be processed
						if (g_debug && g_v4l2_consecutive_failures < 5) {
							fprintf(stderr, "[MAIN] EAGAIN, will retry next iteration (fail count: %d)\n", 
								   g_v4l2_consecutive_failures);
						}
						// Set render_success to true to avoid fallback, but don't actually render anything
						render_success = true;
					}
				}
			}
		} else
#endif
		{
			render_success = render_frame_fixed(&drm, &eglc, &player);
		}
		
		if (!render_success) {
#ifdef USE_FFMPEG_V4L2_PLAYER
			// Check if this is an FFmpeg V4L2 failure that should trigger fallback
			if (g_use_ffmpeg_v4l2 && (g_v4l2_fallback_requested || frames <= 5)) {
				LOG_WARN("FFmpeg V4L2 failed (%s), attempting fallback to MPV", 
				         g_v4l2_fallback_requested ? "timeout/error" : "early failure");
				if (fallback_to_mpv(file, &player)) {
					// Reset frame counter and continue with MPV
					frames = 0;
					g_v4l2_fallback_requested = 0;
					g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
					
					// Give MPV a moment to initialize
					fprintf(stderr, "[FALLBACK] Successfully switched to MPV, continuing playback\n");
					continue; // Try again with MPV
				} else {
					fprintf(stderr, "[FALLBACK] MPV fallback initialization failed\n");
				}
			}
#endif
			fprintf(stderr, "Render failed and no fallback available, exiting\n"); 
			break; 
		} else {
			// Only increment frame counter and update stats if we actually rendered a frame
			// For EAGAIN case, render_success is true but we didn't actually render
#ifdef USE_FFMPEG_V4L2_PLAYER
			// Check if this was an actual frame render or just EAGAIN continuation
			if (g_use_ffmpeg_v4l2) {
				// Only count as a frame if failure count is 0 (meaning we got a frame)
				if (g_v4l2_consecutive_failures == 0) {
					frames++;
					g_mpv_update_flags &= ~(uint64_t)MPV_RENDER_UPDATE_FRAME;
					if (g_stats_enabled) { g_stats_frames++; stats_log_periodic(&player); }
					gettimeofday(&wd_last_activity, NULL);
					gettimeofday(&g_last_frame_time, NULL); // Update last successful frame time
					
					// Reset stall counter after successful frames
					if (g_stall_reset_count > 0) {
						fprintf(stderr, "[wd] playback resumed normally, resetting stall counter\n");
						g_stall_reset_count = 0;
					}
				}
			} else
#endif
			{
				// MPV or other renderer - always count frames
				frames++;
				g_mpv_update_flags &= ~(uint64_t)MPV_RENDER_UPDATE_FRAME;
				if (g_stats_enabled) { g_stats_frames++; stats_log_periodic(&player); }
				gettimeofday(&wd_last_activity, NULL);
				gettimeofday(&g_last_frame_time, NULL);
				
				if (g_stall_reset_count > 0) {
					fprintf(stderr, "[wd] playback resumed normally, resetting stall counter\n");
					g_stall_reset_count = 0;
				}
			}
		}
		}  // End of if (need_frame) block
		if (force_loop && !need_frame && !g_pending_flip) usleep(1000); // light backoff
	}
#endif // EVENT_DRIVEN_ENABLED

	stats_log_final(&player);
	
	// Save keystone settings to a file if they were modified
	if (g_keystone.enabled) {
		// First try to save to local directory
		if (keystone_save_config("./keystone.conf")) {
			LOG_INFO("Saved keystone configuration to ./keystone.conf");
		} else {
			// If that fails, save to user's home directory
			const char* home = getenv("HOME");
			if (home) {
				char config_path[512];
				snprintf(config_path, sizeof(config_path), "%s/.config", home);
				
				// Create .config directory if it doesn't exist
				mkdir(config_path, 0755);
				
				snprintf(config_path, sizeof(config_path), "%s/.config/pickle_keystone.conf", home);
				if (keystone_save_config(config_path)) {
					LOG_INFO("Saved keystone configuration to %s", config_path);
				}
			}
		}
	}
	
	// Restore terminal settings
	restore_terminal_state();
	
	// Clean up joystick resources
	cleanup_joystick();
	
	
	// Clean up compute shader keystone if initialized
	compute_keystone_cleanup();
	
	// Clean up keystone resources
	keystone_cleanup();
	
	// Release first frame BO if still held (must be done before deinit_gbm_egl)
	if (g_first_frame_bo && g_egl_for_handler && g_egl_for_handler->gbm_surf) {
		gbm_surface_release_buffer(g_egl_for_handler->gbm_surf, g_first_frame_bo);
		g_first_frame_bo = NULL;
	}

	// Clean up decoder resources
#ifdef USE_FFMPEG_V4L2_PLAYER
	if (g_use_ffmpeg_v4l2) {
		// Make sure EGL context is current for GL cleanup
		if (eglc.dpy != EGL_NO_DISPLAY && eglc.ctx != EGL_NO_CONTEXT && eglc.surf != EGL_NO_SURFACE) {
			eglMakeCurrent(eglc.dpy, eglc.surf, eglc.surf, eglc.ctx);
		}
		cleanup_ffmpeg_v4l2_player(&ffmpeg_v4l2_player);
	} else
#endif
	{
		destroy_mpv(&player);
	}
	deinit_gbm_egl(&eglc);
	deinit_drm(&drm);
	return 0;
fail:
	// Restore terminal settings in case of failure
	restore_terminal_state();
	
	// Clean up joystick resources
	cleanup_joystick();
	
	
	// Clean up compute shader keystone if initialized
	compute_keystone_cleanup();
	
	// Clean up keystone resources
	keystone_cleanup();
	
	destroy_mpv(&player);
	deinit_gbm_egl(&eglc);
	deinit_drm(&drm);
	return 1;
}

