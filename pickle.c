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

// Version information for production debugging
#define PICKLE_VERSION_MAJOR 1
#define PICKLE_VERSION_MINOR 0
#define PICKLE_VERSION_PATCH 0
#define PICKLE_VERSION_STRING "1.0.0"

#ifndef PICKLE_BUILD_DATE
#define PICKLE_BUILD_DATE __DATE__
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

// Logging macros for consistent output formatting
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_DRM(fmt, ...)   fprintf(stderr, "[DRM] " fmt "\n", ##__VA_ARGS__)
#define LOG_MPV(fmt, ...)   fprintf(stderr, "[MPV] " fmt "\n", ##__VA_ARGS__)
#define LOG_EGL(fmt, ...)   fprintf(stderr, "[EGL] " fmt "\n", ##__VA_ARGS__)
#define LOG_GL(fmt, ...)    fprintf(stderr, "[GL] " fmt "\n", ##__VA_ARGS__)

// Debug logging - only prints when g_debug is enabled
#define LOG_DEBUG(fmt, ...) do { if (g_debug) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)

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
static void handle_sigterm(int s){ (void)s; g_stop = 1; }  // Graceful shutdown for systemd/docker
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

struct kms_ctx {
	int fd;
	drmModeRes *res;
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	drmModeCrtc *orig_crtc;
	uint32_t crtc_id;
	uint32_t connector_id;
	drmModeModeInfo mode;
};

struct egl_ctx {
	struct gbm_device *gbm_dev;
	struct gbm_surface *gbm_surf;
	EGLDisplay dpy;
	EGLConfig config;
	EGLContext ctx;
	EGLSurface surf;
};

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

// Keystone correction structure
typedef struct {
    float points[4][2];      // Normalized corner coordinates [0.0-1.0]
    int active_corner;       // Which corner is currently being adjusted (-1 = none)
    bool enabled;            // Whether keystone correction is enabled
    float matrix[16];        // The transformation matrix for rendering
    bool mesh_enabled;       // Whether to use mesh-based warping instead of simple 4-point
    int mesh_size;           // Mesh grid size (e.g., 4 = 4x4 grid)
    float **mesh_points;     // Dynamic mesh control points [mesh_size][mesh_size][2]
    int active_mesh_point[2];// Active mesh point coordinates (x,y) or (-1,-1) for none
    bool perspective_pins[4];// Whether each corner is pinned (fixed) during adjustments
} keystone_t;

// Forward declaration for mpv player structure (matches typedef below)
typedef struct mpv_player_struct mpv_player_t;

// Video instance structure - encapsulates one video + keystone pair
#define MAX_VIDEOS 2
typedef struct video_instance {
    mpv_player_t *player;         // Pointer to mpv player (allocated separately)
    keystone_t keystone;          // Keystone settings for this video
    GLuint fbo;                   // FBO for mpv render target
    GLuint fbo_texture;           // Texture attached to FBO
    int fbo_w, fbo_h;             // FBO dimensions
    char config_path[512];        // Path to keystone config file
    int index;                    // Instance index (0 or 1)
    const char *video_file;       // Path to video file
    volatile uint64_t update_flags; // mpv update flags for this instance
	int use_subrect;              // Use texture sub-rectangle (single-mpv mode)
	float u0, u1, v0, v1;         // Texture coordinates when use_subrect=1
} video_instance_t;

// Typedefs for clarity
typedef struct kms_ctx kms_ctx_t;
typedef struct egl_ctx egl_ctx_t;
typedef struct fb_ring fb_ring_t;
typedef struct fb_ring_entry fb_ring_entry_t;

// Forward declarations for keystone functions
static void keystone_update_matrix(void);
static void keystone_update_matrix_for(keystone_t *ks);
static void keystone_adjust_corner(int corner, float x_delta, float y_delta);
static bool keystone_handle_key(char key);
static void keystone_init(void);
static void keystone_init_instance(video_instance_t *inst, int video_index, int total_videos);
static bool keystone_load_config(const char* path);
static bool keystone_load_config_to(const char* path, keystone_t *ks);
static bool keystone_load_instance_config(video_instance_t *inst);
static bool keystone_save_config(const char* path);
static bool keystone_save_config_from(const char* path, const keystone_t *ks);
static bool keystone_save_instance_config(video_instance_t *inst);
static void cleanup_mesh_resources(void);

// Global state 
static fb_ring_t g_fb_ring = {0};
static int g_have_master = 0; // set if we successfully become DRM master

// Multi-video instance management
static video_instance_t g_videos[MAX_VIDEOS] = {0};  // Array of video instances
static int g_num_videos = 0;                          // Number of active video instances (1 or 2)
static int g_active_corner_global = 0;                // Global corner index (0-7 for 2 videos, 0-3 for 1)
                                                      // Corners 0-3 = video 0, corners 4-7 = video 1

// Alternate frame rendering for dual-video performance
static int g_alternate_frame_mode = 1;                // 1=enabled (default for dual video)
static int g_render_frame_count = 0;                  // Frame counter for alternation
static int g_single_mpv_mode = 0;                     // 1=use single mpv with lavfi-complex

// Composite FBO/texture when using single mpv for dual videos
static GLuint g_composite_fbo = 0;
static GLuint g_composite_texture = 0;
static int g_composite_w = 0;
static int g_composite_h = 0;

// Helper macros for multi-video corner management
#define CORNER_VIDEO(c) ((c) / 4)                     // Which video instance (0 or 1)
#define CORNER_LOCAL(c) ((c) % 4)                     // Local corner within video (0-3)
#define CORNER_GLOBAL(v, c) ((v) * 4 + (c))           // Global corner from video + local

// Legacy global keystone for backward compatibility (points to g_videos[0].keystone when single video)
static keystone_t g_keystone = {
    .points = {
        {0.0f, 0.0f},  // Top-left
        {1.0f, 0.0f},  // Top-right
        {0.0f, 1.0f},  // Bottom-left
        {1.0f, 1.0f}   // Bottom-right
    },
    .active_corner = -1,
    .enabled = false,
    .mesh_enabled = false,
    .mesh_size = 4,    // 4x4 mesh by default
    .mesh_points = NULL,
    .active_mesh_point = {-1, -1},
    .perspective_pins = {false, false, false, false}
}; // Keystone correction settings (used for single-video mode)
static int g_keystone_adjust_step = 1; // Step size for keystone adjustments (in 1/1000 units)
static bool g_show_border = false; // Whether to show border around the video
static int g_border_width = 5; // Border width in pixels
static bool g_show_background = false; // Deprecated: background is always black now
static bool g_show_corner_markers = true; // Show keystone corner highlights
static int g_loop_playback = 0; // Whether to loop video playback
static GLuint g_keystone_shader_program = 0; // Shader program for keystone correction (shared)
static GLuint g_keystone_vertex_shader = 0;
static GLuint g_keystone_fragment_shader = 0;
static GLuint g_keystone_vertex_buffer = 0;  // Shared vertex buffer
static GLuint g_keystone_texcoord_buffer = 0; // Shared texcoord buffer
static GLuint g_keystone_index_buffer = 0;   // Shared index buffer for quad
// Note: FBO is now per-instance in video_instance_t, these are kept for single-video backward compat
static GLuint g_keystone_fbo = 0;            // Cached FBO for mpv render target (single video mode)
static GLuint g_keystone_fbo_texture = 0;    // Texture attached to FBO (single video mode)
static int g_keystone_fbo_w = 0;             // FBO width
static int g_keystone_fbo_h = 0;             // FBO height
static GLint g_keystone_a_position_loc = -1;
static GLint g_keystone_a_texcoord_loc = -1;
static GLint g_keystone_u_texture_loc = -1;

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
static void cleanup_keystone_shader(void);
static bool init_joystick(void);
static bool handle_joystick_event(struct js_event *event);

static bool ensure_drm_master(int fd) {
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
static bool init_drm(kms_ctx_t *d) {
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
static void deinit_drm(kms_ctx_t *d) {
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
static bool init_gbm_egl(const kms_ctx_t *d, egl_ctx_t *e) {
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
static void deinit_gbm_egl(egl_ctx_t *e) {
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
struct mpv_player_struct {
	mpv_handle *mpv;             // MPV API handle
	mpv_render_context *rctx;    // MPV render context for OpenGL rendering
	int using_libmpv;            // Flag indicating fallback to vo=libmpv occurred
};

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

// Debug / instrumentation control (enabled with PICKLE_DEBUG env)
static int g_debug = 0;

// Performance controls
static int g_triple_buffer = 1;         // Enable triple buffering by default
static int g_vsync_enabled = 1;         // Enable vsync by default
static int g_frame_timing_enabled = 0;  // Detailed frame timing metrics (when PICKLE_TIMING=1)

// Target FPS limiting for smooth playback (0 = unlimited)
static int g_target_fps = 0;            // Will be auto-set based on video count
static double g_frame_interval_us = 0;  // Microseconds between frames (calculated from target_fps)
static struct timeval g_last_render_time = {0}; // For frame pacing

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

static double tv_diff(const struct timeval *a, const struct timeval *b) {
	return (double)(a->tv_sec - b->tv_sec) + (double)(a->tv_usec - b->tv_usec)/1e6;
}

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
	double mpv_fps = 0.0, container_fps = 0.0;
	if (p && p->mpv) {
		mpv_get_property(p->mpv, "drop-frame-count", MPV_FORMAT_INT64, &drop_dec);
		mpv_get_property(p->mpv, "vo-drop-frame-count", MPV_FORMAT_INT64, &drop_vo);
		mpv_get_property(p->mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &mpv_fps);
		mpv_get_property(p->mpv, "container-fps", MPV_FORMAT_DOUBLE, &container_fps);
	}
	fprintf(stderr, "[stats] total=%.2fs frames=%llu avg_fps=%.2f inst_fps=%.2f mpv_fps=%.1f container=%.1f dropped=%lld/%lld\n",
			total, (unsigned long long)frames_now, avg_fps, inst_fps,
			mpv_fps, container_fps, (long long)drop_dec, (long long)drop_vo);
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

// Helper for mpv option failure logging
static void log_opt_result(const char *opt, int code) {
	if (code < 0) fprintf(stderr, "[mpv] option %s failed (%d)\n", opt, code);
}

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
	if (!vo_req || !*vo_req) vo_req = "libmpv";
	r = mpv_set_option_string(p->mpv, "vo", vo_req);
	if (r < 0) {
		fprintf(stderr, "[mpv] vo=%s failed (%d); falling back to vo=libmpv\n", vo_req, r);
		vo_req = "libmpv";
		r = mpv_set_option_string(p->mpv, "vo", "libmpv");
		log_opt_result("vo=libmpv", r);
	}
	const char *vo_used = vo_req;
	
	// Hardware decoding: drm-copy is most stable for RPi4 with vo=libmpv
	// Uses V4L2 hardware decoder with efficient GPU upload
	const char *hwdec_pref = getenv("PICKLE_HWDEC");
	if (!hwdec_pref || !*hwdec_pref) hwdec_pref = "drm-copy";
	r = mpv_set_option_string(p->mpv, "hwdec", hwdec_pref); 
	log_opt_result("hwdec", r);
	
	// Specify V4L2 codec preference for RPi4 (uses hardware H.264/HEVC decoder)
	r = mpv_set_option_string(p->mpv, "hwdec-codecs", "h264,hevc,mpeg2video,mpeg4,vp8,vp9");
	log_opt_result("hwdec-codecs", r);
	
	r = mpv_set_option_string(p->mpv, "opengl-es", "yes"); log_opt_result("opengl-es=yes", r);
	
	// Disable aspect ratio preservation to fill entire display (important for keystone/projector mode)
	r = mpv_set_option_string(p->mpv, "keepaspect", "no");
	log_opt_result("keepaspect=no", r);
	
	// Video sync mode: use "display-vdrop" for better 60fps performance
	// (drops/repeats frames to match display, less GPU overhead than interpolation)
	const char *video_sync = g_vsync_enabled ? "display-vdrop" : "audio";
	r = mpv_set_option_string(p->mpv, "video-sync", video_sync);
	log_opt_result("video-sync", r);
	
	// Disable interpolation by default (saves GPU, especially with keystone shader)
	// Can still be enabled via PICKLE_INTERPOLATION=1 for non-matching framerates
	const char *interp_env = getenv("PICKLE_INTERPOLATION");
	if (g_vsync_enabled && interp_env && *interp_env && strcmp(interp_env, "0") != 0) {
		r = mpv_set_option_string(p->mpv, "interpolation", "yes");
		log_opt_result("interpolation", r);
		// Use fast bilinear for temporal interpolation (low GPU overhead)
		r = mpv_set_option_string(p->mpv, "tscale", "oversample");
		log_opt_result("tscale=oversample", r);
	}
	
	// Note: vo-queue-size not supported in vo=libmpv mode (fails with -5)
	
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
	
	// === RPi4 GPU Optimizations ===
	// Use fast bilinear scaling (less GPU load than spline/lanczos)
	r = mpv_set_option_string(p->mpv, "scale", "bilinear");
	log_opt_result("scale=bilinear", r);
	
	// Disable expensive post-processing for embedded/kiosk use
	r = mpv_set_option_string(p->mpv, "deband", "no");
	log_opt_result("deband=no", r);
	r = mpv_set_option_string(p->mpv, "dither-depth", "no");
	log_opt_result("dither-depth=no", r);
	
	// Reduce latency for initial frame display
	r = mpv_set_option_string(p->mpv, "demuxer-readahead-secs", "1.0");
	log_opt_result("demuxer-readahead-secs", r);
	
	// Prefer using MPV_RENDER_PARAM_FLIP_Y during rendering instead of global rotation

	// vo=libmpv doesn't need gpu-context configuration

	int use_adv = 0;
	const char *adv_env = getenv("PICKLE_GL_ADV");
	if (adv_env && *adv_env && strcmp(vo_used, "gpu") == 0) use_adv = 1;
	fprintf(stderr, "[mpv] Advanced control %s (PICKLE_GL_ADV=%s vo=%s)\n", use_adv?"ENABLED":"disabled", adv_env?adv_env:"unset", vo_used);

	// Audio is disabled by default for video-only playback (eliminates A/V desync warnings)
	// Set PICKLE_FORCE_AUDIO=1 to enable audio output
	int disable_audio = 1;  // Default: no audio
	if (getenv("PICKLE_FORCE_AUDIO")) { 
		fprintf(stderr, "[mpv] Audio enabled (PICKLE_FORCE_AUDIO set)\n"); 
		disable_audio = 0; 
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

// Initialize a single mpv instance that plays two videos via lavfi-complex and outputs a side-by-side composite
static bool init_mpv_lavfi_dual(mpv_player_t *p, const char *file1, const char *file2) {
	if (!p || !file1 || !file2) return false;
	memset(p, 0, sizeof(*p));

	p->mpv = mpv_create();
	if (!p->mpv) { fprintf(stderr, "failed to create mpv context\n"); return false; }

	const char *vo_env = getenv("PICKLE_VO");
	const char *vo_used = vo_env ? vo_env : "gpu";
	int r = mpv_set_option_string(p->mpv, "vo", vo_used);
	if (r < 0) { fprintf(stderr, "mpv_set_option(vo=%s) failed (%d)\n", vo_used, r); return false; }

	const char *hwdec_pref = getenv("PICKLE_HWDEC");
	if (!hwdec_pref || !*hwdec_pref) hwdec_pref = "no"; // safer default
	r = mpv_set_option_string(p->mpv, "hwdec", hwdec_pref);
	log_opt_result("hwdec", r);

	// Basic playback flags
	mpv_set_option_string(p->mpv, "osd-level", "0");
	mpv_set_option_string(p->mpv, "cursor-autohide", "always");
	mpv_set_option_string(p->mpv, "audio", getenv("PICKLE_FORCE_AUDIO") ? "yes" : "no");

	// GPU-friendly defaults
	mpv_set_option_string(p->mpv, "scale", "bilinear");
	mpv_set_option_string(p->mpv, "deband", "no");
	mpv_set_option_string(p->mpv, "dither-depth", "no");

	// Compose two inputs side-by-side at half resolution each (960x540 for 1080p sources)
	const char *graph = "[vid1]scale=iw/2:ih/2[va];[vid2]scale=iw/2:ih/2[vb];[va][vb]hstack=inputs=2:shortest=1[vo]";
	r = mpv_set_option_string(p->mpv, "lavfi-complex", graph);
	log_opt_result("lavfi-complex", r);

	if (mpv_initialize(p->mpv) < 0) { fprintf(stderr, "mpv_initialize failed\n"); return false; }

	mpv_opengl_init_params gl_init = { .get_proc_address = mpv_get_proc_address, .get_proc_address_ctx = NULL };
	mpv_render_param params[4]; memset(params,0,sizeof(params)); int pi=0;
	params[pi].type = MPV_RENDER_PARAM_API_TYPE; params[pi++].data = (void*)MPV_RENDER_API_TYPE_OPENGL;
	params[pi].type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS; params[pi++].data = &gl_init;
	params[pi].type = 0;
	int cr = mpv_render_context_create(&p->rctx, p->mpv, params);
	if (cr < 0) { fprintf(stderr, "mpv_render_context_create failed (%d)\n", cr); return false; }
	mpv_render_context_set_update_callback(p->rctx, on_mpv_events, NULL);
	mpv_set_wakeup_callback(p->mpv, mpv_wakeup_cb, NULL);

	const char *cmd1[] = {"loadfile", file1, NULL};
	if (mpv_command(p->mpv, cmd1) < 0) { fprintf(stderr, "Failed to load file %s\n", file1); return false; }
	const char *cmd2[] = {"loadfile", file2, "append", NULL};
	if (mpv_command(p->mpv, cmd2) < 0) { fprintf(stderr, "Failed to append file %s\n", file2); return false; }

	fprintf(stderr, "[mpv] Initialized lavfi-complex dual playback (vo=%s)\n", vo_used);
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
	g_last_render_time = now;  // For frame pacing
	
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
 * Load keystone configuration from a specified file path into a specific keystone struct
 * 
 * @param path The file path to load the configuration from
 * @param ks Pointer to keystone struct to load into
 * @return true if the configuration was loaded successfully, false otherwise
 */
static bool keystone_load_config_to(const char* path, keystone_t *ks) {
    if (!path || !ks) return false;
    
    FILE* f = fopen(path, "r");
    if (!f) return false;
    
    char line[128];
    while (fgets(line, sizeof(line), f)) {

        if (line[0] == '#' || line[0] == '\n') continue;
        
        if (strncmp(line, "enabled=", 8) == 0) {
            ks->enabled = (atoi(line + 8) != 0);
        }
        else if (strncmp(line, "mesh_enabled=", 13) == 0) {
            ks->mesh_enabled = (atoi(line + 13) != 0);
        }
        else if (strncmp(line, "corner1=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &ks->points[0][0], &ks->points[0][1]);
        }
        else if (strncmp(line, "corner2=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &ks->points[1][0], &ks->points[1][1]);
        }
        else if (strncmp(line, "corner3=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &ks->points[2][0], &ks->points[2][1]);
        }
        else if (strncmp(line, "corner4=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &ks->points[3][0], &ks->points[3][1]);
        }
        else if (strncmp(line, "pin1=", 5) == 0) {
            ks->perspective_pins[0] = (atoi(line + 5) != 0);
        }
        else if (strncmp(line, "pin2=", 5) == 0) {
            ks->perspective_pins[1] = (atoi(line + 5) != 0);
        }
        else if (strncmp(line, "pin3=", 5) == 0) {
            ks->perspective_pins[2] = (atoi(line + 5) != 0);
        }
        else if (strncmp(line, "pin4=", 5) == 0) {
            ks->perspective_pins[3] = (atoi(line + 5) != 0);
        }
    }
    fclose(f);
    
    if (ks->enabled) {
        keystone_update_matrix_for(ks);
    }
    
    return true;
}

/**
 * Save keystone configuration from a specific keystone struct to a file
 * 
 * @param path The file path to save the configuration to
 * @param ks Pointer to keystone struct to save from
 * @return true if the configuration was saved successfully, false otherwise
 */
static bool keystone_save_config_from(const char* path, const keystone_t *ks) {
    if (!path || !ks) return false;
    
    FILE* f = fopen(path, "w");
    if (!f) {
        LOG_ERROR("Failed to open file for writing: %s (%s)", path, strerror(errno));
        return false;
    }
    
    fprintf(f, "# Pickle keystone configuration\n");
    fprintf(f, "enabled=%d\n", ks->enabled ? 1 : 0);
    fprintf(f, "mesh_enabled=%d\n", ks->mesh_enabled ? 1 : 0);
    
    for (int i = 0; i < 4; i++) {
        fprintf(f, "corner%d=%.6f,%.6f\n", i+1, ks->points[i][0], ks->points[i][1]);
        fprintf(f, "pin%d=%d\n", i+1, ks->perspective_pins[i] ? 1 : 0);
    }
    
    fclose(f);
    return true;
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
static bool keystone_load_config(const char* path) {
    if (!path) return false;
    
    FILE* f = fopen(path, "r");
    if (!f) return false;
    
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;
        
        if (strncmp(line, "enabled=", 8) == 0) {
            g_keystone.enabled = (atoi(line + 8) != 0);
        }
        else if (strncmp(line, "mesh_enabled=", 13) == 0) {
            g_keystone.mesh_enabled = (atoi(line + 13) != 0);
        }
        else if (strncmp(line, "mesh_size=", 10) == 0) {
            int new_size = atoi(line + 10);
            if (new_size >= 2 && new_size <= 10) { // Sanity check
                // Only change if different (requires reallocation)
                if (new_size != g_keystone.mesh_size) {
                    // Clean up old mesh if it exists
                    cleanup_mesh_resources();
                    
                    // Set new size and allocate
					g_keystone.mesh_size = new_size;
					g_keystone.mesh_points = malloc((size_t)new_size * sizeof(float*));
                    if (g_keystone.mesh_points) {
                        for (int i = 0; i < new_size; i++) {
							g_keystone.mesh_points[i] = malloc((size_t)new_size * 2 * sizeof(float));
                            if (g_keystone.mesh_points[i]) {
                                // Initialize with default grid positions
                                for (int j = 0; j < new_size; j++) {
									float x = (float)j / (float)(new_size - 1);
									float y = (float)i / (float)(new_size - 1);
                                    g_keystone.mesh_points[i][j*2] = x;
                                    g_keystone.mesh_points[i][j*2+1] = y;
                                }
                            }
                        }
                    }
                }
            }
        }
        else if (strncmp(line, "corner1=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &g_keystone.points[0][0], &g_keystone.points[0][1]);
        }
        else if (strncmp(line, "corner2=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &g_keystone.points[1][0], &g_keystone.points[1][1]);
        }
        else if (strncmp(line, "corner3=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &g_keystone.points[2][0], &g_keystone.points[2][1]);
        }
        else if (strncmp(line, "corner4=", 8) == 0) {
            sscanf(line + 8, "%f,%f", &g_keystone.points[3][0], &g_keystone.points[3][1]);
        }
        else if (strncmp(line, "pin1=", 5) == 0) {
            g_keystone.perspective_pins[0] = (atoi(line + 5) != 0);
        }
        else if (strncmp(line, "pin2=", 5) == 0) {
            g_keystone.perspective_pins[1] = (atoi(line + 5) != 0);
        }
        else if (strncmp(line, "pin3=", 5) == 0) {
            g_keystone.perspective_pins[2] = (atoi(line + 5) != 0);
        }
        else if (strncmp(line, "pin4=", 5) == 0) {
            g_keystone.perspective_pins[3] = (atoi(line + 5) != 0);
        }
        else if (strncmp(line, "border=", 7) == 0) {
            g_show_border = (atoi(line + 7) != 0);
        }
        else if (strncmp(line, "cornermarks=", 12) == 0) {
            g_show_corner_markers = (atoi(line + 12) != 0);
        }
        else if (strncmp(line, "mesh_", 5) == 0) {
            // Parse mesh point coordinates: mesh_i_j=x,y
            int i, j;
            float x, y;
            if (sscanf(line + 5, "%d_%d=%f,%f", &i, &j, &x, &y) == 4) {
                if (i >= 0 && i < g_keystone.mesh_size && 
                    j >= 0 && j < g_keystone.mesh_size &&
                    g_keystone.mesh_points && g_keystone.mesh_points[i]) {
                    g_keystone.mesh_points[i][j*2] = x;
                    g_keystone.mesh_points[i][j*2+1] = y;
                }
            }
        }
    }
    fclose(f);
    
    // Update matrix based on loaded settings
    if (g_keystone.enabled) {
        keystone_update_matrix();
    }
    
    return true;
}

static void keystone_init(void) {
    // Initialize with default values (rectangle at full screen)
    g_keystone.points[0][0] = 0.0f; g_keystone.points[0][1] = 0.0f; // Top-left
    g_keystone.points[1][0] = 1.0f; g_keystone.points[1][1] = 0.0f; // Top-right
    g_keystone.points[2][0] = 1.0f; g_keystone.points[2][1] = 1.0f; // Bottom-right
    g_keystone.points[3][0] = 0.0f; g_keystone.points[3][1] = 1.0f; // Bottom-left
    
    g_keystone.active_corner = -1; // No active corner
    g_keystone.enabled = false;
    g_keystone.mesh_enabled = false;
    g_keystone.mesh_size = 4; // 4x4 grid by default
    g_keystone.active_mesh_point[0] = -1;
    g_keystone.active_mesh_point[1] = -1;
    
    // Initialize perspective pins to all unpinned
    for (int i = 0; i < 4; i++) {
        g_keystone.perspective_pins[i] = false;
    }
    
    // Initialize identity matrix
    for (int i = 0; i < 16; i++) {
        g_keystone.matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    
    // Allocate mesh points if necessary
    if (g_keystone.mesh_points == NULL) {
	g_keystone.mesh_points = malloc((size_t)g_keystone.mesh_size * sizeof(float*));
        if (g_keystone.mesh_points) {
            for (int i = 0; i < g_keystone.mesh_size; i++) {
				g_keystone.mesh_points[i] = malloc((size_t)g_keystone.mesh_size * 2 * sizeof(float));
                if (g_keystone.mesh_points[i]) {
                    // Initialize regular grid of points
                    for (int j = 0; j < g_keystone.mesh_size; j++) {
						float x = (float)j / (float)(g_keystone.mesh_size - 1);
						float y = (float)i / (float)(g_keystone.mesh_size - 1);
                        g_keystone.mesh_points[i][j*2] = x;     // x coordinate
                        g_keystone.mesh_points[i][j*2+1] = y;   // y coordinate
                    }
                }
            }
        }
    }
    
    bool config_loaded = false;
    
    // First try to load from local keystone.conf file in current directory
    config_loaded = keystone_load_config("./keystone.conf");
    if (config_loaded) {
        LOG_INFO("Loaded keystone configuration from ./keystone.conf");
    }
    
    // If local config not found, try to load from user's home directory
    if (!config_loaded) {
        const char* home = getenv("HOME");
        if (home) {
            char config_path[512];
            snprintf(config_path, sizeof(config_path), "%s/.config/pickle_keystone.conf", home);
            if (keystone_load_config(config_path)) {
                LOG_INFO("Loaded keystone configuration from %s", config_path);
                config_loaded = true;
            }
        }
    }
    
    // Check for environment variable to control keystone
    // PICKLE_KEYSTONE=1 enables, PICKLE_KEYSTONE=0 disables
    const char* keystone_env = getenv("PICKLE_KEYSTONE");
    if (keystone_env && *keystone_env) {
        if (strcmp(keystone_env, "0") == 0 || strcasecmp(keystone_env, "off") == 0 || strcasecmp(keystone_env, "no") == 0) {
            g_keystone.enabled = false;
            LOG_INFO("Keystone correction disabled via PICKLE_KEYSTONE=0");
        } else {
            g_keystone.enabled = true;
            LOG_INFO("Keystone correction enabled via PICKLE_KEYSTONE");
        }
        
        // Parse custom corner positions if provided in format "x1,y1,x2,y2,x3,y3,x4,y4"
        if (g_keystone.enabled && strlen(keystone_env) > 10) // Arbitrary minimum length for valid data
        {
            float values[8];
            int count = 0;
            char* copy = strdup(keystone_env);
            char* token = strtok(copy, ",");
            
            while (token && count < 8) {
				values[count++] = strtof(token, NULL);
                token = strtok(NULL, ",");
            }
            
            if (count == 8) {
                for (int i = 0; i < 4; i++) {
                    g_keystone.points[i][0] = values[i*2];
                    g_keystone.points[i][1] = values[i*2+1];
                }
                LOG_INFO("Loaded keystone corners from environment variable");
            }
            
            free(copy);
        }
    }
    
    // Check for environment variable to set adjustment step size
    const char* step_env = getenv("PICKLE_KEYSTONE_STEP");
    if (step_env && *step_env) {
        int step = atoi(step_env);
        if (step > 0 && step <= 100) {
            g_keystone_adjust_step = step;
            LOG_INFO("Keystone adjustment step set to %d", g_keystone_adjust_step);
        }
    }
    
    // Check for environment variable to enable border
    const char* border_env = getenv("PICKLE_SHOW_BORDER");
    if (border_env && *border_env) {
        g_show_border = true;
        int width = atoi(border_env);
        if (width > 0 && width <= 50) {
            g_border_width = width;
        }
        LOG_INFO("Video border enabled with width %d", g_border_width);
    }
    
	// Background is always black; ignore PICKLE_SHOW_BACKGROUND
	g_show_background = false;
    
    // Initialize shader program if keystone is enabled
    if (g_keystone.enabled && g_keystone_shader_program == 0) {
        // Note: shader initialization must be done after EGL context is created
        // We'll do it in render_frame_fixed when needed
    }

	// Texture flip defaults via env
	const char* flipx = getenv("PICKLE_TEX_FLIP_X");
	const char* flipy = getenv("PICKLE_TEX_FLIP_Y");
	if (flipx && *flipx) g_tex_flip_x = atoi(flipx) ? 1 : 0;
	if (flipy && *flipy) g_tex_flip_y = atoi(flipy) ? 1 : 0;

	// Corner markers env (1=on, 0=off)
	const char* show_corners = getenv("PICKLE_SHOW_CORNERS");
	if (show_corners && *show_corners) g_show_corner_markers = atoi(show_corners) ? true : false;

	// Single mpv lavfi-complex mode (1=on, 0=off)
	const char* single_mpv = getenv("PICKLE_SINGLE_MPV");
	if (single_mpv && *single_mpv) g_single_mpv_mode = atoi(single_mpv) ? 1 : 0;
	
	// Alternate frame rendering mode for dual-video (1=on [default], 0=off)
	// When enabled, only one video's FBO is updated per frame, halving GPU work
	const char* alt_frame = getenv("PICKLE_ALTERNATE_FRAMES");
	if (alt_frame && *alt_frame) g_alternate_frame_mode = atoi(alt_frame) ? 1 : 0;
}

/**
 * Initialize keystone for a specific video instance with default position
 * 
 * @param inst Pointer to the video instance
 * @param video_index Index of this video (0 or 1)
 * @param total_videos Total number of videos (1 or 2)
 */
static void keystone_init_instance(video_instance_t *inst, int video_index, int total_videos) {
    keystone_t *ks = &inst->keystone;
    
    // Initialize with default values
    if (total_videos == 1) {
        // Single video: full screen
        ks->points[0][0] = 0.0f; ks->points[0][1] = 0.0f; // Top-left
        ks->points[1][0] = 1.0f; ks->points[1][1] = 0.0f; // Top-right
        ks->points[2][0] = 1.0f; ks->points[2][1] = 1.0f; // Bottom-right
        ks->points[3][0] = 0.0f; ks->points[3][1] = 1.0f; // Bottom-left
    } else {
        // Dual video: split left/right
        if (video_index == 0) {
            // Video 0: left half
            ks->points[0][0] = 0.0f; ks->points[0][1] = 0.0f; // Top-left
            ks->points[1][0] = 0.5f; ks->points[1][1] = 0.0f; // Top-right
            ks->points[2][0] = 0.5f; ks->points[2][1] = 1.0f; // Bottom-right
            ks->points[3][0] = 0.0f; ks->points[3][1] = 1.0f; // Bottom-left
        } else {
            // Video 1: right half
            ks->points[0][0] = 0.5f; ks->points[0][1] = 0.0f; // Top-left
            ks->points[1][0] = 1.0f; ks->points[1][1] = 0.0f; // Top-right
            ks->points[2][0] = 1.0f; ks->points[2][1] = 1.0f; // Bottom-right
            ks->points[3][0] = 0.5f; ks->points[3][1] = 1.0f; // Bottom-left
        }
    }
    
    ks->active_corner = -1;
    ks->enabled = true;  // Always enabled in multi-video mode
    ks->mesh_enabled = false;
    ks->mesh_size = 4;
    ks->mesh_points = NULL;
    ks->active_mesh_point[0] = -1;
    ks->active_mesh_point[1] = -1;
    
    for (int i = 0; i < 4; i++) {
        ks->perspective_pins[i] = false;
    }
    
    // Initialize identity matrix
    for (int i = 0; i < 16; i++) {
        ks->matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
    
    // Initialize FBO to 0 (will be created during render)
    inst->fbo = 0;
    inst->fbo_texture = 0;
    inst->fbo_w = 0;
    inst->fbo_h = 0;
    inst->index = video_index;
    inst->update_flags = 0;
    
    // Set config path for this instance
    snprintf(inst->config_path, sizeof(inst->config_path), "./keystone_%d.conf", video_index);
    
    LOG_INFO("Initialized keystone %d: TL(%.2f,%.2f) TR(%.2f,%.2f) BR(%.2f,%.2f) BL(%.2f,%.2f)",
        video_index,
        ks->points[0][0], ks->points[0][1],
        ks->points[1][0], ks->points[1][1],
        ks->points[2][0], ks->points[2][1],
        ks->points[3][0], ks->points[3][1]);
}

/**
 * Load keystone config for a specific video instance
 * 
 * @param inst Pointer to the video instance
 * @return true if config was loaded, false otherwise
 */
static bool keystone_load_instance_config(video_instance_t *inst) {
    // Try local config first
    if (keystone_load_config_to(inst->config_path, &inst->keystone)) {
        LOG_INFO("Loaded keystone %d config from %s", inst->index, inst->config_path);
        return true;
    }
    
    // Try home directory
    const char *home = getenv("HOME");
    if (home) {
        char alt_path[512];
        snprintf(alt_path, sizeof(alt_path), "%s/.config/pickle_keystone_%d.conf", home, inst->index);
        if (keystone_load_config_to(alt_path, &inst->keystone)) {
            LOG_INFO("Loaded keystone %d config from %s", inst->index, alt_path);
            snprintf(inst->config_path, sizeof(inst->config_path), "%s", alt_path);
            return true;
        }
    }
    
    return false;
}

/**
 * Save keystone config for a specific video instance
 * 
 * @param inst Pointer to the video instance
 * @return true if config was saved, false otherwise
 */
static bool keystone_save_instance_config(video_instance_t *inst) {
    return keystone_save_config_from(inst->config_path, &inst->keystone);
}

/**
 * Update keystone matrix for a specific instance
 * 
 * @param ks Pointer to the keystone structure to update
 */
static void keystone_update_matrix_for(keystone_t *ks) {
    if (!ks->enabled) {
        for (int i = 0; i < 16; i++) {
            ks->matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
        return;
    }
    
    // Convert normalized corner positions to clip space
    float vertices[8];
    for (int i = 0; i < 4; i++) {
        vertices[i*2]   = ks->points[i][0] * 2.0f - 1.0f;
        vertices[i*2+1] = (1.0f - ks->points[i][1]) * 2.0f - 1.0f;
    }
    
    // Store as vertex positions
    ks->matrix[0] = vertices[0];  // TL.x
    ks->matrix[1] = vertices[1];  // TL.y
    ks->matrix[2] = vertices[2];  // TR.x
    ks->matrix[3] = vertices[3];  // TR.y
    ks->matrix[4] = vertices[6];  // BL.x
    ks->matrix[5] = vertices[7];  // BL.y
    ks->matrix[6] = vertices[4];  // BR.x
    ks->matrix[7] = vertices[5];  // BR.y
}

/**
 * Calculate the perspective transformation matrix based on the corner points
 * Updates the vertex coordinates used for rendering with the shader
 */
static void keystone_update_matrix(void) {
    if (!g_keystone.enabled) {
        // Identity matrix
        for (int i = 0; i < 16; i++) {
            g_keystone.matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }
        return;
    }
    
    // Convert the normalized corner positions to clip space coordinates (-1 to 1)
    float vertices[8];
    
    // Map from 0-1 normalized space to -1 to 1 clip space
    for (int i = 0; i < 4; i++) {
        vertices[i*2]   = g_keystone.points[i][0] * 2.0f - 1.0f; // x: 0-1 -> -1 to 1
        vertices[i*2+1] = (1.0f - g_keystone.points[i][1]) * 2.0f - 1.0f; // y: 0-1 -> 1 to -1 (flip y)
    }
    
    // Store the matrix as vertex positions for the shader
    // Vertex order for triangle strip: top-left, top-right, bottom-left, bottom-right
    g_keystone.matrix[0] = vertices[0];  // TL.x
    g_keystone.matrix[1] = vertices[1];  // TL.y
    g_keystone.matrix[2] = vertices[2];  // TR.x
    g_keystone.matrix[3] = vertices[3];  // TR.y
    g_keystone.matrix[4] = vertices[6];  // BL.x
    g_keystone.matrix[5] = vertices[7];  // BL.y
    g_keystone.matrix[6] = vertices[4];  // BR.x
    g_keystone.matrix[7] = vertices[5];  // BR.y
    
    LOG_DEBUG("Updated keystone vertices: TL(%.2f,%.2f) TR(%.2f,%.2f) BL(%.2f,%.2f) BR(%.2f,%.2f)",
        vertices[0], vertices[1], vertices[2], vertices[3], 
        vertices[6], vertices[7], vertices[4], vertices[5]);
}

/**
 * Adjust a specific corner of the keystone correction
 * 
 * @param corner Corner index (0-3)
 * @param x_delta X adjustment (positive = right)
 * @param y_delta Y adjustment (positive = down)
 */
// Adjust a mesh point position
static void keystone_adjust_mesh_point(int row, int col, float x_delta, float y_delta) {
    if (row < 0 || row >= g_keystone.mesh_size || 
        col < 0 || col >= g_keystone.mesh_size ||
        !g_keystone.mesh_points || !g_keystone.mesh_points[row]) {
        return;
    }
    
    // Make step 10x larger to make movement more noticeable
    x_delta *= 10.0f;
    y_delta *= 10.0f;
    
    // Adjust the mesh point position
    g_keystone.mesh_points[row][col*2] += x_delta;
    g_keystone.mesh_points[row][col*2+1] += y_delta;
    
    // Clamp values to reasonable ranges (slightly beyond 0-1 to allow for overcorrection)
    g_keystone.mesh_points[row][col*2] = fmaxf(-0.5f, fminf(1.5f, g_keystone.mesh_points[row][col*2]));
    g_keystone.mesh_points[row][col*2+1] = fmaxf(-0.5f, fminf(1.5f, g_keystone.mesh_points[row][col*2+1]));
}

// Toggle pinning status of a corner
static void keystone_toggle_pin(int corner) {
    if (corner < 0 || corner > 3) return;
    
    // Toggle the pin status
    g_keystone.perspective_pins[corner] = !g_keystone.perspective_pins[corner];
    LOG_INFO("Corner %d pin %s", corner + 1, g_keystone.perspective_pins[corner] ? "enabled" : "disabled");
}

/**
 * Adjust a corner on a specific keystone instance
 * 
 * @param ks Pointer to the keystone to adjust
 * @param corner The corner index (0-3)
 * @param x_delta The X adjustment delta
 * @param y_delta The Y adjustment delta
 */
static void keystone_adjust_corner_on(keystone_t *ks, int corner, float x_delta, float y_delta) {
    if (!ks || corner < 0 || corner > 3) return;
    
    if (ks->perspective_pins[corner]) {
        LOG_INFO("Corner %d is pinned.", corner + 1);
        return;
    }
    
    x_delta *= 10.0f;
    y_delta *= 10.0f;
    
    ks->points[corner][0] += x_delta;
    ks->points[corner][1] += y_delta;
    
    ks->points[corner][0] = fmaxf(-0.5f, fminf(1.5f, ks->points[corner][0]));
    ks->points[corner][1] = fmaxf(-0.5f, fminf(1.5f, ks->points[corner][1]));
    
    keystone_update_matrix_for(ks);
    
    LOG_DEBUG("Adjusted corner %d to (%.3f, %.3f)", 
              corner + 1, ks->points[corner][0], ks->points[corner][1]);
}

static void keystone_adjust_corner(int corner, float x_delta, float y_delta) {
    // In multi-video mode, adjust the correct keystone based on g_active_corner_global
    if (g_num_videos > 1) {
        int video_idx = CORNER_VIDEO(g_active_corner_global);
        int local_corner = CORNER_LOCAL(g_active_corner_global);
        if (video_idx >= 0 && video_idx < g_num_videos) {
            keystone_adjust_corner_on(&g_videos[video_idx].keystone, local_corner, x_delta, y_delta);
            return;
        }
    }
    
    // Single video mode: use legacy g_keystone
    if (corner < 0 || corner > 3) return;
    if (g_keystone.perspective_pins[corner]) {
        LOG_INFO("Corner %d is pinned. Unpin with Shift+%d to adjust.", corner + 1, corner + 1);
        return;
    }
    
    // Make step 10x larger to make movement more noticeable
    x_delta *= 10.0f;
    y_delta *= 10.0f;
    
    // Adjust the corner position
    g_keystone.points[corner][0] += x_delta;
    g_keystone.points[corner][1] += y_delta;
    
    // Clamp values to reasonable ranges (slightly beyond 0-1 to allow for overcorrection)
    g_keystone.points[corner][0] = fmaxf(-0.5f, fminf(1.5f, g_keystone.points[corner][0]));
    g_keystone.points[corner][1] = fmaxf(-0.5f, fminf(1.5f, g_keystone.points[corner][1]));
    
    // Update the transformation matrix
    keystone_update_matrix();
    
    LOG_INFO("Adjusted corner %d to (%.3f, %.3f)", 
              corner + 1, g_keystone.points[corner][0], g_keystone.points[corner][1]);
}

// Shader source code for keystone correction
static const char* g_vertex_shader_src = 
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texCoord;\n"
    "varying vec2 v_texCoord;\n"
    "void main() {\n"
    "    // Position is already in clip space coordinates (-1 to 1)\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    \n"
    "    // Use the provided texture coordinates directly\n"
    "    v_texCoord = a_texCoord;\n"
    "}\n";

static const char* g_fragment_shader_src = 
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_texture, v_texCoord);\n"
    "}\n";

// Border shader: positions only, uniform color
static const char* g_border_vs_src =
	"attribute vec2 a_position;\n"
	"void main(){\n"
	"  gl_Position = vec4(a_position, 0.0, 1.0);\n"
	"}\n";

static const char* g_border_fs_src =
	"precision mediump float;\n"
	"uniform vec4 u_color;\n"
	"void main(){\n"
	"  gl_FragColor = u_color;\n"
	"}\n";

// Forward declaration for shader compiler utility
static GLuint compile_shader(GLenum shader_type, const char* source);

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

// Compile shader of the specified type
static GLuint compile_shader(GLenum shader_type, const char* source) {
    GLuint shader = glCreateShader(shader_type);
    if (!shader) {
        LOG_ERROR("Failed to create shader of type %d", shader_type);
        return 0;
    }
    
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
			char* info_log = malloc(sizeof(char) * (size_t)info_len);
            glGetShaderInfoLog(shader, info_len, NULL, info_log);
            LOG_ERROR("Error compiling shader: %s", info_log);
            free(info_log);
        }
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

// Initialize keystone shader program
static bool init_keystone_shader() {
    // Compile vertex shader
    g_keystone_vertex_shader = compile_shader(GL_VERTEX_SHADER, g_vertex_shader_src);
    if (!g_keystone_vertex_shader) {
        LOG_ERROR("Failed to compile vertex shader");
        return false;
    }
    
    // Compile fragment shader
    g_keystone_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, g_fragment_shader_src);
    if (!g_keystone_fragment_shader) {
        LOG_ERROR("Failed to compile fragment shader");
        glDeleteShader(g_keystone_vertex_shader);
        g_keystone_vertex_shader = 0;
        return false;
    }
    
    // Create shader program
    g_keystone_shader_program = glCreateProgram();
    if (!g_keystone_shader_program) {
        LOG_ERROR("Failed to create shader program");
        glDeleteShader(g_keystone_vertex_shader);
        glDeleteShader(g_keystone_fragment_shader);
        g_keystone_vertex_shader = 0;
        g_keystone_fragment_shader = 0;
        return false;
    }
    
    // Attach shaders
    glAttachShader(g_keystone_shader_program, g_keystone_vertex_shader);
    glAttachShader(g_keystone_shader_program, g_keystone_fragment_shader);
    
    // Link program
    glLinkProgram(g_keystone_shader_program);
    
    GLint linked;
    glGetProgramiv(g_keystone_shader_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(g_keystone_shader_program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
			char* info_log = malloc(sizeof(char) * (size_t)info_len);
            glGetProgramInfoLog(g_keystone_shader_program, info_len, NULL, info_log);
            LOG_ERROR("Error linking shader program: %s", info_log);
            free(info_log);
        }
        glDeleteProgram(g_keystone_shader_program);
        glDeleteShader(g_keystone_vertex_shader);
        glDeleteShader(g_keystone_fragment_shader);
        g_keystone_shader_program = 0;
        g_keystone_vertex_shader = 0;
        g_keystone_fragment_shader = 0;
        return false;
    }
    
    // Get attribute and uniform locations
    g_keystone_a_position_loc = glGetAttribLocation(g_keystone_shader_program, "a_position");
    g_keystone_a_texcoord_loc = glGetAttribLocation(g_keystone_shader_program, "a_texCoord");
    g_keystone_u_texture_loc = glGetUniformLocation(g_keystone_shader_program, "u_texture");
    
    // Create vertex buffer for the quad
    glGenBuffers(1, &g_keystone_vertex_buffer);
    
    LOG_INFO("Keystone shader program initialized successfully");
    return true;
}

// Free allocated mesh resources
static void cleanup_mesh_resources(void) {
    if (g_keystone.mesh_points) {
        for (int i = 0; i < g_keystone.mesh_size; i++) {
            if (g_keystone.mesh_points[i]) {
                free(g_keystone.mesh_points[i]);
            }
        }
        free(g_keystone.mesh_points);
        g_keystone.mesh_points = NULL;
    }
}

// Cleanup keystone shader resources
static void cleanup_keystone_shader(void) {
    if (g_keystone_vertex_buffer) {
        glDeleteBuffers(1, &g_keystone_vertex_buffer);
        g_keystone_vertex_buffer = 0;
    }
    
    if (g_keystone_texcoord_buffer) {
        glDeleteBuffers(1, &g_keystone_texcoord_buffer);
        g_keystone_texcoord_buffer = 0;
    }
    
    if (g_keystone_shader_program) {
        glDeleteProgram(g_keystone_shader_program);
        g_keystone_shader_program = 0;
    }
    
    if (g_keystone_vertex_shader) {
        glDeleteShader(g_keystone_vertex_shader);
        g_keystone_vertex_shader = 0;
    }
    
    if (g_keystone_fragment_shader) {
        glDeleteShader(g_keystone_fragment_shader);
        g_keystone_fragment_shader = 0;
    }
    
	// Cached index buffer
	if (g_keystone_index_buffer) {
		glDeleteBuffers(1, &g_keystone_index_buffer);
		g_keystone_index_buffer = 0;
	}

	// Cached FBO/texture
	if (g_keystone_fbo) {
		glDeleteFramebuffers(1, &g_keystone_fbo);
		g_keystone_fbo = 0;
	}
	if (g_keystone_fbo_texture) {
		glDeleteTextures(1, &g_keystone_fbo_texture);
		g_keystone_fbo_texture = 0;
	}
	g_keystone_fbo_w = g_keystone_fbo_h = 0;

	// Border shader
	if (g_border_shader_program) { glDeleteProgram(g_border_shader_program); g_border_shader_program=0; }
	if (g_border_vertex_shader) { glDeleteShader(g_border_vertex_shader); g_border_vertex_shader=0; }
	if (g_border_fragment_shader) { glDeleteShader(g_border_fragment_shader); g_border_fragment_shader=0; }

    // Cleanup mesh resources
    cleanup_mesh_resources();
}

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

static bool keystone_handle_key(char key) {
	// Handle multi-character escape sequences for arrow keys
	LOG_DEBUG("keystone_handle_key key=%d(0x%02x) '%c' esc=%d br=%d", 
			 (int)key, (int)key, (key >= 32 && key < 127) ? key : '?', 
			 g_key_seq_state.in_escape_seq, g_key_seq_state.in_bracket_seq);
             
    if (g_key_seq_state.in_escape_seq) {
        if (g_key_seq_state.in_bracket_seq) {
            // This is the final character of an escape sequence
            g_key_seq_state.in_escape_seq = false;
            g_key_seq_state.in_bracket_seq = false;
            
            // Only process arrow keys if keystone mode is enabled
            if (g_keystone.enabled) {
				LOG_DEBUG("Processing arrow key: %d (0x%02x) '%c'", (int)key, (int)key, key);
                float step = (float)g_keystone_adjust_step / 1000.0f; // Convert to 0-1 range
                
                switch (key) {
                    case ARROW_UP:    // Up arrow
                        if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                            keystone_adjust_mesh_point(g_keystone.active_mesh_point[0], 
                                                      g_keystone.active_mesh_point[1], 
                                                      0.0f, -step);
                        } else {
                            keystone_adjust_corner(g_keystone.active_corner, 0.0f, -step);
                        }
						LOG_DEBUG("Point adjusted (UP)");
                        return true;
                        
                    case ARROW_DOWN:  // Down arrow
                        if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                            keystone_adjust_mesh_point(g_keystone.active_mesh_point[0], 
                                                      g_keystone.active_mesh_point[1], 
                                                      0.0f, step);
                        } else {
                            keystone_adjust_corner(g_keystone.active_corner, 0.0f, step);
                        }
						LOG_DEBUG("Point adjusted (DOWN)");
                        return true;
                        
                    case ARROW_LEFT:  // Left arrow
                        if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                            keystone_adjust_mesh_point(g_keystone.active_mesh_point[0], 
                                                      g_keystone.active_mesh_point[1], 
                                                      -step, 0.0f);
                        } else {
                            keystone_adjust_corner(g_keystone.active_corner, -step, 0.0f);
                        }
						LOG_DEBUG("Point adjusted (LEFT)");
                        return true;
                        
                    case ARROW_RIGHT: // Right arrow
                        if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                            keystone_adjust_mesh_point(g_keystone.active_mesh_point[0], 
                                                      g_keystone.active_mesh_point[1], 
                                                      step, 0.0f);
                        } else {
                            keystone_adjust_corner(g_keystone.active_corner, step, 0.0f);
                        }
						LOG_DEBUG("Point adjusted (RIGHT)");
                        return true;
                }
            }
            return false;
        } else if (key == '[') {
            // This is the [ character after ESC
            g_key_seq_state.in_bracket_seq = true;
            return false;
        } else {
            // Not a sequence we recognize, reset state
            g_key_seq_state.in_escape_seq = false;
            return false;
        }
    } else if (key == ESC_CHAR) {
        // Start of a potential escape sequence
        g_key_seq_state.in_escape_seq = true;
        g_key_seq_state.in_bracket_seq = false;
        return false;
    }
    
    // Regular key handling
    if (!g_keystone.enabled) {
        // Only enable keystone mode with 'k' key
        if (key == 'k') {
            LOG_INFO("Enabling keystone mode");
            g_keystone.enabled = true;
            g_keystone.active_corner = 0; // Start with the first corner
            keystone_update_matrix();
            LOG_INFO("Keystone correction enabled, adjusting corner %d", g_keystone.active_corner + 1);
            return true;
        }
        LOG_INFO("Keystone mode not enabled, ignoring key");
        return false;
    }
    
    // When keystone mode is active
    float step = (float)g_keystone_adjust_step / 1000.0f; // Convert to 0-1 range
    
    switch (key) {
        case 'k': // Toggle keystone mode
            g_keystone.enabled = false;
            g_keystone.active_corner = -1;
			LOG_INFO("Keystone correction disabled");
            return true;
            
        case '1': case '2': case '3': case '4': // Select corner (keystone 0)
            if (g_num_videos == 1) {
                // Single video mode: select corner 0-3
                g_keystone.active_corner = key - '1';
                g_keystone.active_mesh_point[0] = -1;
                g_keystone.active_mesh_point[1] = -1;
                LOG_INFO("Adjusting corner %d", g_keystone.active_corner + 1);
            } else {
                // Multi-video mode: keys 1-4 select corners on keystone 0
                int local_corner = key - '1';
                g_active_corner_global = CORNER_GLOBAL(0, local_corner);
                g_videos[0].keystone.active_corner = local_corner;
                g_videos[1].keystone.active_corner = -1;  // Deselect other keystone
                LOG_INFO("Adjusting keystone 1, corner %d (global %d)", local_corner + 1, g_active_corner_global + 1);
            }
            return true;
            
        case '5': case '6': case '7': case '8': // Select corner (keystone 1, multi-video only)
            if (g_num_videos > 1) {
                int local_corner = key - '5';
                g_active_corner_global = CORNER_GLOBAL(1, local_corner);
                g_videos[1].keystone.active_corner = local_corner;
                g_videos[0].keystone.active_corner = -1;  // Deselect other keystone
                LOG_INFO("Adjusting keystone 2, corner %d (global %d)", local_corner + 1, g_active_corner_global + 1);
                return true;
            }
            break;
            
        case '\t': // Tab: cycle through all corners
            if (g_num_videos == 1) {
                // Single video: cycle 0-3
                g_keystone.active_corner = (g_keystone.active_corner + 1) % 4;
                LOG_INFO("Adjusting corner %d", g_keystone.active_corner + 1);
            } else {
                // Multi-video: cycle 0-7
                g_active_corner_global = (g_active_corner_global + 1) % (g_num_videos * 4);
                int video_idx = CORNER_VIDEO(g_active_corner_global);
                int local_corner = CORNER_LOCAL(g_active_corner_global);
                
                // Update active corners on each keystone
                for (int i = 0; i < g_num_videos; i++) {
                    if (i == video_idx) {
                        g_videos[i].keystone.active_corner = local_corner;
                    } else {
                        g_videos[i].keystone.active_corner = -1;
                    }
                }
                LOG_INFO("Adjusting keystone %d, corner %d (global %d)", video_idx + 1, local_corner + 1, g_active_corner_global + 1);
            }
            return true;
            
        case '!': case '@': case '#': case '$': // Pin/unpin corners (shift+1,2,3,4)
            {
                int corner = -1;
                if (key == '!') corner = 0;      // Shift+1
                else if (key == '@') corner = 1; // Shift+2
                else if (key == '#') corner = 2; // Shift+3
                else if (key == '$') corner = 3; // Shift+4
                
                if (corner >= 0) {
                    keystone_toggle_pin(corner);
			LOG_INFO("Corner %d pin %s", corner + 1, 
				g_keystone.perspective_pins[corner] ? "enabled" : "disabled");
                    return true;
                }
            }
            break;
            
        case 'm': // Toggle mesh warping mode
            g_keystone.mesh_enabled = !g_keystone.mesh_enabled;
            LOG_INFO("Mesh warping %s", g_keystone.mesh_enabled ? "enabled" : "disabled");
            
            // If enabling, ensure mesh points are allocated
            if (g_keystone.mesh_enabled && !g_keystone.mesh_points) {
				g_keystone.mesh_points = malloc((size_t)g_keystone.mesh_size * sizeof(float*));
                if (g_keystone.mesh_points) {
                    for (int i = 0; i < g_keystone.mesh_size; i++) {
						g_keystone.mesh_points[i] = malloc((size_t)g_keystone.mesh_size * 2 * sizeof(float));
                        if (g_keystone.mesh_points[i]) {
                            // Initialize regular grid
                            for (int j = 0; j < g_keystone.mesh_size; j++) {
								float x = (float)j / (float)(g_keystone.mesh_size - 1);
								float y = (float)i / (float)(g_keystone.mesh_size - 1);
                                g_keystone.mesh_points[i][j*2] = x;
                                g_keystone.mesh_points[i][j*2+1] = y;
                            }
                        }
                    }
                }
            }
            
            if (g_keystone.mesh_enabled) {
                // Select first mesh point when enabling
                g_keystone.active_mesh_point[0] = 0;
                g_keystone.active_mesh_point[1] = 0;
				LOG_INFO("Mesh warping enabled, selected point [0,0]");
            } else {
                // Deselect mesh point when disabling
                g_keystone.active_mesh_point[0] = -1;
                g_keystone.active_mesh_point[1] = -1;
				LOG_INFO("Mesh warping disabled, using corner-based keystone");
            }
            return true;
            
        case '+': // Increase mesh resolution
            if (g_keystone.mesh_enabled && g_keystone.mesh_size < 10) {
                int new_size = g_keystone.mesh_size + 1;
                
                // Clean up old mesh
                cleanup_mesh_resources();
                
                // Allocate new mesh
                g_keystone.mesh_size = new_size;
				g_keystone.mesh_points = malloc((size_t)new_size * sizeof(float*));
                if (g_keystone.mesh_points) {
                    for (int i = 0; i < new_size; i++) {
						g_keystone.mesh_points[i] = malloc((size_t)new_size * 2 * sizeof(float));
                        if (g_keystone.mesh_points[i]) {
                            // Initialize grid
                            for (int j = 0; j < new_size; j++) {
								float x = (float)j / (float)(new_size - 1);
								float y = (float)i / (float)(new_size - 1);
                                g_keystone.mesh_points[i][j*2] = x;
                                g_keystone.mesh_points[i][j*2+1] = y;
                            }
                        }
                    }
                }
                
                // Reset active point
                g_keystone.active_mesh_point[0] = 0;
                g_keystone.active_mesh_point[1] = 0;
                
				LOG_INFO("Mesh size increased to %dx%d", new_size, new_size);
                return true;
            }
            break;
            
        case '-': // Decrease mesh resolution
            if (g_keystone.mesh_enabled && g_keystone.mesh_size > 2) {
                int new_size = g_keystone.mesh_size - 1;
                
                // Clean up old mesh
                cleanup_mesh_resources();
                
                // Allocate new mesh
                g_keystone.mesh_size = new_size;
				g_keystone.mesh_points = malloc((size_t)new_size * sizeof(float*));
                if (g_keystone.mesh_points) {
                    for (int i = 0; i < new_size; i++) {
						g_keystone.mesh_points[i] = malloc((size_t)new_size * 2 * sizeof(float));
                        if (g_keystone.mesh_points[i]) {
                            // Initialize grid
                            for (int j = 0; j < new_size; j++) {
								float x = (float)j / (float)(new_size - 1);
								float y = (float)i / (float)(new_size - 1);
                                g_keystone.mesh_points[i][j*2] = x;
                                g_keystone.mesh_points[i][j*2+1] = y;
                            }
                        }
                    }
                }
                
                // Reset active point
                g_keystone.active_mesh_point[0] = 0;
                g_keystone.active_mesh_point[1] = 0;
                
				LOG_INFO("Mesh size decreased to %dx%d", new_size, new_size);
                return true;
            }
            break;
            
        case 'q': // Previous mesh point
            if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                g_keystone.active_mesh_point[1]--;
                if (g_keystone.active_mesh_point[1] < 0) {
                    g_keystone.active_mesh_point[1] = g_keystone.mesh_size - 1;
                    g_keystone.active_mesh_point[0]--;
                    if (g_keystone.active_mesh_point[0] < 0) {
                        g_keystone.active_mesh_point[0] = g_keystone.mesh_size - 1;
                    }
                }
		LOG_INFO("Selected mesh point [%d,%d]", 
			g_keystone.active_mesh_point[0], g_keystone.active_mesh_point[1]);
                return true;
            }
            break;
            
        case 'e': // Next mesh point
            if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                g_keystone.active_mesh_point[1]++;
                if (g_keystone.active_mesh_point[1] >= g_keystone.mesh_size) {
                    g_keystone.active_mesh_point[1] = 0;
                    g_keystone.active_mesh_point[0]++;
                    if (g_keystone.active_mesh_point[0] >= g_keystone.mesh_size) {
                        g_keystone.active_mesh_point[0] = 0;
                    }
                }
		LOG_INFO("Selected mesh point [%d,%d]", 
			g_keystone.active_mesh_point[0], g_keystone.active_mesh_point[1]);
                return true;
            }
            break;
            
        case 'w': // Move active point up
            if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                keystone_adjust_mesh_point(g_keystone.active_mesh_point[0], 
                                           g_keystone.active_mesh_point[1], 
                                           0.0f, -step);
            } else {
                keystone_adjust_corner(g_keystone.active_corner, 0.0f, -step);
            }
			LOG_DEBUG("Point adjusted (W)");
            return true;
            
        case 's': // Move active point down
            if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                keystone_adjust_mesh_point(g_keystone.active_mesh_point[0], 
                                           g_keystone.active_mesh_point[1], 
                                           0.0f, step);
            } else {
                keystone_adjust_corner(g_keystone.active_corner, 0.0f, step);
            }
			LOG_DEBUG("Point adjusted (S)");
            return true;
            
        case 'a': // Move active point left
            if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                keystone_adjust_mesh_point(g_keystone.active_mesh_point[0], 
                                           g_keystone.active_mesh_point[1], 
                                           -step, 0.0f);
            } else {
                keystone_adjust_corner(g_keystone.active_corner, -step, 0.0f);
            }
			LOG_DEBUG("Point adjusted (A)");
            return true;
            
        case 'd': // Move active point right
            if (g_keystone.mesh_enabled && g_keystone.active_mesh_point[0] >= 0) {
                keystone_adjust_mesh_point(g_keystone.active_mesh_point[0], 
                                           g_keystone.active_mesh_point[1], 
                                           step, 0.0f);
            } else {
                keystone_adjust_corner(g_keystone.active_corner, step, 0.0f);
            }
			LOG_DEBUG("Point adjusted (D)");
            return true;
            
        case 'r': // Reset to default
            if (g_num_videos > 1) {
                // Multi-video mode: reset the currently active keystone
                int video_idx = CORNER_VIDEO(g_active_corner_global);
                if (video_idx >= 0 && video_idx < g_num_videos) {
                    keystone_t *ks = &g_videos[video_idx].keystone;
                    
                    // Reset to default split position for this video
                    if (video_idx == 0) {
                        // Video 0: left half
                        ks->points[0][0] = 0.0f; ks->points[0][1] = 0.0f; // Top-left
                        ks->points[1][0] = 0.5f; ks->points[1][1] = 0.0f; // Top-right
                        ks->points[2][0] = 0.5f; ks->points[2][1] = 1.0f; // Bottom-right
                        ks->points[3][0] = 0.0f; ks->points[3][1] = 1.0f; // Bottom-left
                    } else {
                        // Video 1: right half
                        ks->points[0][0] = 0.5f; ks->points[0][1] = 0.0f; // Top-left
                        ks->points[1][0] = 1.0f; ks->points[1][1] = 0.0f; // Top-right
                        ks->points[2][0] = 1.0f; ks->points[2][1] = 1.0f; // Bottom-right
                        ks->points[3][0] = 0.5f; ks->points[3][1] = 1.0f; // Bottom-left
                    }
                    
                    // Reset pins
                    for (int i = 0; i < 4; i++) {
                        ks->perspective_pins[i] = false;
                    }
                    
                    keystone_update_matrix_for(ks);
                    LOG_INFO("Reset keystone %d to default position", video_idx + 1);
                }
                return true;
            }
            {
                // Single video mode: original behavior
                // Save the current enabled state
                bool was_enabled = g_keystone.enabled;
                bool was_mesh_enabled = g_keystone.mesh_enabled;
                
                // Reset corner positions
                g_keystone.points[0][0] = 0.0f; g_keystone.points[0][1] = 0.0f; // Top-left
                g_keystone.points[1][0] = 1.0f; g_keystone.points[1][1] = 0.0f; // Top-right
                g_keystone.points[2][0] = 1.0f; g_keystone.points[2][1] = 1.0f; // Bottom-right
                g_keystone.points[3][0] = 0.0f; g_keystone.points[3][1] = 1.0f; // Bottom-left
                
                // Reset pins
                for (int i = 0; i < 4; i++) {
                    g_keystone.perspective_pins[i] = false;
                }
                
                // Reset mesh if enabled
                if (was_mesh_enabled && g_keystone.mesh_points) {
                    for (int i = 0; i < g_keystone.mesh_size; i++) {
                        if (g_keystone.mesh_points[i]) {
                            for (int j = 0; j < g_keystone.mesh_size; j++) {
								float x = (float)j / (float)(g_keystone.mesh_size - 1);
								float y = (float)i / (float)(g_keystone.mesh_size - 1);
                                g_keystone.mesh_points[i][j*2] = x;
                                g_keystone.mesh_points[i][j*2+1] = y;
                            }
                        }
                    }
                }
                
                // Restore enabled states
                g_keystone.enabled = was_enabled;
                g_keystone.mesh_enabled = was_mesh_enabled;
                
                // Update the transformation matrix
                keystone_update_matrix();
                
				LOG_INFO("Reset to default positions");
                return true;
            }
            
            LOG_INFO("Keystone reset to default rectangle");
            return true;
            
		case '>': // Increase adjustment step
			g_keystone_adjust_step = (g_keystone_adjust_step * 2 > 100) ? 100 : (g_keystone_adjust_step * 2);
            LOG_INFO("Keystone step increased to %d", g_keystone_adjust_step);
            return true;
            
		case '<': // Decrease adjustment step
			g_keystone_adjust_step = (g_keystone_adjust_step / 2 < 1) ? 1 : (g_keystone_adjust_step / 2);
            LOG_INFO("Keystone step decreased to %d", g_keystone_adjust_step);
            return true;
            
        case 'b': // Toggle border
            g_show_border = !g_show_border;
            LOG_INFO("Border %s", g_show_border ? "enabled" : "disabled");
            return true;

		case 'c': // Toggle corner markers
			g_show_corner_markers = !g_show_corner_markers;
			LOG_INFO("Corner markers %s", g_show_corner_markers ? "enabled" : "disabled");
			return true;

		case 'o': // Toggle horizontal flip
			g_tex_flip_x = !g_tex_flip_x;
			LOG_INFO("Texture flip X %s", g_tex_flip_x ? "ON" : "off");
			return true;

		case 'p': // Toggle vertical flip
			g_tex_flip_y = !g_tex_flip_y;
			LOG_INFO("Texture flip Y %s", g_tex_flip_y ? "ON" : "off");
			return true;
            
        case '[': // Decrease border width
			if (g_show_border) {
				g_border_width = (g_border_width - 1 < 1) ? 1 : (g_border_width - 1);
                LOG_INFO("Border width decreased to %d", g_border_width);
            }
            return true;
            
        case ']': // Increase border width
			if (g_show_border) {
				g_border_width = (g_border_width + 1 > 50) ? 50 : (g_border_width + 1);
                LOG_INFO("Border width increased to %d", g_border_width);
            }
            return true;
            
		// 'g' background toggle removed; background is always black
		case 'g':
			return false;
            
        case 'S': // Save keystone configuration to local file
            if (g_num_videos > 1) {
                // Multi-video mode: save each keystone to its own file
                bool all_ok = true;
                for (int i = 0; i < g_num_videos; i++) {
                    if (!keystone_save_instance_config(&g_videos[i])) {
                        LOG_ERROR("Failed to save keystone %d configuration", i + 1);
                        all_ok = false;
                    }
                }
                if (all_ok) {
                    LOG_INFO("Keystone configurations saved to keystone_0.conf and keystone_1.conf");
                }
            } else {
                if (keystone_save_config("./keystone.conf")) {
                    LOG_INFO("Keystone configuration saved to local file: keystone.conf");
                } else {
                    LOG_ERROR("Failed to save keystone configuration to local file");
                }
            }
            return true;
            
		default:
			return false;
    }
	// If we hit a non-returning case due to unmet conditions (e.g., '+' when mesh disabled),
	// fall back to not-handled to avoid control reaching end of non-void function.
	return false;
}

/**
 * Save keystone configuration to a specified file path
 * 
 * @param path The file path to save the configuration to
 * @return true if the configuration was saved successfully, false otherwise
 */
static bool keystone_save_config(const char* path) {
    if (!path) return false;
    
    FILE* f = fopen(path, "w");
    if (!f) {
        LOG_ERROR("Failed to open file for writing: %s (%s)", path, strerror(errno));
        return false;
    }
    
    fprintf(f, "# Pickle keystone configuration\n");
    fprintf(f, "enabled=%d\n", g_keystone.enabled ? 1 : 0);
    fprintf(f, "mesh_enabled=%d\n", g_keystone.mesh_enabled ? 1 : 0);
    fprintf(f, "mesh_size=%d\n", g_keystone.mesh_size);
    
    // Save the corner points
    for (int i = 0; i < 4; i++) {
        fprintf(f, "corner%d=%.6f,%.6f\n", i+1, g_keystone.points[i][0], g_keystone.points[i][1]);
        fprintf(f, "pin%d=%d\n", i+1, g_keystone.perspective_pins[i] ? 1 : 0);
    }
    
    // Save border and corner marker settings
    fprintf(f, "border=%d\n", g_show_border ? 1 : 0);
    fprintf(f, "cornermarks=%d\n", g_show_corner_markers ? 1 : 0);
    
    // Save mesh points if mesh warping is enabled
    if (g_keystone.mesh_enabled && g_keystone.mesh_points) {
        for (int i = 0; i < g_keystone.mesh_size; i++) {
            for (int j = 0; j < g_keystone.mesh_size; j++) {
                if (g_keystone.mesh_points[i]) {
                    fprintf(f, "mesh_%d_%d=%.6f,%.6f\n", i, j, 
                            g_keystone.mesh_points[i][j*2],
                            g_keystone.mesh_points[i][j*2+1]);
                }
            }
        }
    }
    
    fclose(f);
    
    return true;
}

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
static void bo_destroy_handler(struct gbm_bo *bo, void *data) {
	(void)bo;
	struct fb_holder *h = data;
	if (h) {
		if (h->fb) drmModeRmFB(h->fd, h->fb);
		free(h);
	}
}

/**
 * Update a video instance's FBO by rendering from mpv
 * This is the expensive operation we want to do less frequently in dual-video mode
 */
static bool update_video_fbo(video_instance_t *inst, int screen_w, int screen_h) {
	if (!inst || !inst->player || !inst->player->rctx) return false;
	
	mpv_player_t *p = inst->player;
	
	// In dual-video mode, use quarter resolution to reduce GPU load significantly
	// mpv renders at 960x540, keystone shader upscales to screen
	// This reduces pixel fill by 75% per video
	int want_w = screen_w;
	int want_h = screen_h;
	if (g_num_videos > 1) {
		want_w = screen_w / 2;
		want_h = screen_h / 2;  // Quarter resolution (half width × half height)
	}
	bool need_recreate = (inst->fbo == 0) || (inst->fbo_w != want_w) || (inst->fbo_h != want_h);
	if (need_recreate) {
		if (inst->fbo) {
			glDeleteFramebuffers(1, &inst->fbo);
			inst->fbo = 0;
		}
		if (inst->fbo_texture) {
			glDeleteTextures(1, &inst->fbo_texture);
			inst->fbo_texture = 0;
		}
		glGenTextures(1, &inst->fbo_texture);
		glBindTexture(GL_TEXTURE_2D, inst->fbo_texture);
		// Use LINEAR filtering for smooth upscaling from quarter-res FBO
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, want_w, want_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		
		glGenFramebuffers(1, &inst->fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, inst->fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->fbo_texture, 0);
		
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			LOG_ERROR("Instance %d FBO setup failed, status: %d", inst->index, status);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &inst->fbo);
			glDeleteTextures(1, &inst->fbo_texture);
			inst->fbo = 0;
			inst->fbo_texture = 0;
			return false;
		}
		inst->fbo_w = want_w;
		inst->fbo_h = want_h;
	}
	
	// Render mpv to this instance's FBO
	glBindFramebuffer(GL_FRAMEBUFFER, inst->fbo);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // Transparent black
	glClear(GL_COLOR_BUFFER_BIT);
	
	mpv_opengl_fbo mpv_fbo = { .fbo = (int)inst->fbo, .w = inst->fbo_w, .h = inst->fbo_h, .internal_format = 0 };
	int mpv_flip_y = 0;
	mpv_render_param r_params[] = {
		{MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
		{MPV_RENDER_PARAM_FLIP_Y, &mpv_flip_y},
		{0}
	};
	mpv_render_context_render(p->rctx, r_params);
	
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

// Render both videos (already composed by lavfi) into a single composite FBO
static bool update_composite_fbo(mpv_player_t *p, int screen_w, int screen_h) {
	if (!p || !p->rctx) return false;

	// Composite output at half height to reduce fill; width matches screen for keystone mapping
	int want_w = screen_w;
	int want_h = screen_h / 2; // 1920x540 for 1080p
	bool need_recreate = (g_composite_fbo == 0) || (g_composite_w != want_w) || (g_composite_h != want_h);
	if (need_recreate) {
		if (g_composite_fbo) { glDeleteFramebuffers(1, &g_composite_fbo); g_composite_fbo = 0; }
		if (g_composite_texture) { glDeleteTextures(1, &g_composite_texture); g_composite_texture = 0; }
		glGenTextures(1, &g_composite_texture);
		glBindTexture(GL_TEXTURE_2D, g_composite_texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, want_w, want_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

		glGenFramebuffers(1, &g_composite_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, g_composite_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_composite_texture, 0);
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			LOG_ERROR("Composite FBO setup failed, status: %d", status);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &g_composite_fbo);
			glDeleteTextures(1, &g_composite_texture);
			g_composite_fbo = 0; g_composite_texture = 0;
			return false;
		}
		g_composite_w = want_w;
		g_composite_h = want_h;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, g_composite_fbo);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	mpv_opengl_fbo mpv_fbo = { .fbo = (int)g_composite_fbo, .w = g_composite_w, .h = g_composite_h, .internal_format = 0 };
	int mpv_flip_y = 0;
	mpv_render_param r_params[] = {
		{MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
		{MPV_RENDER_PARAM_FLIP_Y, &mpv_flip_y},
		{0}
	};
	mpv_render_context_render(p->rctx, r_params);

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return true;
}

/**
 * Render the keystone quad for a video instance using its cached FBO texture
 * This is cheap and can be done every frame
 */
static bool render_keystone_quad(video_instance_t *inst, int screen_w, int screen_h) {
	if (!inst || inst->fbo_texture == 0) return false;
	
	keystone_t *ks = &inst->keystone;
	
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	
	glUseProgram(g_keystone_shader_program);
	
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, inst->fbo_texture);
	glUniform1i(g_keystone_u_texture_loc, 0);
	
	// Convert keystone points to clip space
	float vertices[] = {
		ks->points[0][0] * 2.0f - 1.0f, 1.0f - (ks->points[0][1] * 2.0f),  // Top left 
		ks->points[1][0] * 2.0f - 1.0f, 1.0f - (ks->points[1][1] * 2.0f),  // Top right
		ks->points[3][0] * 2.0f - 1.0f, 1.0f - (ks->points[3][1] * 2.0f),  // Bottom left 
		ks->points[2][0] * 2.0f - 1.0f, 1.0f - (ks->points[2][1] * 2.0f)   // Bottom right
	};
	
	float u0 = inst->use_subrect ? inst->u0 : 0.0f;
	float u1 = inst->use_subrect ? inst->u1 : 1.0f;
	float v0 = inst->use_subrect ? inst->v0 : 0.0f;
	float v1 = inst->use_subrect ? inst->v1 : 1.0f;
	float texcoords[] = {
		u0, v0,  // Top left
		u1, v0,  // Top right
		u0, v1,  // Bottom left
		u1, v1   // Bottom right
	};
	
	glEnableVertexAttribArray((GLuint)g_keystone_a_position_loc);
	glBindBuffer(GL_ARRAY_BUFFER, g_keystone_vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
	glVertexAttribPointer((GLuint)g_keystone_a_position_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
	
	if (g_keystone_texcoord_buffer == 0) {
		glGenBuffers(1, &g_keystone_texcoord_buffer);
	}
	glBindBuffer(GL_ARRAY_BUFFER, g_keystone_texcoord_buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray((GLuint)g_keystone_a_texcoord_loc);
	glVertexAttribPointer((GLuint)g_keystone_a_texcoord_loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
	
	if (g_keystone_index_buffer == 0) {
		GLushort indices[] = {0, 1, 2, 2, 1, 3};
		glGenBuffers(1, &g_keystone_index_buffer);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_keystone_index_buffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
	} else {
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_keystone_index_buffer);
	}
	
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
	
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glDisableVertexAttribArray((GLuint)g_keystone_a_position_loc);
	glDisableVertexAttribArray((GLuint)g_keystone_a_texcoord_loc);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
	glDisable(GL_BLEND);
	
	// Draw corner markers for this keystone (always show when enabled)
	if (g_show_corner_markers) {
		int corner_size = 12;
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		
		for (int i = 0; i < 4; i++) {
			int x = (int)(ks->points[i][0] * (float)screen_w);
			int y = (int)(ks->points[i][1] * (float)screen_h);
			
			// Color: active corner = yellow, inactive corners = green/blue by keystone
			if (i == ks->active_corner) {
				glClearColor(1.0f, 1.0f, 0.0f, 0.9f);  // Yellow for active corner
			} else if (inst->index == 0) {
				glClearColor(0.0f, 0.7f, 0.0f, 0.6f);  // Green for keystone 0
			} else {
				glClearColor(0.0f, 0.4f, 0.8f, 0.6f);  // Blue for keystone 1
			}
			
			x = x - corner_size/2;
			y = y - corner_size/2;
			if (x < 0) x = 0; else if (x > screen_w - corner_size) x = screen_w - corner_size;
			if (y < 0) y = 0; else if (y > screen_h - corner_size) y = screen_h - corner_size;
			
			glScissor(x, screen_h - y - corner_size, corner_size, corner_size);
			glEnable(GL_SCISSOR_TEST);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
	}
	
	return true;
}

/**
 * Render a single video instance with its keystone to the current framebuffer
 * Used in multi-video mode (legacy wrapper, now uses split functions)
 * 
 * @param d DRM context
 * @param e EGL context
 * @param inst Video instance to render
 * @param screen_w Screen width
 * @param screen_h Screen height
 * @return true on success
 */
/**
 * Combined render function - updates FBO and renders keystone quad
 * Used for single video mode or when alternate frame mode is disabled
 */
static bool render_video_instance(kms_ctx_t *d, egl_ctx_t *e, video_instance_t *inst, int screen_w, int screen_h) {
	(void)d; (void)e;  // Unused parameters, kept for API compatibility
	if (!inst || !inst->player || !inst->player->rctx) return false;
	
	// Update FBO from mpv
	if (!update_video_fbo(inst, screen_w, screen_h)) return false;
	
	// Render keystone quad to screen
	return render_keystone_quad(inst, screen_w, screen_h);
}

static bool render_frame_fixed(kms_ctx_t *d, egl_ctx_t *e, mpv_player_t *p) {
	if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
		fprintf(stderr, "eglMakeCurrent failed\n"); return false; 
	}
	
	// Initialize keystone shader if needed
	bool any_keystone = g_keystone.enabled || (g_num_videos > 1);
	if (any_keystone && g_keystone_shader_program == 0) {
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
	
	// Multi-video mode: render each video instance with its own keystone
	if (g_num_videos > 1) {
		int screen_w = (int)d->mode.hdisplay;
		int screen_h = (int)d->mode.vdisplay;
		
		// Update active_corner for each keystone based on g_active_corner_global
		// Each keystone only has its corner active if it's the currently selected keystone
		for (int i = 0; i < g_num_videos; i++) {
			int current_video = CORNER_VIDEO(g_active_corner_global);
			if (i == current_video) {
				g_videos[i].keystone.active_corner = CORNER_LOCAL(g_active_corner_global);
			} else {
				g_videos[i].keystone.active_corner = -1;  // Not active
			}
		}
		
		if (g_single_mpv_mode) {
			// Single mpv: render composite once, then two keystones sampling sub-rects
			if (!update_composite_fbo(g_videos[0].player, screen_w, screen_h)) {
				LOG_WARN("Failed to update composite FBO");
			}
			for (int i = 0; i < g_num_videos; i++) {
				g_videos[i].fbo_texture = g_composite_texture;
				if (!render_keystone_quad(&g_videos[i], screen_w, screen_h)) {
					LOG_WARN("Failed to render keystone quad for video %d", i);
				}
			}
		} else if (g_alternate_frame_mode && g_num_videos == 2) {
			int video_to_update = g_render_frame_count % 2;
			g_render_frame_count++;
			if (!update_video_fbo(&g_videos[video_to_update], screen_w, screen_h)) {
				LOG_WARN("Failed to update FBO for video instance %d", video_to_update);
			}
			for (int i = 0; i < g_num_videos; i++) {
				if (g_videos[i].fbo_texture != 0) {
					if (!render_keystone_quad(&g_videos[i], screen_w, screen_h)) {
						LOG_WARN("Failed to render keystone quad for video %d", i);
					}
				}
			}
		} else {
			for (int i = 0; i < g_num_videos; i++) {
				if (!render_video_instance(d, e, &g_videos[i], screen_w, screen_h)) {
					LOG_WARN("Failed to render video instance %d", i);
				}
			}
		}
		
		// Skip single-video rendering path below
		goto do_swap;
	}
	
	// Single video mode: use legacy keystone rendering
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
			// Create texture - use RGB instead of RGBA to save bandwidth
			glGenTextures(1, &g_keystone_fbo_texture);
			glBindTexture(GL_TEXTURE_2D, g_keystone_fbo_texture);
			// Use nearest filtering for maximum performance
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			// Use RGBA back - RGB might not be compatible with mpv output
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
	
do_swap:
	// Swap buffers to display the rendered frame
	eglSwapBuffers(e->dpy, e->surf);

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
		if (drmModeSetCrtc(d->fd, d->crtc_id, fb_id, 0,0, &d->connector_id,1,&d->mode)) {
			int err = errno;
			fprintf(stderr, "drmModeSetCrtc failed (%s)\n", strerror(err));
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
		
		if (drmModePageFlip(d->fd, d->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, bo)) {
			gbm_surface_release_buffer(e->gbm_surf, bo);
			return false;
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
		{"stats", no_argument, NULL, 's'},
		{"help", no_argument, NULL, 'h'},
		{"version", no_argument, NULL, 'V'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "lshV", long_options, NULL)) != -1) {
		switch (opt) {
			case 'l':
				g_loop_playback = 1;
				break;
			case 's':
				g_stats_enabled = 1;
				break;
			case 'V':
				fprintf(stderr, "pickle %s (built %s)\n", PICKLE_VERSION_STRING, PICKLE_BUILD_DATE);
				fprintf(stderr, "DRM/KMS + GBM + EGL + libmpv video player for Raspberry Pi 4\n");
				return 0;
			case 'h':
				fprintf(stderr, "pickle %s - DRM/KMS video player for Raspberry Pi 4\n\n", PICKLE_VERSION_STRING);
				fprintf(stderr, "Usage: %s [options] <video-file> [video-file2]\n", argv[0]);
				fprintf(stderr, "Options:\n");
				fprintf(stderr, "  -l, --loop            Loop playback continuously\n");
				fprintf(stderr, "  -s, --stats           Enable performance statistics overlay\n");
				fprintf(stderr, "  -h, --help            Show this help message\n");
				fprintf(stderr, "  -V, --version         Show version information\n");
				fprintf(stderr, "\nDual video mode:\n");
				fprintf(stderr, "  When two video files are specified, each video plays in its own\n");
				fprintf(stderr, "  keystone region. Use Tab to cycle corners (1-8) across both keystones.\n");
				fprintf(stderr, "  Keystone 1 (green corners) = left half, Keystone 2 (blue corners) = right half.\n");
				fprintf(stderr, "  Configs saved to keystone_0.conf and keystone_1.conf respectively.\n");
				fprintf(stderr, "\nSee README.md for environment variables and keystone controls.\n");
				return 0;
			default:
				fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
				return 1;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "Error: No input file specified\n");
		fprintf(stderr, "Usage: %s [options] <video-file> [video-file2]\n", argv[0]);
		return 1;
	}

	// Count video files (1 or 2 supported)
	int num_files = argc - optind;
	if (num_files > MAX_VIDEOS) {
		fprintf(stderr, "Warning: Only %d video files supported, ignoring extras\n", MAX_VIDEOS);
		num_files = MAX_VIDEOS;
	}
	g_num_videos = num_files;
	if (g_num_videos == 1 && g_single_mpv_mode) {
		// Single-mpv mode is only meaningful in dual-video; disable to avoid confusion
		g_single_mpv_mode = 0;
	}
	
	// Target FPS can be set via environment variable for frame pacing
	// By default, let GPU render at natural rate (vsync handles timing)
	const char *target_fps_env = getenv("PICKLE_TARGET_FPS");
	if (target_fps_env && *target_fps_env) {
		g_target_fps = atoi(target_fps_env);
		if (g_target_fps > 0) {
			g_frame_interval_us = 1000000.0 / g_target_fps;
			LOG_INFO("Target FPS: %d (frame interval: %.1f ms)", g_target_fps, g_frame_interval_us / 1000.0);
		}
	}
	
	// Store video file paths
	const char *files[MAX_VIDEOS] = {NULL, NULL};
	for (int i = 0; i < num_files; i++) {
		files[i] = argv[optind + i];
		g_videos[i].video_file = files[i];
	}
	
	// For backward compatibility with single video mode
	const char *file = files[0];
	
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
	signal(SIGTERM, handle_sigterm);  // Graceful shutdown for systemd/docker
	signal(SIGSEGV, handle_sigsegv);
	signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe (prevents crash on audio disconnect)

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
	mpv_player_t player = {0};      // Primary player (for single video mode)
	mpv_player_t player2 = {0};     // Secondary player (for dual video mode)

	// Parse stats env
	const char *stats_env = getenv("PICKLE_STATS");
	if (stats_env && *stats_env && strcmp(stats_env, "0") != 0 && strcasecmp(stats_env, "off") != 0) {
		g_stats_enabled = 1;
		const char *ival = getenv("PICKLE_STATS_INTERVAL");
		if (ival && *ival) {
			double v = atof(ival);
			if (v > 0.05) g_stats_interval_sec = v; // clamp minimal interval
		}
	}
	
	// Initialize stats timer if enabled (by flag or env)
	if (g_stats_enabled) {
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
	
	// Initialize keystone correction based on number of videos
	if (g_num_videos == 1) {
		// Single video mode: use legacy keystone_init()
		keystone_init();
		g_videos[0].player = &player;
		g_videos[0].use_subrect = 0;
	} else {
		// Multi-video mode: initialize each video instance with split keystones
		for (int i = 0; i < g_num_videos; i++) {
			keystone_init_instance(&g_videos[i], i, g_num_videos);
			keystone_load_instance_config(&g_videos[i]);
			keystone_update_matrix_for(&g_videos[i].keystone);
			g_videos[i].use_subrect = 0;
		}
		if (g_single_mpv_mode) {
			// Both keystones sample from the same composite texture; set sub-rects
			g_videos[0].player = &player;
			g_videos[1].player = &player; // shared mpv
			g_videos[0].use_subrect = 1; g_videos[0].u0 = 0.0f; g_videos[0].u1 = 0.5f; g_videos[0].v0 = 0.0f; g_videos[0].v1 = 1.0f;
			g_videos[1].use_subrect = 1; g_videos[1].u0 = 0.5f; g_videos[1].u1 = 1.0f; g_videos[1].v0 = 0.0f; g_videos[1].v1 = 1.0f;
		} else {
			g_videos[0].player = &player;
			g_videos[1].player = &player2;
		}
		// Set initial active corner to video 0, corner 0 (top-left)
		g_active_corner_global = 0;
		g_videos[0].keystone.active_corner = 0;
		fprintf(stderr, "\nDual video mode: %d videos loaded\n", g_num_videos);
	}
	
	// Initialize mpv player(s)
	if (g_num_videos > 1 && g_single_mpv_mode) {
		if (!init_mpv_lavfi_dual(&player, files[0], files[1])) RET("init_mpv_lavfi_dual");
		// Shared player already assigned above
	} else {
		if (!init_mpv(&player, files[0])) RET("init_mpv");
		if (g_num_videos > 1) {
			if (!init_mpv(&player2, files[1])) RET("init_mpv (video 2)");
		}
	}
	// Prime event processing in case mpv already queued wakeups before pipe creation.
	g_mpv_wakeup = 1;

	fprintf(stderr, "pickle %s (built %s)\n", PICKLE_VERSION_STRING, PICKLE_BUILD_DATE);
	if (g_num_videos == 1) {
		fprintf(stderr, "Playing %s at %dx%d %.2f Hz\n", file, drm.mode.hdisplay, drm.mode.vdisplay,
				(drm.mode.vrefresh ? (double)drm.mode.vrefresh : (double)drm.mode.clock / (drm.mode.htotal * drm.mode.vtotal)));
	} else {
		fprintf(stderr, "Playing %d videos at %dx%d %.2f Hz\n", g_num_videos, drm.mode.hdisplay, drm.mode.vdisplay,
				(drm.mode.vrefresh ? (double)drm.mode.vrefresh : (double)drm.mode.clock / (drm.mode.htotal * drm.mode.vtotal)));
		for (int i = 0; i < g_num_videos; i++) {
			fprintf(stderr, "  Video %d: %s\n", i + 1, files[i]);
		}
	}
	
	// Print keystone control instructions
	if (g_num_videos == 1) {
		if (g_keystone.enabled) {
			fprintf(stderr, "\nKeystone correction enabled. Controls:\n");
		} else {
			fprintf(stderr, "\nKeystone correction available. Controls:\n");
		}
		fprintf(stderr, "  k - Toggle keystone mode\n");
		fprintf(stderr, "  1-4 - Select corner to adjust\n");
	} else {
		fprintf(stderr, "\nDual keystone mode. Controls:\n");
		fprintf(stderr, "  Tab - Cycle through all 8 corners (both keystones)\n");
		fprintf(stderr, "  1-4 - Select corner on keystone 1 (green)\n");
		fprintf(stderr, "  5-8 - Select corner on keystone 2 (blue)\n");
	}
	fprintf(stderr, "  w/a/s/d - Move selected corner up/left/down/right\n");
	fprintf(stderr, "  +/- - Increase/decrease adjustment step size\n");
	fprintf(stderr, "  r - Reset current keystone to default\n");
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
		// Drain any pending mpv events BEFORE potentially blocking in poll to avoid startup deadlock
		if (g_mpv_wakeup) {
			g_mpv_wakeup = 0;
			drain_mpv_events(player.mpv);
			if (player.rctx) {
				uint64_t flags = mpv_render_context_update(player.rctx);
				g_mpv_update_flags |= flags;
			}
			// Handle second player in dual-video mode
			if (g_num_videos > 1 && player2.mpv) {
				drain_mpv_events(player2.mpv);
				if (player2.rctx) {
					uint64_t flags = mpv_render_context_update(player2.rctx);
					g_mpv_update_flags |= flags;
				}
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
		
		// Check if we can render a new frame
		// With triple buffering, we could allow more parallelism, but for stability
		// we wait until the current flip completes
		int need_frame = 0;
		if (frames == 0 && !g_pending_flip) need_frame = 1; // guarantee first frame submission
		else if (force_loop && !g_pending_flip) need_frame = 1; // continuous mode
		else if ((g_mpv_update_flags & MPV_RENDER_UPDATE_FRAME) && !g_pending_flip) need_frame = 1;
		
		// Frame pacing: if target FPS is set, throttle frame rate for smooth playback
		if (need_frame && g_target_fps > 0 && frames > 0) {
			struct timeval now;
			gettimeofday(&now, NULL);
			double elapsed_us = (double)(now.tv_sec - g_last_render_time.tv_sec) * 1000000.0 +
			                    (double)(now.tv_usec - g_last_render_time.tv_usec);
			if (elapsed_us < g_frame_interval_us) {
				// Not enough time has passed, skip this frame
				need_frame = 0;
			}
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
				g_pending_flips = 0;
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
							const char *cmd[] = {"cycle-values", "hwdec", "drm-copy", "auto-safe", "no", NULL};
							mpv_command_async(player.mpv, 0, cmd);
							fprintf(stderr, "[wd] cycling hwdec as part of recovery\n");
						}
					}
				}
			}
		}
		if (need_frame) {
			if (g_debug && frames < 10) fprintf(stderr, "[debug] rendering frame #%d flags=0x%llx pending_flip=%d\n", frames, (unsigned long long)g_mpv_update_flags, g_pending_flip);
			if (!render_frame_fixed(&drm, &eglc, &player)) { 
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
	
	// Save keystone settings based on mode
	if (g_num_videos == 1) {
		// Single video mode: save legacy keystone
		if (g_keystone.enabled) {
			if (keystone_save_config("./keystone.conf")) {
				LOG_INFO("Saved keystone configuration to ./keystone.conf");
			} else {
				const char* home = getenv("HOME");
				if (home) {
					char config_path[512];
					snprintf(config_path, sizeof(config_path), "%s/.config", home);
					mkdir(config_path, 0755);
					snprintf(config_path, sizeof(config_path), "%s/.config/pickle_keystone.conf", home);
					if (keystone_save_config(config_path)) {
						LOG_INFO("Saved keystone configuration to %s", config_path);
					}
				}
			}
		}
	} else {
		// Multi-video mode: save each keystone config
		for (int i = 0; i < g_num_videos; i++) {
			if (g_videos[i].keystone.enabled) {
				if (keystone_save_instance_config(&g_videos[i])) {
					LOG_INFO("Saved keystone %d configuration to %s", i, g_videos[i].config_path);
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
	
	destroy_mpv(&player);
	if (g_num_videos > 1) {
		destroy_mpv(&player2);
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
	
	destroy_mpv(&player);
	if (g_num_videos > 1) {
		destroy_mpv(&player2);
	}
	deinit_gbm_egl(&eglc);
	deinit_drm(&drm);
	
	return 1;
}