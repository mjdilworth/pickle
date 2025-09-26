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
#include "utils.h"
#include "shader.h"
#include "pickle_stubs.h"
#include "keystone.h"
#include "input.h"
#include "hvs_keystone.h"
#include "compute_keystone.h"
#include "drm.h"
#include "egl.h"
#include "stats_overlay.h"
#include "h264_analysis.h"
#include "hwdec_monitor.h"

// For event-driven architecture
#ifdef EVENT_DRIVEN_ENABLED
#include "v4l2_player.h"
#include "mpv.h"  
#include "pickle_events.h"
#endif

#include "v4l2_decoder.h"
#ifdef USE_V4L2_DECODER
#include "mp4_demuxer.h"
#include <libavcodec/avcodec.h>
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
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
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

// OpenGL compatibility defines for matrix operations (not in GLES2 headers)
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

// Exported globals for use in other modules
volatile sig_atomic_t g_stop = 0;

// Global hardware decoder monitor
static hwdec_monitor_t g_hwdec_monitor = {0};

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
static int g_have_master = 0; // set if we successfully become DRM master
int g_use_v4l2_decoder = 0; // Use V4L2 decoder instead of MPV's internal decoder

// These keystone settings are defined in keystone.c through pickle_keystone adapter
// Removed static variables and using extern from pickle_keystone.h

static int g_loop_playback = 0; // Whether to loop video playback

// Simple solid-color shader for drawing outlines/borders around keystone quad
#ifdef ENABLE_BORDER_SHADER
static GLuint g_border_shader_program = 0;
static GLuint g_border_vertex_shader = 0;
static GLuint g_border_fragment_shader = 0;
static GLint  g_border_a_position_loc = -1;
static GLint  g_border_u_color_loc = -1;
#endif

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
#ifndef PICKLE_MPV_H
// Only define this if mpv.h hasn't been included
typedef struct {
	mpv_handle *mpv;             // MPV API handle
	mpv_render_context *rctx;    // MPV render context for OpenGL rendering
	int using_libmpv;            // Flag indicating fallback to vo=libmpv occurred
} mpv_player_t_unused; // Renamed to avoid conflict
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
static void on_mpv_events(void *data) { (void)data; g_mpv_wakeup = 1; }

// Performance controls
static int g_triple_buffer = 1;         // Enable triple buffering by default
static int g_vsync_enabled = 1;         // Enable vsync by default
static int g_frame_timing_enabled = 0;  // Detailed frame timing metrics (when PICKLE_TIMING=1)

// Texture orientation controls (used in keystone pass only)
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
	double since_last = tv_diff(&now, &g_stats_last);
	if (since_last < g_stats_interval_sec) return;
	double total = tv_diff(&now, &g_stats_start);
	uint64_t frames_now = g_stats_frames;
	uint64_t delta_frames = frames_now - g_stats_last_frames;
	double inst_fps = (since_last > 0.0) ? (double)delta_frames / since_last : 0.0;
	double avg_fps  = (total > 0.0) ? (double)frames_now / total : 0.0;
	// Query mpv drop stats if possible
	int64_t drop_dec = 0, drop_vo = 0;
	if (p && p->handle) {
		mpv_get_property(p->handle, "drop-frame-count", MPV_FORMAT_INT64, &drop_dec);
		mpv_get_property(p->handle, "vo-drop-frame-count", MPV_FORMAT_INT64, &drop_vo);
	}
	fprintf(stderr, "[stats] total=%.2fs frames=%llu avg_fps=%.2f inst_fps=%.2f dropped_dec=%lld dropped_vo=%lld\n",
			total, (unsigned long long)frames_now, avg_fps, inst_fps,
			(long long)drop_dec, (long long)drop_vo);
	g_stats_last = now;
	g_stats_last_frames = frames_now;
}

static void stats_log_final(mpv_player_t *p) {
	if (!g_stats_enabled) return;
	struct timeval now; gettimeofday(&now, NULL);
	double total = tv_diff(&now, &g_stats_start);
	double avg_fps = (total > 0.0) ? (double)g_stats_frames / total : 0.0;
	int64_t drop_dec = 0, drop_vo = 0;
	if (p && p->handle) {
		mpv_get_property(p->handle, "drop-frame-count", MPV_FORMAT_INT64, &drop_dec);
		mpv_get_property(p->handle, "vo-drop-frame-count", MPV_FORMAT_INT64, &drop_vo);
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
		"  START: toggle keystone\n"
		"  Cycle button (default X): corners TL -> TR -> BR -> BL\n"
		"  Help button (default B): toggle this help\n"
		"  D-Pad/Left stick: move point\n"
		"  L1/R1: step -/+    SELECT: reset    Y/Home(Guide): toggle border\n"
		"  START+SELECT (hold 2s): quit\n";
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
	memset(p, 0, sizeof(*p));
	const char *no_mpv = getenv("PICKLE_NO_MPV");
	if (no_mpv && *no_mpv) {
		fprintf(stderr, "[mpv] Skipping mpv initialization (PICKLE_NO_MPV set)\n");
		return true;
	}
	p->handle = mpv_create();
	if (!p->handle) { fprintf(stderr, "mpv_create failed\n"); return false; }
	const char *want_debug = getenv("PICKLE_LOG_MPV");
	if (want_debug && *want_debug) mpv_request_log_messages(p->handle, "debug"); else mpv_request_log_messages(p->handle, "warn");

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
	r = mpv_set_option_string(p->handle, "vo", vo_req);
	if (r < 0) {
		fprintf(stderr, "[mpv] vo=%s failed (%d); falling back to vo=libmpv\n", vo_req, r);
		vo_req = "libmpv";
		r = mpv_set_option_string(p->handle, "vo", "libmpv");
		log_opt_result("vo=libmpv", r);
	}
	const char *vo_used = vo_req;
	const char *hwdec_pref = getenv("PICKLE_HWDEC");
	if (!hwdec_pref || !*hwdec_pref) hwdec_pref = "auto-safe";
	r = mpv_set_option_string(p->handle, "hwdec", hwdec_pref); log_opt_result("hwdec", r);
	r = mpv_set_option_string(p->handle, "opengl-es", "yes"); log_opt_result("opengl-es=yes", r);
	
	// Video sync mode for better frame timing
	const char *video_sync = g_vsync_enabled ? "display-resample" : "audio";
	r = mpv_set_option_string(p->handle, "video-sync", video_sync);
	log_opt_result("video-sync", r);
	
	// Optimize queue size based on performance mode
	const char *queue_size = g_vsync_enabled ? "4" : "1";  // Reduce queue for lower latency when vsync off
	r = mpv_set_option_string(p->handle, "vo-queue-size", queue_size);
	log_opt_result("vo-queue-size", r);
	
	// Configure loop behavior if enabled
	if (g_loop_playback) {
		r = mpv_set_option_string(p->handle, "loop-file", "inf");
		log_opt_result("loop-file", r);
		r = mpv_set_option_string(p->handle, "loop-playlist", "inf");
		log_opt_result("loop-playlist", r);
	}
	
	// Performance-based cache settings
	if (g_vsync_enabled) {
		// Larger cache for quality when vsync enabled
		r = mpv_set_option_string(p->handle, "demuxer-max-bytes", "64MiB");
		log_opt_result("demuxer-max-bytes", r);
		r = mpv_set_option_string(p->handle, "cache-secs", "10");
		log_opt_result("cache-secs", r);
	} else {
		// Smaller cache for lower latency when performance mode
		r = mpv_set_option_string(p->handle, "demuxer-max-bytes", "16MiB");
		log_opt_result("demuxer-max-bytes (perf)", r);
		r = mpv_set_option_string(p->handle, "cache-secs", "2");
		log_opt_result("cache-secs (perf)", r);
		// Enable aggressive frame dropping for maximum performance
		r = mpv_set_option_string(p->handle, "framedrop", "decoder+vo");
		log_opt_result("framedrop", r);
	}
	
	// Set a larger audio buffer for smoother audio output
	r = mpv_set_option_string(p->handle, "audio-buffer", "0.2");  // 200ms audio buffer
	log_opt_result("audio-buffer", r);
	
	// Prefer using MPV_RENDER_PARAM_FLIP_Y during rendering instead of global rotation

	const char *ctx_override = getenv("PICKLE_GPU_CONTEXT");
	int forced_headless = getenv("PICKLE_FORCE_HEADLESS") ? 1 : 0;
	int headless_attempted = 0;
	if (ctx_override && *ctx_override && strcmp(vo_used, "gpu") == 0) {
		int rc = mpv_set_option_string(p->handle, "gpu-context", ctx_override);
		log_opt_result("gpu-context (override)", rc);
	} else if (strcmp(vo_used, "gpu") == 0) {
		// Always try to avoid DRM contexts that conflict with our own DRM usage
		const char *try_contexts[] = {"x11egl", "waylandvk", "wayland", "x11vk", "displayvk", NULL};
		int ctx_set = 0;
		for (int i = 0; try_contexts[i] && !ctx_set; i++) {
			int rc = mpv_set_option_string(p->handle, "gpu-context", try_contexts[i]);
			if (rc >= 0) {
				fprintf(stderr, "[mpv] Using gpu-context=%s to avoid DRM conflicts\n", try_contexts[i]);
				ctx_set = 1;
				break;
			}
		}
		if (!ctx_set && (forced_headless || (!g_have_master && !getenv("PICKLE_DISABLE_HEADLESS")))) {
			int rc = mpv_set_option_string(p->handle, "gpu-context", "headless");
			if (rc < 0) {
				fprintf(stderr, "[mpv] gpu-context=headless unsupported (%d); will proceed without it.\n", rc);
			} else {
				fprintf(stderr, "[mpv] Using gpu-context=headless (%s).\n", forced_headless?"forced":"auto");
				headless_attempted = 1;
			}
		}
	}
	if (strcmp(vo_used, "gpu") == 0) {
		mpv_set_option_string(p->handle, "terminal", "no");
		mpv_set_option_string(p->handle, "input-default-bindings", "no");
		mpv_set_option_string(p->handle, "input-vo-keyboard", "no");  // Disable mpv's keyboard handling
		mpv_set_option_string(p->handle, "input-cursor", "no");       // Disable cursor handling
		mpv_set_option_string(p->handle, "input-media-keys", "no");   // Disable media key handling
		// Prevent mpv from attempting any DRM/KMS operations since we handle display ourselves
		if (!getenv("PICKLE_KEEP_ATOMIC")) {
			mpv_set_option_string(p->handle, "drm-atomic", "no");
			mpv_set_option_string(p->handle, "drm-mode", "");
			mpv_set_option_string(p->handle, "drm-connector", "");
			mpv_set_option_string(p->handle, "drm-device", "");
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
	if (disable_audio) mpv_set_option_string(p->handle, "audio", "no");
	if (mpv_initialize(p->handle) < 0) { fprintf(stderr, "mpv_initialize failed\n"); return false; }

	mpv_opengl_init_params gl_init = { .get_proc_address = mpv_get_proc_address, .get_proc_address_ctx = NULL };
	mpv_render_param params[4]; memset(params,0,sizeof(params)); int pi=0;
	params[pi].type = MPV_RENDER_PARAM_API_TYPE; params[pi++].data = (void*)MPV_RENDER_API_TYPE_OPENGL;
	params[pi].type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS; params[pi++].data = &gl_init;
	if (use_adv) { params[pi].type = MPV_RENDER_PARAM_ADVANCED_CONTROL; params[pi++].data = (void*)1; }
	params[pi].type = 0;
	fprintf(stderr, "[mpv] Creating render context (advanced_control=%d vo=%s) ...\n", use_adv, vo_used);
	int cr = mpv_render_context_create(&p->render_ctx, p->handle, params);
	if (cr < 0 && strcmp(vo_used, "gpu") == 0 && !forced_headless && !headless_attempted) {
		// Retry once with libmpv fallback for compatibility
		fprintf(stderr, "[mpv] render context create failed (%d); retrying with vo=libmpv\n", cr);
		mpv_terminate_destroy(p->handle); p->handle=NULL; p->render_ctx=NULL;
		p->handle = mpv_create();
		if (!p->handle) { fprintf(stderr, "mpv_create (retry) failed\n"); return false; }
		if (want_debug && *want_debug) mpv_request_log_messages(p->handle, "debug"); else mpv_request_log_messages(p->handle, "warn");
		mpv_set_option_string(p->handle, "vo", "libmpv");
		mpv_set_option_string(p->handle, "hwdec", hwdec_pref);
		if (disable_audio) mpv_set_option_string(p->handle, "audio", "no");
		if (mpv_initialize(p->handle) < 0) { fprintf(stderr, "mpv_initialize (libmpv retry) failed\n"); return false; }
		p->initialized = 1;
		cr = mpv_render_context_create(&p->render_ctx, p->handle, params);
	}
	if (cr < 0) { fprintf(stderr, "mpv_render_context_create failed (%d)\n", cr); return false; }
	fprintf(stderr, "[mpv] Render context OK\n");
	mpv_render_context_set_update_callback(p->render_ctx, on_mpv_events, NULL);
	mpv_set_wakeup_callback(p->handle, mpv_wakeup_cb, NULL);
	const char *cmd[] = {"loadfile", file, NULL};
	if (mpv_command(p->handle, cmd) < 0) { fprintf(stderr, "Failed to load file %s\n", file); return false; }
	
	// Initialize hardware decoder monitoring
	hwdec_monitor_init(&g_hwdec_monitor);
	
	// Optionally analyze H.264 profile if requested
	if (getenv("PICKLE_ANALYZE_VIDEO")) {
		h264_analysis_result_t result = {0};
		if (analyze_h264_profile(file, &result)) {
			if (result.is_h264) {
				printf("[H.264 Analysis] Codec: %s, Format: %s, Resolution: %dx%d, FPS: %.2f\n", 
				       result.codec_name ? result.codec_name : "unknown",
				       result.format_name ? result.format_name : "unknown", 
				       (int)result.width, (int)result.height, result.fps);
				if (!result.hw_compatible) {
					printf("[H.264 Analysis] WARNING: This video may not be hardware accelerated on Raspberry Pi 4\n");
					if (result.compatibility_warning) {
						printf("[H.264 Analysis] Reason: %s\n", result.compatibility_warning);
					}
				}
			}
			// Clean up allocated memory
			free_h264_analysis_result(&result);
		}
	}
	
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
	
	if (p->render_ctx) {
		mpv_render_context_free(p->render_ctx);
		p->render_ctx = NULL;
	}
	
	if (p->handle) {
		mpv_terminate_destroy(p->handle);
		p->handle = NULL;
	}
}

/**
 * Initialize the V4L2 decoder
 * 
 * @param p Pointer to V4L2 player structure
 * @param file Path to the video file to play
 * @return true if initialization succeeded, false otherwise
 */
static bool init_v4l2_decoder(v4l2_player_t *p, const char *file) {
	if (!p) return false;
	
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
	// Try to initialize MP4 demuxer first
	if (mp4_demuxer_init(&p->demuxer, file)) {
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
		
		LOG_INFO("Using MP4 demuxer: %s %dx%d @ %.2f fps", codec_name, p->width, p->height, fps);
	} else {
#endif
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
	
	// Allocate buffers
	if (!v4l2_decoder_allocate_buffers(p->decoder, 8, 8)) {
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
	
	// Allocate read buffer
	p->buffer_size = 1024 * 1024;  // 1MB buffer
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

/**
 * Clean up V4L2 decoder resources
 * 
 * @param p Pointer to V4L2 player structure to clean up
 */
static void destroy_v4l2_decoder(v4l2_player_t *p) {
	if (!p) return;
	
	if (p->buffer) {
		free(p->buffer);
		p->buffer = NULL;
	}
	
#ifdef USE_V4L2_DECODER
	if (p->use_demuxer) {
		mp4_demuxer_cleanup(&p->demuxer);
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

/**
 * Process one frame from the V4L2 decoder
 * 
 * @param p Pointer to V4L2 player structure
 * @return true if processing should continue, false to stop playback
 */
#ifdef USE_V4L2_DECODER
static bool process_v4l2_frame(v4l2_player_t *p) {
	if (!p || !p->is_active) return false;
	
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
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	
#ifdef USE_V4L2_DECODER
	if (p->use_demuxer && p->demuxer.fps > 0) {
		// Calculate target frame interval based on demuxer FPS
		double target_interval_ms = 1000.0 / p->demuxer.fps;
		
		if (last_frame_time.tv_sec != 0) {
			// Calculate time since last frame
			double elapsed_ms = (current_time.tv_sec - last_frame_time.tv_sec) * 1000.0 +
								(current_time.tv_nsec - last_frame_time.tv_nsec) / 1000000.0;
			
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
		
		last_frame_time = current_time;
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
		
		// Only send a packet if we haven't exceeded our limit for this frame
		if (packets_this_frame < 2) { // Allow max 2 packets per frame processing cycle
			mp4_packet_t packet;
		if (mp4_demuxer_get_packet(&p->demuxer, &packet)) {
			LOG_DEBUG("Got elementary stream packet: size=%zu, pts=%lld, keyframe=%s", 
					 packet.size, (long long)packet.pts, packet.is_keyframe ? "yes" : "no");
			
			// Send elementary stream packet to decoder
			if (!v4l2_decoder_decode(p->decoder, packet.data, packet.size, packet.pts)) {
				LOG_ERROR("V4L2 decoder decode failed");
				log_memory_usage("V4L2-DECODE-FAIL");
			} else {
				LOG_DEBUG("Successfully sent %zu bytes (elementary stream) to V4L2 decoder", packet.size);
				packets_this_frame++;
			}				// Free packet data
				mp4_demuxer_free_packet(&packet);
			} else {
				// No more packets available (EOF or error)
				LOG_DEBUG("No more packets from MP4 demuxer");
			}
		} else {
			LOG_DEBUG("Packet rate limiting: skipping packet (sent %d packets this frame)", packets_this_frame);
		}
		
		// Check for stop signal during packet processing
		if (g_stop) {
			LOG_INFO("Stop signal received during packet processing");
			return false;
		}
	} else {
#endif
		// Fallback: raw file reading for elementary streams
		if (p->input_file && !feof(p->input_file)) {
			// Use a safe chunk size for V4L2 decoder to avoid buffer issues
			size_t max_chunk_size = 65536;  // 64KB - safe for most V4L2 drivers
			
			LOG_DEBUG("Using V4L2 chunk size: %zu bytes", max_chunk_size);
			
			// Read a chunk of data, but limit to safe chunk size
			size_t bytes_to_read = max_chunk_size < p->buffer_size ? max_chunk_size : p->buffer_size;
			size_t bytes_read = fread(p->buffer, 1, bytes_to_read, p->input_file);
			LOG_DEBUG("Read %zu bytes from input file", bytes_read);
			
			if (bytes_read > 0) {
				// Send to decoder
				if (!v4l2_decoder_decode(p->decoder, p->buffer, bytes_read, p->timestamp)) {
					LOG_ERROR("V4L2 decoder decode failed");
					log_memory_usage("V4L2-DECODE-FAIL");
				} else {
					LOG_DEBUG("Successfully sent %zu bytes to V4L2 decoder", bytes_read);
				}
				p->timestamp += 40000;  // Increment by 40ms (25fps)
			}
		}
#ifdef USE_V4L2_DECODER
	}
#endif
	
	// Check if we have any decoded frames available
	v4l2_decoded_frame_t frame = {0};
	bool frame_available = v4l2_decoder_get_frame(p->decoder, &frame);
	
	if (frame_available) {
		LOG_DEBUG("Got decoded frame: size=%u, pts=%lld, width=%u, height=%u", 
			frame.bytesused, (long long)frame.timestamp, frame.width, frame.height);
		// Normally we would render the frame here
		got_frame_this_cycle = true;
		no_frame_cycles = 0;  // Reset counter when we get a frame
	} else {
		LOG_DEBUG("No decoded frame available yet");
	}
	
	// Poll for decoded frames with timeout and signal checking
	LOG_DEBUG("Polling V4L2 decoder for frames...");
	int poll_result = v4l2_decoder_poll(p->decoder, 0);  // Non-blocking poll
	LOG_DEBUG("V4L2 poll result: %d", poll_result);
	
	if (poll_result > 0) {
		LOG_DEBUG("Processing V4L2 decoder events...");
		// Process events
		v4l2_decoder_process_events(p->decoder);
		
		// Check for stop signal after processing events
		if (g_stop) {
			LOG_INFO("Stop signal received after processing events");
			return false;
		}
		
		// Try to get a decoded frame
		v4l2_decoded_frame_t decoded_frame;
		LOG_DEBUG("Attempting to get decoded frame...");
		if (v4l2_decoder_get_frame(p->decoder, &decoded_frame)) {
			// Store frame information in the player struct
            p->current_frame.valid = true;
            p->current_frame.dmabuf_fd = decoded_frame.dmabuf_fd;
            p->current_frame.width = decoded_frame.width;
            p->current_frame.height = decoded_frame.height;
            p->current_frame.format = decoded_frame.format;
            p->current_frame.buf_index = decoded_frame.buf_index;
            
            LOG_INFO("Got frame: %dx%d timestamp: %ld, dmabuf_fd: %d", 
                     frame.width, frame.height, frame.timestamp, frame.dmabuf_fd);
            
            // Log memory usage when successfully getting frames
			static int successful_frame_count = 0;
			successful_frame_count++;
			if (successful_frame_count % 50 == 1) {
				log_memory_usage("V4L2-FRAME-SUCCESS");
			}
            
            // Create/update OpenGL texture with frame data
            if (frame.dmabuf_fd >= 0) {
                // We have a DMA-BUF frame, create an EGL texture from it
                // This code would depend on your EGL/DMA-BUF implementation
                // Use the create_dmabuf_texture function if available
                
                // For now, just set the texture ID (placeholder)
                // In a real implementation, you would create an actual texture
                p->current_frame.texture = p->texture;
            } else if (frame.data) {
                // We have memory-mapped data, update the texture
                if (p->texture == 0) {
                    // Create a new texture if we don't have one
                    glGenTextures(1, &p->texture);
                    glBindTexture(GL_TEXTURE_2D, p->texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                } else {
                    // Bind existing texture
                    glBindTexture(GL_TEXTURE_2D, p->texture);
                }
                
                // Update texture with new frame data
                // This would depend on the pixel format
                // For now, assume a simple format like RGB/RGBA
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)frame.width, (GLsizei)frame.height, 
                             0, GL_RGBA, GL_UNSIGNED_BYTE, frame.data);
                
                p->current_frame.texture = p->texture;
            }
            
			return true;
		}
	}
	
	// Check if we've reached the end of input and decoder is empty
#ifdef USE_V4L2_DECODER
	bool input_finished = p->use_demuxer ? p->demuxer.eof_reached : (p->input_file && feof(p->input_file));
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
			LOG_INFO("No frames received for %d consecutive cycles, potential infinite loop - exiting", no_frame_cycles);
			return false;
		}
		if (no_frame_cycles % 100 == 0) {  // Log every 100 cycles
			LOG_DEBUG("No frames for %d consecutive cycles", no_frame_cycles);
		}
	}
	
	return true;
}
#endif // USE_V4L2_DECODER

void drain_mpv_events(mpv_handle *h) {
	while (1) {
		mpv_event *ev = mpv_wait_event(h, 0);
		if (ev->event_id == MPV_EVENT_NONE) break;
		if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
			if (g_debug) fprintf(stderr, "[mpv] VIDEO_RECONFIG\n");
		}
		if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
			mpv_event_log_message *lm = ev->data;
			
			// Check for hardware decoder failures using our monitor
			hwdec_monitor_log_message(lm);
			
			// Only print warnings/errors by default; set PICKLE_LOG_MPV for full detail earlier.
			if (lm->level && (strstr(lm->level, "error") || strstr(lm->level, "warn"))) {
				fprintf(stderr, "[mpv-log] %s: %s", lm->level, lm->text ? lm->text : "\n");
			}
			continue;
		}
		if (ev->event_id == MPV_EVENT_PLAYBACK_RESTART) {
			// This event can indicate that playback is resuming after a pause
			// Mark it as activity to prevent stall detection from triggering
			if (g_debug) fprintf(stderr, "[mpv] PLAYBOOK_RESTART\n");
			gettimeofday(&g_last_frame_time, NULL);
			
			// Check for hardware decoder status changes on restart
			const char *current_file = mpv_get_property_string(h, "path");
			hwdec_monitor_check_failure(&g_hwdec_monitor, h, current_file);
			if (current_file) mpv_free((void*)current_file);
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
#ifdef ENABLE_BORDER_SHADER
static bool init_border_shader();

static bool init_border_shader() {
    const char *g_border_vs_src =
        "attribute vec2 a_position;\n"
        "void main() {\n"
        "  gl_Position = vec4(a_position, 0.0, 1.0);\n"
        "}\n";
    
    const char *g_border_fs_src =
        "precision mediump float;\n"
        "uniform vec4 u_color;\n"
        "void main() {\n"
        "  gl_FragColor = u_color;\n"
        "}\n";

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
#endif

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
#define ARROW_LEFT 'D'

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

int main(int argc, char **argv) {
	// Parse command line options
	static struct option long_options[] = {
		{"loop", no_argument, NULL, 'l'},
		{"help", no_argument, NULL, 'h'},
		{"v4l2", no_argument, NULL, 'v'},
		{"no-vsync", no_argument, NULL, 'n'},
		{"high-performance", no_argument, NULL, 'p'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "lhvnp", long_options, NULL)) != -1) {
		switch (opt) {
			case 'l':
				g_loop_playback = 1;
				break;
			case 'v':
				g_use_v4l2_decoder = 1;
				break;
			case 'n':
				g_vsync_enabled = 0;
				fprintf(stderr, "VSync disabled for maximum framerate\n");
				break;
			case 'p':
				g_vsync_enabled = 0;
				g_triple_buffer = 0;  // Reduce latency
				setenv("PICKLE_FORCE_RENDER_LOOP", "1", 1);  // Force continuous rendering
				fprintf(stderr, "High-performance mode: VSync off, continuous rendering enabled\n");
				break;
			case 'h':
				fprintf(stderr, "Usage: %s [options] <video-file>\n", argv[0]);
				fprintf(stderr, "Options:\n");
				fprintf(stderr, "  -l, --loop            Loop playback continuously\n");
				fprintf(stderr, "  -v, --v4l2            Use V4L2 hardware decoder (RPi4 only)\n");
				fprintf(stderr, "  -n, --no-vsync        Disable VSync for maximum framerate\n");
				fprintf(stderr, "  -p, --high-performance Enable high-performance mode (no VSync, continuous render)\n");
				fprintf(stderr, "  -h, --help            Show this help message\n");
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
	} else {
		fprintf(stderr, "Single playback mode (stall threshold: %dms)\n", g_wd_ongoing_ms);
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
	v4l2_player_t v4l2_player = {0};

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
		keystone_init();
		
		// Initialize hardware HVS keystone if supported
		if (hvs_keystone_is_supported()) {
		if (hvs_keystone_init()) {
			LOG_INFO("Hardware HVS keystone initialized successfully");
		} else {
			LOG_WARN("Failed to initialize hardware HVS keystone, falling back to software implementation");
		}
	} else {
		LOG_INFO("Hardware HVS keystone not supported on this platform, using software implementation");
	}
	
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

	
	// Initialize either MPV or V4L2 decoder based on flag
	if (g_use_v4l2_decoder) {
		// Check if V4L2 decoder is supported
		if (!v4l2_decoder_is_supported()) {
			LOG_ERROR("V4L2 decoder not supported on this platform. Falling back to MPV.");
			g_use_v4l2_decoder = 0;
			if (!init_mpv(&player, file)) RET("init_mpv");
			g_mpv_wakeup = 1;
		} else {
			if (!init_v4l2_decoder(&v4l2_player, file)) RET("init_v4l2_decoder");
		}
	} else {
		if (!init_mpv(&player, file)) RET("init_mpv");
		// Prime event processing in case mpv already queued wakeups before pipe creation.
		g_mpv_wakeup = 1;
	}

	fprintf(stderr, "Playing %s at %dx%d %.2f Hz using %s\n", file, drm.mode.hdisplay, drm.mode.vdisplay,
			(drm.mode.vrefresh ? (double)drm.mode.vrefresh : (double)drm.mode.clock / (drm.mode.htotal * drm.mode.vtotal)),
			g_use_v4l2_decoder ? "V4L2 decoder" : "MPV");
	
	// Print keystone control instructions
	if (g_keystone.enabled) {
		fprintf(stderr, "\nKeystone correction enabled. Controls:\n");
	} else {
		fprintf(stderr, "\nKeystone correction available. Controls:\n");
	}
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
		LOG_INFO("Controller mappings: START=Toggle keystone mode");
		LOG_INFO("Cycle button (default X) = Corners TL->TR->BR->BL");
		LOG_INFO("Help button (default B) = Toggle help overlay");
		LOG_INFO("D-pad/Left stick=Move corners, L1/R1=Decrease/Increase step size");
		LOG_INFO("SELECT=Reset keystone, HOME(Guide)=Toggle border");
		LOG_INFO("START+SELECT (hold 2s)=Quit");
	}
	
#ifdef EVENT_DRIVEN_ENABLED
	// Initialize the event-driven architecture
	event_ctx_t *event_ctx = pickle_event_init(&drm, &player, g_use_v4l2_decoder ? &v4l2_player : NULL);
	if (!event_ctx) {
		LOG_ERROR("Failed to initialize event system");
		goto cleanup;
	}
	LOG_INFO("Event-driven architecture initialized");

	// Main loop using event-driven architecture
	while (!g_stop) {
		if (!pickle_event_process_and_render(event_ctx, &drm, &eglc, &player, 
		                                   g_use_v4l2_decoder ? &v4l2_player : NULL, 100)) {
			break;
		}
		stats_log_periodic(&player);
	}
	
	// Clean up event system
	pickle_event_cleanup(event_ctx);
#else
	// Original polling-based main loop
	while (!g_stop) {
		// Handle decoder-specific events
		if (!g_use_v4l2_decoder) {
			// MPV-specific event handling
			if (g_mpv_wakeup) {
				g_mpv_wakeup = 0;
				drain_mpv_events(player.handle);
				if (player.render_ctx) {
					uint64_t flags = mpv_render_context_update(player.render_ctx);
					g_mpv_update_flags |= flags;
				}
			}
		} else {
			// V4L2 decoder specific handling
			// Periodically force a frame update for V4L2 decoder
			struct timeval now;
			gettimeofday(&now, NULL);
			static struct timeval last_v4l2_update = {0, 0};
			if (last_v4l2_update.tv_sec == 0) {
				last_v4l2_update = now;
			}
			double elapsed = tv_diff(&now, &last_v4l2_update) * 1000.0; // ms
			if (elapsed > 40.0) { // ~25fps
				g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
				last_v4l2_update = now;
			}
		}
		// Check controller quit combo (START+SELECT 2s)
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
			if (!g_help_visible) {
				show_help_overlay(player.handle);
				g_help_visible = 1;
			} else {
				hide_help_overlay(player.handle);
				g_help_visible = 0;
			}
			g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
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
			timeout_ms = 0; // don't block if render pending
		} else if (!g_vsync_enabled) {
			// When vsync is disabled, use very short timeout for maximum fps
			timeout_ms = 1; // 1ms for aggressive polling (up to 1000fps theoretical)
		} else if (frames > 0 && g_vsync_enabled) {
			// Estimate appropriate timeout based on refresh rate for vsync
			double refresh_rate = drm.mode.vrefresh ? 
				(double)drm.mode.vrefresh : 
				(double)drm.mode.clock / (drm.mode.htotal * drm.mode.vtotal);
			
			// Use a more aggressive timeout for better responsiveness
			// Aim for half the frame interval to ensure we don't miss vsync
			if (refresh_rate > 0) {
				timeout_ms = (int)(500.0 / refresh_rate); // half frame time in ms
				
				// Clamp to reasonable bounds
				if (timeout_ms < 4) timeout_ms = 4;       // min 4ms (250fps max)
				if (timeout_ms > 100) timeout_ms = 100;   // max 100ms (10fps min)
			} else {
				timeout_ms = 16; // Default to 60Hz (16.6ms) if refresh rate unknown
			}
		}
		
		// Add a max timeout to avoid being stuck in poll forever even if no events come
		// This allows our watchdog logic to run periodically
		if (timeout_ms < 0) timeout_ms = 100; // max 100ms poll timeout

	int pr = poll(pfds, (nfds_t)n, timeout_ms);
		if (pr < 0) { if (errno == EINTR) continue; fprintf(stderr, "poll failed (%s)\n", strerror(errno)); break; }
		for (int i=0;i<n;i++) {
			if (!(pfds[i].revents & POLLIN)) continue;
			if (pfds[i].fd == drm.fd) {
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
					
					// Special case: Force keystone mode with 'K' (capital K)
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
						if (!g_help_visible) {
							show_help_overlay(player.handle);
							g_help_visible = 1;
						} else {
							hide_help_overlay(player.handle);
							g_help_visible = 0;
						}
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
						continue;
					}

					// Handle keystone adjustment keys first (to avoid 'q' conflict)
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
				while (read(get_joystick_fd(), &event, sizeof(event)) > 0) {
					if (handle_joystick_event(&event)) {
						// Force a redraw when keystone parameters change
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
					}
				}
			}
		}
		if (g_mpv_wakeup) {
			g_mpv_wakeup = 0;
			drain_mpv_events(player.handle);
			if (player.render_ctx) {
				uint64_t flags = mpv_render_context_update(player.render_ctx);
				g_mpv_update_flags |= flags;
			}
		}
		if (g_stop) break;
		int need_frame = 0;
		if (frames == 0 && !g_pending_flip) need_frame = 1; // guarantee first frame submission
		else if (force_loop && !g_pending_flip) need_frame = 1; // continuous mode
		else if ((g_mpv_update_flags & MPV_RENDER_UPDATE_FRAME) && !g_pending_flip) need_frame = 1;

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
				if (player.render_ctx) {
					uint64_t flags = mpv_render_context_update(player.render_ctx);
					g_mpv_update_flags |= flags;
					
					// Reset decoder if needed (for more aggressive recovery)
					if (g_stall_reset_count > 1) {
						// When looping, first check if we're at the end of the file
						if (g_loop_playback) {
							// Get current position and duration
							double pos = 0, duration = 0;
							mpv_get_property(player.handle, "time-pos", MPV_FORMAT_DOUBLE, &pos);
							mpv_get_property(player.handle, "duration", MPV_FORMAT_DOUBLE, &duration);
							
							// If we're near the end, force a restart
							if (duration > 0 && pos > (duration - 1.0)) {
								fprintf(stderr, "[wd] near end of file (%.1f/%.1f), forcing restart for loop\n", pos, duration);
								const char *cmd[] = {"loadfile", mpv_get_property_string(player.handle, "path"), "replace", NULL};
								mpv_command_async(player.handle, 0, cmd);
							} else {
								// Just try to step forward
								const char *cmd[] = {"frame-step", NULL};
								mpv_command_async(player.handle, 0, cmd);
								fprintf(stderr, "[wd] requesting explicit frame-step for recovery\n");
							}
						}
						
						// If we're still having issues, try cycling the hardware decoder
						if (g_stall_reset_count > 2) {
							const char *cmd[] = {"cycle-values", "hwdec", "auto-safe", "no", NULL};
							mpv_command_async(player.handle, 0, cmd);
							fprintf(stderr, "[wd] cycling hwdec as part of recovery\n");
						}
					}
				}
			}
		}
		if (need_frame) {
			if (g_debug && frames < 10) fprintf(stderr, "[debug] rendering frame #%d flags=0x%llx pending_flip=%d\n", frames, (unsigned long long)g_mpv_update_flags, g_pending_flip);
			
			bool render_success = false;
			if (g_use_v4l2_decoder) {
				render_success = render_v4l2_frame(&drm, &eglc, &v4l2_player);
			} else {
				render_success = render_frame_fixed(&drm, &eglc, &player);
			}
			
			if (!render_success) { 
				fprintf(stderr, "Render failed, exiting\n"); 
				break; 
			}
			frames++;
			g_mpv_update_flags &= ~(uint64_t)MPV_RENDER_UPDATE_FRAME;
			if (g_stats_enabled) { g_stats_frames++; stats_log_periodic(&player); }
			gettimeofday(&wd_last_activity, NULL);
			gettimeofday(&g_last_frame_time, NULL); // Update last successful frame time
			
			// Reset stall counter after successful frames
			if (g_stall_reset_count > 0) {
				// Reset stall counter immediately after successful frame render
				fprintf(stderr, "[wd] playback resumed normally, resetting stall counter\n");
				g_stall_reset_count = 0;
			}
		}
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
	
	// Clean up HVS keystone if initialized
	hvs_keystone_cleanup();
	
	// Clean up compute shader keystone if initialized
	compute_keystone_cleanup();
	
	// Clean up keystone resources
	keystone_cleanup();
	
	if (g_use_v4l2_decoder) {
		destroy_v4l2_decoder(&v4l2_player);
	} else {
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
	
	// Clean up HVS keystone if initialized
	hvs_keystone_cleanup();
	
	// Clean up compute shader keystone if initialized
	compute_keystone_cleanup();
	
	// Clean up keystone resources
	keystone_cleanup();
	
	if (g_use_v4l2_decoder) {
		destroy_v4l2_decoder(&v4l2_player);
	} else {
		destroy_mpv(&player);
	}
	deinit_gbm_egl(&eglc);
	deinit_drm(&drm);
	return 1;
}

