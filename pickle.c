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

// Include our refactored modules
#include "utils.h"
#include "shader.h"
#include "keystone.h"
#include "hvs_keystone.h"
#include "drm.h"
#include "egl.h"
#include "v4l2_decoder.h"
#include <execinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <poll.h>
#include <termios.h>
#include <linux/joystick.h>
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

static volatile sig_atomic_t g_stop = 0;
static void handle_sigint(int s){ (void)s; g_stop = 1; }
static void handle_sigsegv(int s){
	(void)s;
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
static fb_ring_t g_fb_ring = {0};
static int g_have_master = 0; // set if we successfully become DRM master
static int g_use_v4l2_decoder = 0; // Use V4L2 decoder instead of MPV's internal decoder

// These keystone settings are defined in keystone.c through pickle_keystone adapter
// Removed static variables and using extern from pickle_keystone.h

static bool g_show_background = false; // Deprecated: background is always black now
static int g_loop_playback = 0; // Whether to loop video playback

// Simple solid-color shader for drawing outlines/borders around keystone quad
static GLuint g_border_shader_program = 0;
static GLuint g_border_vertex_shader = 0;
static GLuint g_border_fragment_shader = 0;
static GLint  g_border_a_position_loc = -1;
static GLint  g_border_u_color_loc = -1;

// Joystick/gamepad support
static int g_joystick_fd = -1;        // File descriptor for joystick
static bool g_joystick_enabled = false; // Whether joystick support is enabled
static char g_joystick_name[128];     // Name of the connected joystick
static int g_selected_corner = 0;     // Currently selected corner (0-3)
static struct timeval g_last_js_event_time = {0}; // For debouncing joystick events

// Gamepad layout for ABXY (xbox vs nintendo)
typedef enum { GP_LAYOUT_AUTO = 0, GP_LAYOUT_XBOX, GP_LAYOUT_NINTENDO } gp_layout_t;
static gp_layout_t g_gamepad_layout = GP_LAYOUT_AUTO;

// Track Start+Select hold for safe quit
static bool g_js_start_down = false;
static bool g_js_select_down = false;
static struct timeval g_js_start_time = {0};
static struct timeval g_js_select_time = {0};
static bool g_js_quit_fired = false;

// 8BitDo controller button mappings (may vary by model/mode)
#define JS_BUTTON_A        0
#define JS_BUTTON_B        1
#define JS_BUTTON_X        2
#define JS_BUTTON_Y        3
#define JS_BUTTON_L1       4
#define JS_BUTTON_R1       5
#define JS_BUTTON_SELECT   6
#define JS_BUTTON_START    7
#define JS_BUTTON_HOME     8
#define JS_BUTTON_L3       9
#define JS_BUTTON_R3       10

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

// Optional explicit ABXY mapping via env (decl after macros to use their defaults)
static int g_btn_code_X = JS_BUTTON_X;
static int g_btn_code_A = JS_BUTTON_A;
static int g_btn_code_B = JS_BUTTON_B;
static int g_btn_code_Y = JS_BUTTON_Y;
// Corner indices: 0=TL,1=TR,2=BL,3=BR
static int g_corner_for_X = 0; // X->TL
static int g_corner_for_A = 1; // A->TR
static int g_corner_for_B = 3; // B->BR
static int g_corner_for_Y = 2; // Y->BL
static bool g_use_label_mapping = false;
static int g_x_cycle_enabled = 1; // default: X cycles corners TL->TR->BR->BL
static int g_cycle_button_code = JS_BUTTON_X; // which button number cycles corners
static int g_help_button_code = JS_BUTTON_B;  // which button number toggles help
static int g_help_toggle_request = 0;         // raised by joystick, handled in main loop

static int parse_corner_token(const char *t) {
	if (!t) return -1;
	if (!strcasecmp(t, "TL")) return 0;
	if (!strcasecmp(t, "TR")) return 1;
	if (!strcasecmp(t, "BL")) return 2;
	if (!strcasecmp(t, "BR")) return 3;
	return -1;
}

static void parse_btn_code_env(void) {
	const char *s = getenv("PICKLE_BTN_CODE");
	if (!s || !*s) return;
	char buf[128]; strncpy(buf, s, sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
	for (char *p = buf; *p; ++p) if (*p == ';') *p = ',';
	char *saveptr = NULL; char *tok = strtok_r(buf, ", ", &saveptr);
	while (tok) {
		char key[8]={0}; int val=-1;
		if (sscanf(tok, "%7[^=]=%d", key, &val) == 2) {
			if (!strcasecmp(key, "X")) g_btn_code_X = val;
			else if (!strcasecmp(key, "A")) g_btn_code_A = val;
			else if (!strcasecmp(key, "B")) g_btn_code_B = val;
			else if (!strcasecmp(key, "Y")) g_btn_code_Y = val;
		}
		tok = strtok_r(NULL, ", ", &saveptr);
	}
}

static void parse_corner_map_env(void) {
	const char *s = getenv("PICKLE_CORNER_MAP");
	if (!s || !*s) return;
	char buf[128]; strncpy(buf, s, sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
	for (char *p = buf; *p; ++p) if (*p == ';') *p = ',';
	char *saveptr = NULL; char *tok = strtok_r(buf, ", ", &saveptr);
	while (tok) {
		char key[8]={0}, val[8]={0};
		if (sscanf(tok, "%7[^=]=%7s", key, val) == 2) {
			int corner = parse_corner_token(val);
			if (corner >= 0) {
				if (!strcasecmp(key, "X")) g_corner_for_X = corner;
				else if (!strcasecmp(key, "A")) g_corner_for_A = corner;
				else if (!strcasecmp(key, "B")) g_corner_for_B = corner;
				else if (!strcasecmp(key, "Y")) g_corner_for_Y = corner;
			}
		}
		tok = strtok_r(NULL, ", ", &saveptr);
	}
}

static void setup_label_mapping(void) {
	parse_btn_code_env();
	parse_corner_map_env();
	const char *use = getenv("PICKLE_USE_LABEL_MAPPING");
	if (use && *use && atoi(use) != 0) g_use_label_mapping = true;
	const char *xc = getenv("PICKLE_X_CYCLE");
	if (xc && *xc) g_x_cycle_enabled = (atoi(xc) != 0);
	if (g_use_label_mapping) {
		LOG_INFO("Using explicit ABXY mapping: codes X=%d A=%d B=%d Y=%d; corners X=%d A=%d B=%d Y=%d",
				 g_btn_code_X, g_btn_code_A, g_btn_code_B, g_btn_code_Y,
				 g_corner_for_X, g_corner_for_A, g_corner_for_B, g_corner_for_Y);
	}
	LOG_INFO("X button cycling: %s (PICKLE_X_CYCLE=%s)", g_x_cycle_enabled ? "enabled" : "disabled", xc && *xc ? xc : "(default)");
}

static int label_to_code_default(const char *label) {
	if (!label) return -1;
	if (!strcasecmp(label, "X")) return JS_BUTTON_X;
	if (!strcasecmp(label, "A")) return JS_BUTTON_A;
	if (!strcasecmp(label, "B")) return JS_BUTTON_B;
	if (!strcasecmp(label, "Y")) return JS_BUTTON_Y;
	return -1;
}

static void configure_special_buttons(void) {
	// Defaults based on layout or explicit label mapping
	if (g_use_label_mapping) {
		g_cycle_button_code = g_btn_code_X;
		g_help_button_code = g_btn_code_B;
	} else {
		if (g_gamepad_layout == GP_LAYOUT_NINTENDO) {
			// Typical Nintendo-style mapping: B=0, A=1, Y=2, X=3
			g_cycle_button_code = 3; // physical X
			g_help_button_code = 0;  // physical B
		} else {
			// Xbox-style default mapping
			g_cycle_button_code = JS_BUTTON_X;
			g_help_button_code = JS_BUTTON_B;
		}
	}

	// Env overrides: numeric or label
	const char *cb = getenv("PICKLE_CYCLE_BUTTON");
	if (cb && *cb) {
		char *end=NULL; long v = strtol(cb, &end, 10);
		if (end && *end=='\0') g_cycle_button_code = (int)v; else {
			int code = g_use_label_mapping ?
				(!strcasecmp(cb,"X")?g_btn_code_X:!strcasecmp(cb,"A")?g_btn_code_A:!strcasecmp(cb,"B")?g_btn_code_B:!strcasecmp(cb,"Y")?g_btn_code_Y:-1)
				: label_to_code_default(cb);
			if (code >= 0) g_cycle_button_code = code;
		}
	}
    
	const char *hb = getenv("PICKLE_HELP_BUTTON");
	if (hb && *hb) {
		char *end=NULL; long v = strtol(hb, &end, 10);
		if (end && *end=='\0') g_help_button_code = (int)v; else {
			int code = g_use_label_mapping ?
				(!strcasecmp(hb,"X")?g_btn_code_X:!strcasecmp(hb,"A")?g_btn_code_A:!strcasecmp(hb,"B")?g_btn_code_B:!strcasecmp(hb,"Y")?g_btn_code_Y:-1)
				: label_to_code_default(hb);
			if (code >= 0) g_help_button_code = code;
		}
	}

	LOG_INFO("Cycle button code=%d%s, Help button code=%d%s",
			 g_cycle_button_code, (cb&&*cb)?" (env)":"",
			 g_help_button_code, (hb&&*hb)?" (env)":"");
}

// Forward declarations
static bool init_joystick(void);
static bool handle_joystick_event(struct js_event *event);

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
typedef struct {
	mpv_handle *mpv;             // MPV API handle
	mpv_render_context *rctx;    // MPV render context for OpenGL rendering
	int using_libmpv;            // Flag indicating fallback to vo=libmpv occurred
} mpv_player_t;

// V4L2 decoder integration
typedef struct {
	v4l2_decoder_t *decoder;     // V4L2 decoder instance
	v4l2_codec_t codec;          // Codec being used
	uint32_t width;              // Video width
	uint32_t height;             // Video height
	int is_active;               // Flag indicating decoder is active
	FILE *input_file;            // Input file handle
	uint8_t *buffer;             // Buffer for reading file data
	size_t buffer_size;          // Size of the buffer
	int64_t timestamp;           // Current timestamp
	GLuint texture;              // OpenGL texture for rendering
} v4l2_player_t;

// Wakeup callback sets a flag so main loop knows mpv wants processing.
static volatile int g_mpv_wakeup = 0;
static int g_mpv_pipe[2] = {-1,-1}; // pipe to integrate mpv wakeups into poll loop
static void mpv_wakeup_cb(void *ctx) {
	(void)ctx;
	g_mpv_wakeup = 1;
	if (g_mpv_pipe[1] >= 0) {
		unsigned char b = 0;
		if (write(g_mpv_pipe[1], &b, 1) < 0) { /* ignore EAGAIN */ }
	}
}
static volatile uint64_t g_mpv_update_flags = 0; // bitmask from mpv_render_context_update
static void on_mpv_events(void *data) { (void)data; g_mpv_wakeup = 1; }

// Performance controls
static int g_triple_buffer = 1;         // Enable triple buffering by default
static int g_vsync_enabled = 1;         // Enable vsync by default
static int g_frame_timing_enabled = 0;  // Detailed frame timing metrics (when PICKLE_TIMING=1)

// Texture orientation controls (used in keystone pass only)
static int g_tex_flip_x = 0; // 1 = mirror horizontally (left/right)
static int g_tex_flip_y = 0; // 1 = flip vertically (top/bottom)
static int g_help_visible = 0; // toggle state for help overlay

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
	if (p && p->mpv) {
		mpv_get_property(p->mpv, "drop-frame-count", MPV_FORMAT_INT64, &drop_dec);
		mpv_get_property(p->mpv, "vo-drop-frame-count", MPV_FORMAT_INT64, &drop_vo);
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
static void show_help_overlay(mpv_handle *mpv) {
	if (!mpv) return;
	const char *text =
		"Pickle controls:\n"
		"  q: quit    h: help overlay\n"
		"  k: toggle keystone    1-4: select corner\n"
		"  arrows / WASD: move point\n"
		"  +/-: step    r: reset\n"
		"  b: toggle border    [ / ]: border width\n"
	"  c: toggle corner markers\n"
		"  o: flip X (mirror)  p: flip Y (invert)\n"
		"  m: mesh mode (experimental)\n"
		"  S: save keystone\n"
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

static void hide_help_overlay(mpv_handle *mpv) {
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
	
	// Set a higher frame queue size for smoother playback
	r = mpv_set_option_string(p->mpv, "vo-queue-size", "4");
	log_opt_result("vo-queue-size", r);
	
	// Configure loop behavior if enabled
	if (g_loop_playback) {
		r = mpv_set_option_string(p->mpv, "loop-file", "inf");
		log_opt_result("loop-file", r);
		r = mpv_set_option_string(p->mpv, "loop-playlist", "inf");
		log_opt_result("loop-playlist", r);
	}
	// Increase demuxer cache for smoother playback
	r = mpv_set_option_string(p->mpv, "demuxer-max-bytes", "64MiB");
	log_opt_result("demuxer-max-bytes", r);
	
	// Optimize for smoother playback over perfect A/V sync
	r = mpv_set_option_string(p->mpv, "cache-secs", "10");
	log_opt_result("cache-secs", r);
	
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
	mpv_render_context_set_update_callback(p->rctx, on_mpv_events, NULL);
	mpv_set_wakeup_callback(p->mpv, mpv_wakeup_cb, NULL);
	const char *cmd[] = {"loadfile", file, NULL};
	if (mpv_command(p->mpv, cmd) < 0) { fprintf(stderr, "Failed to load file %s\n", file); return false; }
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
	
	// Open the input file
	p->input_file = fopen(file, "rb");
	if (!p->input_file) {
		LOG_ERROR("Failed to open input file: %s", file);
		free(p->decoder);
		p->decoder = NULL;
		return false;
	}
	
	// For now, assume H.264 codec - we would need to parse the file header to determine this
	p->codec = V4L2_CODEC_H264;
	p->width = 1920;  // Default values, will be updated after parsing
	p->height = 1080;
	
	// Initialize the decoder
	if (!v4l2_decoder_init(p->decoder, p->codec, p->width, p->height)) {
		LOG_ERROR("Failed to initialize V4L2 decoder");
		fclose(p->input_file);
		free(p->decoder);
		p->decoder = NULL;
		return false;
	}
	
	// Set up DMA-BUF for zero-copy
	if (!v4l2_decoder_use_dmabuf(p->decoder)) {
		LOG_WARN("DMA-BUF not supported, falling back to memory copy");
	}
	
	// Allocate buffers
	if (!v4l2_decoder_allocate_buffers(p->decoder, 8, 8)) {
		LOG_ERROR("Failed to allocate V4L2 decoder buffers");
		v4l2_decoder_destroy(p->decoder);
		fclose(p->input_file);
		free(p->decoder);
		p->decoder = NULL;
		return false;
	}
	
	// Start the decoder
	if (!v4l2_decoder_start(p->decoder)) {
		LOG_ERROR("Failed to start V4L2 decoder");
		v4l2_decoder_destroy(p->decoder);
		fclose(p->input_file);
		free(p->decoder);
		p->decoder = NULL;
		return false;
	}
	
	// Allocate read buffer
	p->buffer_size = 64 * 1024;  // 64KB buffer
	p->buffer = malloc(p->buffer_size);
	if (!p->buffer) {
		LOG_ERROR("Failed to allocate read buffer");
		v4l2_decoder_destroy(p->decoder);
		fclose(p->input_file);
		free(p->decoder);
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
static bool process_v4l2_frame(v4l2_player_t *p) {
	if (!p || !p->is_active) return false;
	
	// Feed data to the decoder if needed
	if (p->input_file && !feof(p->input_file)) {
		// Read a chunk of data
		size_t bytes_read = fread(p->buffer, 1, p->buffer_size, p->input_file);
		if (bytes_read > 0) {
			// Send to decoder
			if (!v4l2_decoder_decode(p->decoder, p->buffer, bytes_read, p->timestamp)) {
				LOG_ERROR("V4L2 decoder decode failed");
			}
			p->timestamp += 40000;  // Increment by 40ms (25fps)
		}
	}
	
	// Poll for decoded frames
	if (v4l2_decoder_poll(p->decoder, 0)) {
		// Process events
		v4l2_decoder_process_events(p->decoder);
		
		// Try to get a decoded frame
		v4l2_decoded_frame_t frame;
		if (v4l2_decoder_get_frame(p->decoder, &frame)) {
			// Process the frame - in a real implementation, we would
			// create/update OpenGL textures using the DMA-BUF or mapped memory
			LOG_INFO("Got frame: %dx%d timestamp: %ld", frame.width, frame.height, frame.timestamp);
			
			// TODO: Update OpenGL texture with frame data
			
			// Return the frame to the decoder
			// In a real implementation, this would happen after rendering
			return true;
		}
	}
	
	// Check if we've reached the end of the file and decoder is empty
	if (feof(p->input_file) && p->decoder) {
		// TODO: Check if decoder has any more buffered frames
		// For now, just continue until we get no more frames
	}
	
	return true;
}

static void drain_mpv_events(mpv_handle *h) {
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
static int g_pending_flip = 0; // set after scheduling page flip until event handler fires
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data) {
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
static bool init_border_shader();

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

/**
 * Initialize joystick/gamepad support
 * Attempts to open the first joystick device and set up event handling
 * 
 * @return true if a joystick was found and initialized
 */
static bool init_joystick(void) {
    // Try to open joystick device
    const char *device = "/dev/input/js0";
    g_joystick_fd = open(device, O_RDONLY | O_NONBLOCK);
    
    if (g_joystick_fd < 0) {
        LOG_WARN("Could not open joystick at %s: %s", device, strerror(errno));
        return false;
    }
    
    // Get joystick name
    if (ioctl(g_joystick_fd, JSIOCGNAME(sizeof(g_joystick_name)), g_joystick_name) < 0) {
        strcpy(g_joystick_name, "Unknown Controller");
    }
    
    LOG_INFO("Joystick initialized: %s", g_joystick_name);
    g_joystick_enabled = true;
    
    // Initialize the first corner as selected
    g_selected_corner = 0;

	// Determine gamepad layout
	const char *layout_env = getenv("PICKLE_GAMEPAD_LAYOUT");
	if (layout_env && *layout_env) {
		if (!strcasecmp(layout_env, "xbox")) g_gamepad_layout = GP_LAYOUT_XBOX;
		else if (!strcasecmp(layout_env, "nintendo")) g_gamepad_layout = GP_LAYOUT_NINTENDO;
		else g_gamepad_layout = GP_LAYOUT_AUTO;
	} else {
		// Heuristic: prefer Nintendo layout for 8BitDo Zero or Nintendo devices
		if (strstr(g_joystick_name, "Nintendo") || strstr(g_joystick_name, "Zero")) {
			g_gamepad_layout = GP_LAYOUT_NINTENDO;
		} else {
			g_gamepad_layout = GP_LAYOUT_XBOX;
		}
	}
	LOG_INFO("Gamepad layout: %s", (g_gamepad_layout==GP_LAYOUT_NINTENDO?"nintendo":(g_gamepad_layout==GP_LAYOUT_XBOX?"xbox":"auto")));
    
	// Apply optional explicit ABXY mapping from environment (takes precedence for ABXY selection)
	setup_label_mapping();
	// Configure which buttons perform cycle and help based on layout/env
	configure_special_buttons();
    
    return true;
}

/**
 * Clean up joystick resources
 */
static void cleanup_joystick(void) {
    if (g_joystick_fd >= 0) {
        close(g_joystick_fd);
        g_joystick_fd = -1;
    }
    g_joystick_enabled = false;
}

/**
 * Process a joystick event for keystone control
 * Maps 8BitDo controller buttons to keystone adjustment actions
 * 
 * @param event The joystick event to process
 * @return true if the event was handled and resulted in a keystone adjustment
 */
static bool handle_joystick_event(struct js_event *event) {
    // Debounce to prevent too many events
    struct timeval now;
    gettimeofday(&now, NULL);
    long time_diff_ms = (now.tv_sec - g_last_js_event_time.tv_sec) * 1000 + 
                       (now.tv_usec - g_last_js_event_time.tv_usec) / 1000;
    
    // Require 100ms between events for buttons, 250ms for analog sticks
    int min_ms = (event->type == JS_EVENT_BUTTON) ? 100 : 250;
    if (time_diff_ms < min_ms) {
        return false;
    }
    
    // Track timestamp for debouncing
    g_last_js_event_time = now;
    
    // Skip initial state events sent when joystick is first opened
    if (event->type & JS_EVENT_INIT) {
        return false;
    }
    
	// Handle button events
	if (event->type == JS_EVENT_BUTTON) {
		// Track Start/Select state for quit combo
		if (event->number == JS_BUTTON_START) {
			if (event->value == 1) { g_js_start_down = true; gettimeofday(&g_js_start_time, NULL); }
			else if (event->value == 0) { g_js_start_down = false; }
		} else if (event->number == JS_BUTTON_SELECT) {
			if (event->value == 1) { g_js_select_down = true; gettimeofday(&g_js_select_time, NULL); }
			else if (event->value == 0) { g_js_select_down = false; }
		}

		// If keystone enabled and cycle button is pressed, optionally cycle corners TL->TR->BR->BL
		if (event->value == 1 && g_keystone.enabled && g_x_cycle_enabled && event->number == g_cycle_button_code) {
			int order[4] = {0,1,2,3}; // TL,TR,BL,BR -> next
			int cur = g_keystone.active_corner;
			if (cur < 0) cur = g_selected_corner >= 0 ? g_selected_corner : 0;
			int idx = 0;
			for (int i=0;i<4;i++) if (order[i] == cur) { idx = i; break; }
			int next = order[(idx+1)&3];
			g_keystone.active_corner = next;
			g_selected_corner = next;
			static const char *names[4] = { "Top-left", "Top-right", "Bottom-left", "Bottom-right" };
			LOG_INFO("Cycling to corner %d (%s) via button %d", next+1, names[next], event->number);
			return true;
		}

		// Help button: toggle help overlay via main-loop request (safe access to player.mpv)
		if (event->value == 1 && event->number == g_help_button_code) {
			g_help_toggle_request = 1;
			LOG_INFO("Help toggle requested via button %d", event->number);
			return true;
		}

		// Y button toggles border (layout/env adapt)
		if (event->value == 1) {
			int y_code = g_use_label_mapping ? g_btn_code_Y : (g_gamepad_layout == GP_LAYOUT_NINTENDO ? 2 : JS_BUTTON_Y);
			if (event->number == y_code) {
				if (g_keystone.enabled) {
					g_show_border = !g_show_border;
					LOG_INFO("Border %s (via Y)", g_show_border ? "enabled" : "disabled");
					return true;
				}
			}
		}

		// If using explicit label mapping from environment, handle ABXY selection first
		if (event->value == 1 && g_keystone.enabled && g_use_label_mapping) {
			int corner = -1;
			if (event->number == g_btn_code_X) corner = g_corner_for_X;
			else if (event->number == g_btn_code_A) corner = g_corner_for_A;
			else if (event->number == g_btn_code_B) corner = g_corner_for_B;
			else if (event->number == g_btn_code_Y) corner = g_corner_for_Y;
			if (corner >= 0 && corner <= 3) {
				g_keystone.active_corner = corner;
				g_selected_corner = corner;
				static const char *names[4] = { "Top-left", "Top-right", "Bottom-left", "Bottom-right" };
				LOG_INFO("Adjusting corner %d (%s) [env mapping]", g_keystone.active_corner + 1, names[corner]);
				return true;
			}
		}

		if (event->value == 1) {  // Button pressed
		switch (event->number) {
            case JS_BUTTON_START:  // Toggle keystone mode
                if (!g_keystone.enabled) {
                    g_keystone.enabled = true;
                    g_keystone.active_corner = g_selected_corner;
                    keystone_update_matrix();
                    LOG_INFO("Keystone correction enabled, adjusting corner %d", g_keystone.active_corner + 1);
                } else {
                    g_keystone.enabled = false;
                    g_keystone.active_corner = -1;
                    LOG_INFO("Keystone correction disabled");
                }
                return true;
                
			case JS_BUTTON_X:  // Select top-left (Nintendo layout puts X at top)
				if (g_keystone.enabled) {
					int corner = (g_gamepad_layout == GP_LAYOUT_NINTENDO) ? 0 : 2; // Xbox X is left -> BL
					g_keystone.active_corner = corner;
					g_selected_corner = corner;
					LOG_INFO("Adjusting corner %d (Top-left)", g_keystone.active_corner + 1);
					return true;
				}
				break;

			case JS_BUTTON_A:  // Select top-right (Nintendo A is right)
				if (g_keystone.enabled) {
					int corner = (g_gamepad_layout == GP_LAYOUT_NINTENDO) ? 1 : 0; // Xbox A is bottom -> TL
					g_keystone.active_corner = corner;
					g_selected_corner = corner;
					LOG_INFO("Adjusting corner %d (Top-right)", g_keystone.active_corner + 1);
					return true;
				}
				break;

			case JS_BUTTON_B:  // Select bottom-right (Nintendo B is bottom)
				if (g_keystone.enabled) {
					int corner = (g_gamepad_layout == GP_LAYOUT_NINTENDO) ? 3 : 1; // Xbox B is right -> TR
					g_keystone.active_corner = corner;
					g_selected_corner = corner;
					LOG_INFO("Adjusting corner %d (Bottom-right)", g_keystone.active_corner + 1);
					return true;
				}
				break;

			case JS_BUTTON_Y:  // Select bottom-left (Nintendo Y is left)
				if (g_keystone.enabled) {
					int corner = (g_gamepad_layout == GP_LAYOUT_NINTENDO) ? 2 : 3; // Xbox Y is top -> BR
					g_keystone.active_corner = corner;
					g_selected_corner = corner;
					LOG_INFO("Adjusting corner %d (Bottom-left)", g_keystone.active_corner + 1);
					return true;
				}
				break;
                
            case JS_BUTTON_SELECT:  // Reset keystone to default
                if (g_keystone.enabled) {
                    // Save the current enabled state
                    bool was_enabled = g_keystone.enabled;
                    
                    // Reset to default corner positions
                    g_keystone.points[0][0] = 0.0f; g_keystone.points[0][1] = 0.0f; // Top-left
                    g_keystone.points[1][0] = 1.0f; g_keystone.points[1][1] = 0.0f; // Top-right
                    g_keystone.points[2][0] = 1.0f; g_keystone.points[2][1] = 1.0f; // Bottom-right
                    g_keystone.points[3][0] = 0.0f; g_keystone.points[3][1] = 1.0f; // Bottom-left
                    
                    // Restore the enabled state
                    g_keystone.enabled = was_enabled;
                    
                    // Update the transformation matrix with the new corner positions
                    keystone_update_matrix();
                    
                    LOG_INFO("Keystone reset to default rectangle");
                    return true;
                }
                break;
                
			case JS_BUTTON_L1:  // Decrease adjustment step
                if (g_keystone.enabled) {
					g_keystone_adjust_step = (g_keystone_adjust_step / 2 < 1) ? 1 : (g_keystone_adjust_step / 2);
                    LOG_INFO("Keystone step decreased to %d", g_keystone_adjust_step);
                    return true;
                }
                break;
                
			case JS_BUTTON_R1:  // Increase adjustment step
                if (g_keystone.enabled) {
					g_keystone_adjust_step = (g_keystone_adjust_step * 2 > 100) ? 100 : (g_keystone_adjust_step * 2);
                    LOG_INFO("Keystone step increased to %d", g_keystone_adjust_step);
                    return true;
                }
                break;
                
            case JS_BUTTON_HOME:  // Toggle border
                if (g_keystone.enabled) {
                    g_show_border = !g_show_border;
                    LOG_INFO("Border %s", g_show_border ? "enabled" : "disabled");
                    return true;
                }
                break;

			// D-Pad as buttons: move selected corner. Useful for 8BitDo Zero 2.
			case JS_BUTTON_DPAD_LEFT:
				if (g_keystone.enabled) {
					float step = (float)g_keystone_adjust_step / 1000.0f;
					keystone_adjust_corner(g_keystone.active_corner, -step, 0.0f);
					LOG_INFO("Moving corner %d left (dpad button)", g_keystone.active_corner + 1);
					return true;
				}
				break;
			case JS_BUTTON_DPAD_RIGHT:
				if (g_keystone.enabled) {
					float step = (float)g_keystone_adjust_step / 1000.0f;
					keystone_adjust_corner(g_keystone.active_corner, step, 0.0f);
					LOG_INFO("Moving corner %d right (dpad button)", g_keystone.active_corner + 1);
					return true;
				}
				break;
			case JS_BUTTON_DPAD_UP:
				if (g_keystone.enabled) {
					float step = (float)g_keystone_adjust_step / 1000.0f;
					keystone_adjust_corner(g_keystone.active_corner, 0.0f, -step);
					LOG_INFO("Moving corner %d up (dpad button)", g_keystone.active_corner + 1);
					return true;
				}
				break;
			case JS_BUTTON_DPAD_DOWN:
				if (g_keystone.enabled) {
					float step = (float)g_keystone_adjust_step / 1000.0f;
					keystone_adjust_corner(g_keystone.active_corner, 0.0f, step);
					LOG_INFO("Moving corner %d down (dpad button)", g_keystone.active_corner + 1);
					return true;
				}
				break;

			default:
				if (g_debug) {
					LOG_DEBUG("Joystick button %u pressed (unmapped)", (unsigned)event->number);
				}
				break;
		}}
    }
    
    // Handle axis events (D-pad and analog sticks)
    else if (event->type == JS_EVENT_AXIS) {
        // Only process if keystone is enabled
        if (!g_keystone.enabled) {
            return false;
        }
        
        float step = (float)g_keystone_adjust_step / 1000.0f; // Convert to 0-1 range
        
        // D-pad or left analog stick
        if ((event->number == JS_AXIS_DPAD_X || event->number == JS_AXIS_LEFT_X) && abs(event->value) > 16384) {
            if (event->value < 0) {  // Left
                keystone_adjust_corner(g_keystone.active_corner, -step, 0.0f);
                LOG_INFO("Moving corner %d left", g_keystone.active_corner + 1);
                return true;
            } else {  // Right
                keystone_adjust_corner(g_keystone.active_corner, step, 0.0f);
                LOG_INFO("Moving corner %d right", g_keystone.active_corner + 1);
                return true;
            }
        }
        else if ((event->number == JS_AXIS_DPAD_Y || event->number == JS_AXIS_LEFT_Y) && abs(event->value) > 16384) {
            if (event->value < 0) {  // Up
                keystone_adjust_corner(g_keystone.active_corner, 0.0f, -step);
                LOG_INFO("Moving corner %d up", g_keystone.active_corner + 1);
                return true;
            } else {  // Down
                keystone_adjust_corner(g_keystone.active_corner, 0.0f, step);
                LOG_INFO("Moving corner %d down", g_keystone.active_corner + 1);
                return true;
            }
        }
    }
    
    return false;
}

// Removed const from drm_ctx parameter because drmModeSetCrtc expects a non-const drmModeModeInfoPtr.
// We cache framebuffer IDs per gbm_bo to avoid per-frame AddFB/RmFB churn.
// user data holds a small struct with fb id + drm fd and a destroy handler.
static int g_scanout_disabled = 0; // when set, we skip page flips/modeset and just let mpv decode & render offscreen
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
 * Render a frame using the V4L2 decoder
 * 
 * @param d Pointer to KMS context
 * @param e Pointer to EGL context
 * @param p Pointer to V4L2 player structure
 * @return true if rendering succeeded, false otherwise
 */
static bool render_v4l2_frame(kms_ctx_t *d, egl_ctx_t *e, v4l2_player_t *p) {
	if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
		fprintf(stderr, "eglMakeCurrent failed\n"); return false; 
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
	process_v4l2_frame(p);
	
	// TODO: Create/update OpenGL texture from V4L2 decoded frame
	// For now, just show a black screen with border if enabled
	
	// Apply keystone correction if enabled
	if (g_keystone.enabled && g_keystone_shader_program) {
		// Render keystone-corrected quad (similar to render_frame_fixed)
		// For now, just rendering a black screen with keystone shape
		glUseProgram(g_keystone_shader_program);
		// Set keystone uniforms...
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
		fprintf(stderr, "Failed to find framebuffer for BO\n");
		gbm_surface_release_buffer(e->gbm_surf, bo);
		return false;
	}
	
	// Present the framebuffer using KMS
	bool ret = true;
	if (d->atomic_supported) {
		ret = atomic_present_framebuffer(d, fb_id, g_vsync_enabled);
	} else {
		drmModePageFlip(d->fd, d->crtc_id, fb_id, g_vsync_enabled ? DRM_MODE_PAGE_FLIP_EVENT : 0, d);
	}
	
	// Wait for page flip to complete if using vsync
	if (g_vsync_enabled) {
		wait_for_flip(d->fd);
	}
	
	// Release the buffer
	gbm_surface_release_buffer(e->gbm_surf, bo);
	
	// Release the buffer
	gbm_surface_release_buffer(e->gbm_surf, bo);
	
	return ret;
}

static bool render_frame_fixed(kms_ctx_t *d, egl_ctx_t *e, mpv_player_t *p) {
	if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
		fprintf(stderr, "eglMakeCurrent failed\n"); return false; 
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
	
	// Ensure reusable FBO exists when keystone is enabled, sized to current mode
	if (g_keystone.enabled) {
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
			// Create texture
			glGenTextures(1, &g_keystone_fbo_texture);
			glBindTexture(GL_TEXTURE_2D, g_keystone_fbo_texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, want_w, want_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			// Create FBO
			glGenFramebuffers(1, &g_keystone_fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, g_keystone_fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_keystone_fbo_texture, 0);
			GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE) {
				LOG_ERROR("FBO setup failed, status: %d", status);
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
	if (g_keystone.enabled && g_keystone_fbo) {
		glBindFramebuffer(GL_FRAMEBUFFER, g_keystone_fbo);
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
	
	// Render the mpv frame
	mpv_render_context_render(p->rctx, r_params);
	
	// If keystone is enabled, render the FBO texture with our shader
	if (g_keystone.enabled && g_keystone_fbo && g_keystone_fbo_texture) {
		// Switch back to default framebuffer
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		
		// Use our shader program
		glUseProgram(g_keystone_shader_program);
		
		// Set up texture
		glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g_keystone_fbo_texture);
		glUniform1i(g_keystone_u_texture_loc, 0);
		
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
		
		// Bind and set vertex positions
		glBindBuffer(GL_ARRAY_BUFFER, g_keystone_vertex_buffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
		glVertexAttribPointer((GLuint)g_keystone_a_position_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
		
		// We need another VBO for texture coordinates
		if (g_keystone_texcoord_buffer == 0) {
			glGenBuffers(1, &g_keystone_texcoord_buffer);
		}
		
		// Bind and set texture coordinates
		glBindBuffer(GL_ARRAY_BUFFER, g_keystone_texcoord_buffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_DYNAMIC_DRAW);
		glEnableVertexAttribArray((GLuint)g_keystone_a_texcoord_loc);
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
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		
		// Clean up
		glDisableVertexAttribArray((GLuint)g_keystone_a_position_loc);
		glDisableVertexAttribArray((GLuint)g_keystone_a_texcoord_loc);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glUseProgram(0);
	}
	
	// Draw border around the keystone quad if enabled
	if (g_show_border) {
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
		// Use border shader
		glUseProgram(g_border_shader_program);
		glUniform4f(g_border_u_color_loc, 1.0f, 1.0f, 0.0f, 1.0f); // Yellow
	// Upload a tiny VBO on existing vertex buffer to avoid creating a new one
		if (g_keystone_vertex_buffer == 0) glGenBuffers(1, &g_keystone_vertex_buffer);
		glBindBuffer(GL_ARRAY_BUFFER, g_keystone_vertex_buffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(lines), lines, GL_DYNAMIC_DRAW);
		glEnableVertexAttribArray((GLuint)g_border_a_position_loc);
		glVertexAttribPointer((GLuint)g_border_a_position_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
	// Set line width (may be clamped to 1 on some GLES2 drivers)
	glLineWidth((GLfloat)g_border_width);
	// Draw 4 line segments (each pair of vertices forms a segment)
		glDrawArrays(GL_LINES, 0, 8);
		// Cleanup
		glDisableVertexAttribArray((GLuint)g_border_a_position_loc);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glUseProgram(0);
	}
	
	// Draw corner markers for keystone adjustment if enabled
	// This is simplistic and would need a shader-based approach for proper GLES2 implementation
	if (g_keystone.enabled && g_show_corner_markers) {
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
	
	// Swap buffers to display the rendered frame
	eglSwapBuffers(e->dpy, e->surf);

	static bool first_frame = true;

	// Check if we should use zero-copy path
	// For now, we'll disable the zero-copy path until we can properly integrate it
	if (false && !g_scanout_disabled && should_use_zero_copy(d, e)) {
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
	struct gbm_bo *bo = gbm_surface_lock_front_buffer(e->gbm_surf);
	if (!bo) { fprintf(stderr, "gbm_surface_lock_front_buffer failed\n"); return false; }
	struct fb_holder *h = gbm_bo_get_user_data(bo);
	uint32_t fb_id = h ? h->fb : 0;
	if (!fb_id) {
		uint32_t handle = gbm_bo_get_handle(bo).u32;
		uint32_t pitch = gbm_bo_get_stride(bo);
		uint32_t width = gbm_bo_get_width(bo);
		uint32_t height= gbm_bo_get_height(bo);
		if (!g_scanout_disabled && drmModeAddFB(d->fd, width, height, 24, 32, pitch, handle, &fb_id)) {
			fprintf(stderr, "drmModeAddFB failed (w=%u h=%u pitch=%u handle=%u err=%s)\n", width, height, pitch, handle, strerror(errno));
			gbm_surface_release_buffer(e->gbm_surf, bo);
			return false;
		}
		struct fb_holder *nh = calloc(1, sizeof(*nh));
		if (!nh) { fprintf(stderr, "Out of memory allocating fb_holder\n"); gbm_surface_release_buffer(e->gbm_surf, bo); return false; }
		nh->fb = fb_id; nh->fd = d->fd;
		gbm_bo_set_user_data(bo, nh, bo_destroy_handler);
	}
	static bool first=true;
	if (!g_scanout_disabled && first) {
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
			fprintf(stderr, "%s failed (%s)\n", 
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
			if (drmModePageFlip(d->fd, d->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, bo)) {
				gbm_surface_release_buffer(e->gbm_surf, bo);
				return false;
			}
		}
		g_pending_flip = 1; // will release in handler
		g_pending_flips++;  // increment pending flip count
	} else {
		// Offscreen mode: just release BO immediately (no scanout usage).
		gbm_surface_release_buffer(e->gbm_surf, bo);
	}
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
		{"v4l2", no_argument, NULL, 'v'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "lhv", long_options, NULL)) != -1) {
		switch (opt) {
			case 'l':
				g_loop_playback = 1;
				break;
			case 'v':
				g_use_v4l2_decoder = 1;
				break;
			case 'h':
				fprintf(stderr, "Usage: %s [options] <video-file>\n", argv[0]);
				fprintf(stderr, "Options:\n");
				fprintf(stderr, "  -l, --loop            Loop playback continuously\n");
				fprintf(stderr, "  -v, --v4l2            Use V4L2 hardware decoder (RPi4 only)\n");
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
	if (!init_gbm_egl(&drm, &eglc)) RET("init_gbm_egl");
	// Optional preallocation of FB ring (env PICKLE_FB_RING, default 3)
	int fb_ring_n = 3; {
		const char *re = getenv("PICKLE_FB_RING");
		if (re && *re) {
			int v = atoi(re); if (v > 0 && v < 16) fb_ring_n = v; }
	}
	preallocate_fb_ring(&drm, &eglc, fb_ring_n);
	
	// Initialize keystone correction
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
	fprintf(stderr, "  w/a/s/d - Move selected corner up/left/down/right\n");
	fprintf(stderr, "  +/- - Increase/decrease adjustment step size\n");
	fprintf(stderr, "  r - Reset keystone to default\n");
	fprintf(stderr, "  b - Toggle border around video\n");
	fprintf(stderr, "  [/] - Decrease/increase border width\n");
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
	struct termios old_term, new_term;
	tcgetattr(STDIN_FILENO, &old_term);
	new_term = old_term;
	new_term.c_lflag &= (tcflag_t)~(ICANON | ECHO); // Disable canonical mode and echo
	tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
	
	// Set stdin to non-blocking mode
	int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
	
	// Initialize joystick/gamepad support for 8BitDo controller
	init_joystick();
	if (g_joystick_enabled) {
		LOG_INFO("8BitDo controller detected and enabled for keystone adjustment");
		LOG_INFO("Controller mappings: START=Toggle keystone mode");
		LOG_INFO("Cycle button (default X) = Corners TL->TR->BR->BL");
		LOG_INFO("Help button (default B) = Toggle help overlay");
	LOG_INFO("D-pad/Left stick=Move corners, L1/R1=Decrease/Increase step size");
	LOG_INFO("SELECT=Reset keystone, HOME(Guide)=Toggle border");
	LOG_INFO("START+SELECT (hold 2s)=Quit");
	}
	
	while (!g_stop) {
		// Handle decoder-specific events
		if (!g_use_v4l2_decoder) {
			// MPV-specific event handling
			if (g_mpv_wakeup) {
				g_mpv_wakeup = 0;
				drain_mpv_events(player.mpv);
				if (player.rctx) {
					uint64_t flags = mpv_render_context_update(player.rctx);
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
		if (g_joystick_enabled) {
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
				show_help_overlay(player.mpv);
				g_help_visible = 1;
			} else {
				hide_help_overlay(player.mpv);
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
		if (g_joystick_enabled && g_joystick_fd >= 0) {
			pfds[n].fd = g_joystick_fd; pfds[n].events = POLLIN; pfds[n].revents = 0; n++;
		}
		int timeout_ms = -1;
		
		// Calculate appropriate poll timeout based on frame rate and vsync
		if (force_loop || (g_mpv_update_flags & MPV_RENDER_UPDATE_FRAME)) {
			timeout_ms = 0; // don't block if render pending
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
						fprintf(stderr, "\rKeystone correction FORCE enabled, use arrow keys or WASD to adjust corner %d", 
								g_keystone.active_corner + 1);
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
						continue;
					}
					
					// Help overlay
					if (c == 'h' && !g_key_seq_state.in_escape_seq) {
						if (!g_help_visible) {
							show_help_overlay(player.mpv);
							g_help_visible = 1;
						} else {
							hide_help_overlay(player.mpv);
							g_help_visible = 0;
						}
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
						continue;
					}

					// Handle keystone adjustment keys first (to avoid 'q' conflict)
					bool keystone_handled = keystone_handle_key(c);
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
			} else if (g_joystick_enabled && pfds[i].fd == g_joystick_fd) {
				// Handle joystick input
				struct js_event event;
				while (read(g_joystick_fd, &event, sizeof(event)) > 0) {
					if (handle_joystick_event(&event)) {
						// Force a redraw when keystone parameters change
						g_mpv_update_flags |= MPV_RENDER_UPDATE_FRAME;
					}
				}
			}
		}
		if (g_mpv_wakeup) {
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
				if (player.rctx) {
					uint64_t flags = mpv_render_context_update(player.rctx);
					g_mpv_update_flags |= flags;
					
					// Reset decoder if needed (for more aggressive recovery)
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
	tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
	
	// Clean up joystick resources
	if (g_joystick_enabled) {
		cleanup_joystick();
	}
	
	// Clean up HVS keystone if initialized
	hvs_keystone_cleanup();
	
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
	tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
	
	// Clean up joystick resources
	if (g_joystick_enabled) {
		cleanup_joystick();
	}
	
	// Clean up HVS keystone if initialized
	hvs_keystone_cleanup();
	
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

