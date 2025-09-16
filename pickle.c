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

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <sys/ioctl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>

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
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "ERROR: %s failed (%s) at %s:%d\n", msg, strerror(errno), __FILE__, __LINE__); goto fail; } } while (0)
#define RET(msg) do { fprintf(stderr, "ERROR: %s at %s:%d\n", msg, __FILE__, __LINE__); goto fail; } while (0)

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
static void *g_libegl;
static void *g_libgles;
static void *mpv_get_proc_address(void *ctx, const char *name) {
	(void)ctx;
	if (!g_libegl) {
		g_libegl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
		if (!g_libegl) g_libegl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
	}
	if (!g_libgles) {
		g_libgles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
		if (!g_libgles) g_libgles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
	}
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

static int g_have_master = 0; // set if we successfully become DRM master

static bool ensure_drm_master(int fd) {
	// Attempt to become DRM master; non-fatal if it fails (we may still page flip if compositor allows)
	if (drmSetMaster(fd) == 0) {
		fprintf(stderr, "[DRM] Acquired master\n");
		g_have_master = 1;
		return true;
	}
	fprintf(stderr, "[DRM] drmSetMaster failed (%s) – another process may own the display. Modeset might fail.\n", strerror(errno));
	return false;
}

static bool init_drm(struct kms_ctx *d) {
	memset(d, 0, sizeof(*d));
	// Enumerate potential /dev/dri/card* nodes (0-15) to find one with resources + connected connector.
	// On Raspberry Pi 4 with full KMS (dtoverlay=vc4-kms-v3d), the primary render/display node
	// is typically card1 (card0 often firmware emulation or simpledrm early driver). We scan
	// all to stay generic across distro kernels.
	char path[32];
	for (int idx=0; idx<16; ++idx) {
		snprintf(path, sizeof(path), "/dev/dri/card%d", idx);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			continue; // skip silently; permission or non-existent
		}
		drmModeRes *res = drmModeGetResources(fd);
		if (!res) {
			fprintf(stderr, "[DRM] card%d: drmModeGetResources failed: %s\n", idx, strerror(errno));
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
			if (chosen->modes[mi].type & DRM_MODE_TYPE_PREFERRED) { d->mode = chosen->modes[mi]; break; }
		}
		fprintf(stderr, "[DRM] Selected card path %s\n", path);
		ensure_drm_master(fd);
		break;
	}
	if (d->fd < 0 || !d->connector) {
		fprintf(stderr, "Failed to locate a usable DRM device.\n");
		fprintf(stderr, "Troubleshooting: Ensure vc4 KMS overlay enabled and you have permission (try sudo or be in 'video' group).\n");
		return false;
	}

	// Find encoder for connector
	if (d->connector->encoder_id)
		d->encoder = drmModeGetEncoder(d->fd, d->connector->encoder_id);
	if (!d->encoder) {
		for (int i=0; i<d->connector->count_encoders; ++i) {
			d->encoder = drmModeGetEncoder(d->fd, d->connector->encoders[i]);
			if (d->encoder) break;
		}
	}
	if (!d->encoder) { fprintf(stderr, "No encoder for connector %u\n", d->connector_id); return false; }
	d->crtc_id = d->encoder->crtc_id;
	d->orig_crtc = drmModeGetCrtc(d->fd, d->crtc_id);
	if (!d->orig_crtc) { fprintf(stderr, "Failed get original CRTC (%s)\n", strerror(errno)); return false; }
	fprintf(stderr, "[DRM] Using card with fd=%d connector=%u mode=%s %ux%u@%u\n", d->fd, d->connector_id, d->mode.name, d->mode.hdisplay, d->mode.vdisplay, d->mode.vrefresh);

	return true;
}

static void deinit_drm(struct kms_ctx *d) {
	if (!d) return;
	if (d->orig_crtc) {
		drmModeSetCrtc(d->fd, d->orig_crtc->crtc_id, d->orig_crtc->buffer_id,
					   d->orig_crtc->x, d->orig_crtc->y, &d->connector_id, 1, &d->orig_crtc->mode);
		drmModeFreeCrtc(d->orig_crtc);
	}
	if (d->encoder) drmModeFreeEncoder(d->encoder);
	if (d->connector) drmModeFreeConnector(d->connector);
	if (d->res) drmModeFreeResources(d->res);
	if (d->fd >= 0) close(d->fd);
}

static bool init_gbm_egl(const struct kms_ctx *d, struct egl_ctx *e) {
	memset(e, 0, sizeof(*e));
	e->gbm_dev = gbm_create_device(d->fd);
	if (!e->gbm_dev) { fprintf(stderr, "gbm_create_device failed (%s)\n", strerror(errno)); return false; }
	e->gbm_surf = gbm_surface_create(e->gbm_dev, d->mode.hdisplay, d->mode.vdisplay,
						 GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!e->gbm_surf) { fprintf(stderr, "gbm_surface_create failed (%s)\n", strerror(errno)); return false; }

	e->dpy = eglGetDisplay((EGLNativeDisplayType)e->gbm_dev);
	if (e->dpy == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay failed\n"); return false; }
	if (!eglInitialize(e->dpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed (eglError=0x%04x)\n", eglGetError()); return false; }
	eglBindAPI(EGL_OPENGL_ES_API);

	// We must pick an EGLConfig compatible with the GBM surface format (XRGB8888). We'll iterate.
	EGLint cfg_attrs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 0,
		EGL_NONE
	};
	EGLint num=0;
	if (!eglChooseConfig(e->dpy, cfg_attrs, NULL, 0, &num) || num == 0) {
		fprintf(stderr, "eglChooseConfig(query) failed (eglError=0x%04x)\n", eglGetError());
		return false;
	}
	size_t cfg_count = (size_t)num;
	EGLConfig *cfgs = cfg_count ? calloc(cfg_count, sizeof(EGLConfig)) : NULL;
	if (!cfgs) { fprintf(stderr, "Out of memory allocating config list\n"); return false; }
	if (!eglChooseConfig(e->dpy, cfg_attrs, cfgs, num, &num)) {
		fprintf(stderr, "eglChooseConfig(list) failed (eglError=0x%04x)\n", eglGetError());
		free(cfgs);
		return false;
	}
	EGLConfig chosen = NULL;
	for (int i=0; i<num; ++i) {
		EGLint r,g,b,a;
		eglGetConfigAttrib(e->dpy, cfgs[i], EGL_RED_SIZE, &r);
		eglGetConfigAttrib(e->dpy, cfgs[i], EGL_GREEN_SIZE, &g);
		eglGetConfigAttrib(e->dpy, cfgs[i], EGL_BLUE_SIZE, &b);
		eglGetConfigAttrib(e->dpy, cfgs[i], EGL_ALPHA_SIZE, &a);
		if (r==8 && g==8 && b==8) { // alpha may be 0 or 8; either OK with XRGB
			chosen = cfgs[i];
			if (a==0) break; // perfect match for XRGB
		}
	}
	if (!chosen) chosen = cfgs[0];
	e->config = chosen;
	free(cfgs);
	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	e->ctx = eglCreateContext(e->dpy, e->config, EGL_NO_CONTEXT, ctx_attr);
	if (e->ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed (eglError=0x%04x)\n", eglGetError()); return false; }
	EGLint win_attrs[] = { EGL_NONE };
	e->surf = eglCreateWindowSurface(e->dpy, e->config, (EGLNativeWindowType)e->gbm_surf, win_attrs);
	if (e->surf == EGL_NO_SURFACE) { fprintf(stderr, "eglCreateWindowSurface failed (eglError=0x%04x) -> trying with alpha config fallback\n", eglGetError()); }
	if (e->surf == EGL_NO_SURFACE) {
		// Retry with alpha-enabled config if original lacked alpha.
		// Simple fallback: request alpha size 8.
		EGLint retry_attrs[] = {
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_NONE
		};
		EGLint n2=0;
		if (eglChooseConfig(e->dpy, retry_attrs, &e->config, 1, &n2) && n2==1) {
			e->surf = eglCreateWindowSurface(e->dpy, e->config, (EGLNativeWindowType)e->gbm_surf, win_attrs);
		}
		if (e->surf == EGL_NO_SURFACE) {
			fprintf(stderr, "eglCreateWindowSurface still failed (eglError=0x%04x)\n", eglGetError());
			return false;
		}
	}
	if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) { fprintf(stderr, "eglMakeCurrent failed (eglError=0x%04x)\n", eglGetError()); return false; }
	const char *gl_vendor = (const char*)glGetString(GL_VENDOR);
	const char *gl_renderer = (const char*)glGetString(GL_RENDERER);
	const char *gl_version = (const char*)glGetString(GL_VERSION);
	fprintf(stderr, "[GL] VENDOR='%s' RENDERER='%s' VERSION='%s'\n", gl_vendor?gl_vendor:"?", gl_renderer?gl_renderer:"?", gl_version?gl_version:"?");
	return true;
}

static void deinit_gbm_egl(struct egl_ctx *e) {
	if (!e) return;
	if (e->dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (e->ctx != EGL_NO_CONTEXT) eglDestroyContext(e->dpy, e->ctx);
		if (e->surf != EGL_NO_SURFACE) eglDestroySurface(e->dpy, e->surf);
		eglTerminate(e->dpy);
	}
	if (e->gbm_surf) gbm_surface_destroy(e->gbm_surf);
	if (e->gbm_dev) gbm_device_destroy(e->gbm_dev);
}

// mpv rendering integration
struct mpv_player {
	mpv_handle *mpv;
	mpv_render_context *rctx;
	int using_libmpv; // flag indicating fallback to vo=libmpv occurred
};

// Wakeup callback sets a flag so main loop knows mpv wants processing.
static volatile int g_mpv_wakeup = 0;
static void mpv_wakeup_cb(void *ctx) { (void)ctx; g_mpv_wakeup = 1; }
static volatile uint64_t g_mpv_update_flags = 0; // bitmask from mpv_render_context_update
static void on_mpv_events(void *data) { (void)data; g_mpv_wakeup = 1; }

// --- Statistics ---
static int g_stats_enabled = 0;
static double g_stats_interval_sec = 2.0; // default
static uint64_t g_stats_frames = 0;
static struct timeval g_stats_start = {0};
static struct timeval g_stats_last = {0};
static uint64_t g_stats_last_frames = 0;

static double tv_diff(const struct timeval *a, const struct timeval *b) {
	return (double)(a->tv_sec - b->tv_sec) + (double)(a->tv_usec - b->tv_usec)/1e6;
}

static void stats_log_periodic(struct mpv_player *p) {
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

static void stats_log_final(struct mpv_player *p) {
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
}

// Helper for mpv option failure logging
static void log_opt_result(const char *opt, int code) {
	if (code < 0) fprintf(stderr, "[mpv] option %s failed (%d)\n", opt, code);
}

static bool init_mpv(struct mpv_player *p, const char *file) {
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

static void destroy_mpv(struct mpv_player *p) {
	if (!p) return;
	if (p->rctx) mpv_render_context_free(p->rctx);
	if (p->mpv) {
		mpv_terminate_destroy(p->mpv);
	}
}

static void drain_mpv_events(mpv_handle *h) {
	while (1) {
		mpv_event *ev = mpv_wait_event(h, 0);
		if (ev->event_id == MPV_EVENT_NONE) break;
		if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
			mpv_event_log_message *lm = ev->data;
			// Only print warnings/errors by default; set PICKLE_LOG_MPV for full detail earlier.
			if (lm->level && (strstr(lm->level, "error") || strstr(lm->level, "warn"))) {
				fprintf(stderr, "[mpv-log] %s: %s", lm->level, lm->text ? lm->text : "\n");
			}
			continue;
		}
		if (ev->event_id == MPV_EVENT_END_FILE) {
			const mpv_event_end_file *ef = ev->data;
			fprintf(stderr, "End of file (reason=%d:%s)\n", ef->reason, mpv_end_reason_str(ef->reason));
			if (ef->error < 0) {
				const char *err = mpv_error_string(ef->error);
				fprintf(stderr, "[mpv] end-file error detail: %s (%d)\n", err, ef->error);
			}
			g_stop = 1;
		}
	}
}

// Note: Removed earlier experimental render_frame() that used a non-standard C++ lambda.
// The fixed implementation below (render_frame_fixed) is the one actually used.

// Because we attempted a C++-style lambda above (invalid in C), we need a proper handler
// but we also must know the gbm_bo to release. We'll store a static reference to egl_ctx
// and pass the bo as user data.
static struct egl_ctx *g_egl_for_handler;
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data) {
	(void)fd; (void)frame; (void)sec; (void)usec;
	struct gbm_bo *old = data;
	if (g_egl_for_handler && old) gbm_surface_release_buffer(g_egl_for_handler->gbm_surf, old);
}

// Removed const from drm_ctx parameter because drmModeSetCrtc expects a non-const drmModeModeInfoPtr.
// We cache framebuffer IDs per gbm_bo to avoid per-frame AddFB/RmFB churn.
// user data holds a small struct with fb id + drm fd and a destroy handler.
static struct gbm_bo *g_first_frame_bo = NULL; // BO used for initial modeset, released after second frame
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

static bool render_frame_fixed(struct kms_ctx *d, struct egl_ctx *e, struct mpv_player *p) {
	if (!eglMakeCurrent(e->dpy, e->surf, e->surf, e->ctx)) {
		fprintf(stderr, "eglMakeCurrent failed\n"); return false; }
	mpv_opengl_fbo fbo = { .fbo = 0, .w = d->mode.hdisplay, .h = d->mode.vdisplay, .internal_format = 0 };
	int flip_y = 0;
	mpv_render_param r_params[] = {
		(mpv_render_param){MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
		(mpv_render_param){MPV_RENDER_PARAM_FLIP_Y, &flip_y},
		(mpv_render_param){0}
	};
	if (!p->rctx) {
		fprintf(stderr, "mpv render context NULL\n");
		return false;
	}
	mpv_render_context_render(p->rctx, r_params);
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
		if (drmModePageFlip(d->fd, d->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, bo)) {
			fprintf(stderr, "drmModePageFlip failed (%s)\n", strerror(errno));
			gbm_surface_release_buffer(e->gbm_surf, bo);
			return false;
		}
		fd_set fds; FD_ZERO(&fds); FD_SET(d->fd, &fds);
		if (select(d->fd+1, &fds, NULL, NULL, NULL) < 0) {
			fprintf(stderr, "select failed\n");
			return false;
		}
		drmEventContext ev = { .version = DRM_EVENT_CONTEXT_VERSION, .page_flip_handler = page_flip_handler };
		drmHandleEvent(d->fd, &ev);
		if (g_first_frame_bo && g_first_frame_bo != bo) {
			gbm_surface_release_buffer(e->gbm_surf, g_first_frame_bo);
			g_first_frame_bo = NULL;
		}
	} else {
		// Offscreen mode: just release BO immediately (no scanout usage).
		gbm_surface_release_buffer(e->gbm_surf, bo);
	}
	// No need to remove FB each frame; retained until BO destroyed.
	return true;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "Usage: %s <video-file>\n", argv[0]); return 1; }
	signal(SIGINT, handle_sigint);
    signal(SIGSEGV, handle_sigsegv);
	const char *file = argv[1];

	struct kms_ctx drm = {0};
	struct egl_ctx eglc = {0};
	struct mpv_player player = {0};

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
	if (!init_mpv(&player, file)) RET("init_mpv");

	fprintf(stderr, "Playing %s at %dx%d %.2f Hz\n", file, drm.mode.hdisplay, drm.mode.vdisplay,
			(drm.mode.vrefresh ? (double)drm.mode.vrefresh : (double)drm.mode.clock / (drm.mode.htotal * drm.mode.vtotal))); 

	// Event-driven loop: render only when mpv signals a frame update unless override forces continuous loop.
	// Event-driven loop: use mpv_render_context_update flags to decide when to render
	const int idle_sleep_us = 16000; // used only in force loop mode
	int frames = 0;
	int force_loop = getenv("PICKLE_FORCE_RENDER_LOOP") ? 1 : 0;
	while (!g_stop) {
		if (g_mpv_wakeup) {
			g_mpv_wakeup = 0;
			drain_mpv_events(player.mpv);
			if (player.rctx) {
				uint64_t flags = mpv_render_context_update(player.rctx);
				g_mpv_update_flags |= flags; // accumulate until consumed
			}
		}
		if (g_stop) break;
		int need_frame = 0;
		if (force_loop) need_frame = 1;
		else if (g_mpv_update_flags & MPV_RENDER_UPDATE_FRAME) need_frame = 1;
		if (need_frame) {
			if (!render_frame_fixed(&drm, &eglc, &player)) { fprintf(stderr, "Render failed, exiting\n"); break; }
			frames++;
			g_mpv_update_flags &= ~MPV_RENDER_UPDATE_FRAME; // consume frame flag
			if (g_stats_enabled) { g_stats_frames++; stats_log_periodic(&player); }
		} else {
			usleep(2000);
		}
		if (force_loop && !g_mpv_wakeup) usleep(idle_sleep_us);
	}

	stats_log_final(&player);
	destroy_mpv(&player);
	deinit_gbm_egl(&eglc);
	deinit_drm(&drm);
	return 0;
fail:
	destroy_mpv(&player);
	deinit_gbm_egl(&eglc);
	deinit_drm(&drm);
	return 1;
}

