# Modular Makefile for pickle DRM+EGL+libmpv player
#
# Pickle is a minimal fullscreen Raspberry Pi 4 video player using
# DRM/KMS + GBM + EGL + libmpv for hardware accelerated playback.
#
# Main build targets:
#   make        - Build the default pickle binary
#   make rpi4   - Build with Raspberry Pi 4 optimizations
#   make run VIDEO=video.mp4 - Build and run with specified video
#   make help   - Show all build options
#
# For more information, run 'make help' or see the documentation in docs/
#

# Modularization Todo:
# ✅ Create keystone module (keystone.c/keystone.h)
# ✅ Create utils module (utils.c/utils.h)
# ✅ Create shader module (shader.c/shader.h)
# ✅ Clean up redundant adapter files and scripts
# ✅ Create DRM module (drm.c/drm.h)
# ✅ Create EGL module (egl.c/egl.h)
# ✅ Create input module (input.c/input.h)

APP      := pickle

# Source files - add new modules here
COMMON_SOURCES := pickle.c utils.c shader.c keystone.c keystone_funcs.c keystone_get_config.c drm.c drm_atomic.c drm_keystone.c egl.c egl_dmabuf.c render_video.c zero_copy.c input.c error.c frame_pacing.c render.c mpv.c dispmanx.c v4l2_decoder.c hvs_keystone.c compute_keystone.c event.c event_callbacks.c pickle_events.c pickle_globals.c mpv_render.c render_backend.c

# Conditional source files based on features
ifeq ($(VULKAN),1)
SOURCES := $(COMMON_SOURCES) vulkan.c vulkan_utils.c vulkan_compute.c
else
SOURCES := $(COMMON_SOURCES)
endif

OBJECTS  := $(SOURCES:.c=.o)

# Common header files
COMMON_HEADERS := utils.h shader.h keystone.h drm.h drm_keystone.h egl.h input.h error.h frame_pacing.h render.h mpv.h dispmanx.h v4l2_decoder.h v4l2_player.h hvs_keystone.h compute_keystone.h event.h event_callbacks.h pickle_events.h pickle_globals.h render_backend.h

# Conditional header files
ifeq ($(VULKAN),1)
HEADERS := $(COMMON_HEADERS) vulkan.h
else
HEADERS := $(COMMON_HEADERS)
endif

# Toolchain / standards
CROSS   ?=
CC      ?= $(CROSS)gcc
CSTD    ?= c11

# Warnings & optimization defaults
WARN    ?= -Wall -Wextra -Wno-unused-parameter -Wshadow -Wformat=2 -Wconversion -Wpointer-arith
OPT     ?= -O2
DEBUG   ?= -g

# Feature toggles (set to 1 to enable)
LTO     ?= 0
ZEROCOPY ?= 1  # Enable zero-copy by default
NO_MPV  ?= 0   # build with -DPICKLE_NO_MPV_DEFAULT so runtime can skip mpv init
PERF    ?= 0   # high-performance build tweaks (e.g. make PERF=1)
EVENT   ?= 1   # Enable event-driven architecture
DISPMANX ?= 1  # Enable DispmanX support for RPi (e.g. make DISPMANX=1)
VULKAN  ?= 0   # Enable Vulkan support (e.g. make VULKAN=1)

PKGS       := mpv gbm egl glesv2 libdrm libv4l2

# Add bcm_host package for Raspberry Pi with DispmanX
ifeq ($(DISPMANX),1)
	PKGS += bcm_host
endif

# Add Vulkan package if Vulkan is enabled
ifeq ($(VULKAN),1)
PKGS += vulkan
# Add explicit include path for libdrm to avoid conflicts with local drm.h
CFLAGS += -I/usr/include/libdrm
endif

# Allow overriding pkg-config binary
PKG_CONFIG ?= pkg-config
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS) 2>/dev/null)

# Basic flags
CFLAGS  ?= $(OPT) $(DEBUG) $(WARN) -std=$(CSTD)
LDFLAGS ?=

# Define feature flags
ifeq ($(VULKAN),1)
FEATURE_FLAGS += -DVULKAN_ENABLED=1
$(info Building with Vulkan support enabled - VULKAN_ENABLED=1 will be defined)
endif

ifeq ($(ZEROCOPY),1)
FEATURE_FLAGS += -DZEROCOPY_ENABLED=1
endif

ifeq ($(EVENT),1)
FEATURE_FLAGS += -DEVENT_DRIVEN_ENABLED=1
endif

ifeq ($(DISPMANX),1)
FEATURE_FLAGS += -DDISPMANX_ENABLED=1
endif

# Combine all flags
CFLAGS += $(PKG_CFLAGS) -DPICKLE_KEYSTONE_MODULAR $(FEATURE_FLAGS)

# If pkg-config fails, fall back to a reasonable default library set
ifeq ($(strip $(PKG_LIBS)),)
LIBS     := -lmpv -lgbm -lEGL -lGLESv2 -ldrm -lv4l2 -lpthread -lm
# Add Vulkan library if Vulkan is enabled
ifeq ($(VULKAN),1)
LIBS     += -lvulkan
endif
else
LIBS     := $(PKG_LIBS) -lpthread -lm
# Ensure Vulkan lib is included when pkg-config is available
ifeq ($(VULKAN),1)
ifeq ($(shell $(PKG_CONFIG) --exists vulkan || echo notfound),notfound)
LIBS     += -lvulkan
endif
endif
endif

# Apply toggles
ifeq ($(LTO),1)
	CFLAGS  += -flto
	LDFLAGS += -flto
endif
ifeq ($(NO_MPV),1)
	CFLAGS  += -DPICKLE_NO_MPV_DEFAULT=1
endif
ifeq ($(PERF),1)
	CFLAGS += -O3 -DNDEBUG -fomit-frame-pointer -march=native -mtune=native
	CFLAGS += -ffast-math -fno-math-errno -fno-trapping-math
	# Enable LTO automatically if not already set
	ifeq ($(strip $(LTO)),0)
		CFLAGS  += -flto
		LDFLAGS += -flto
	endif
	# Prefer mold or lld if available
	ifneq (,$(shell command -v mold 2>/dev/null))
		LDFLAGS += -fuse-ld=mold
	else ifneq (,$(shell command -v ld.lld 2>/dev/null))
		LDFLAGS += -fuse-ld=lld
	endif
endif

# Check for zero-copy enable
ifeq ($(ZEROCOPY),1)
	CFLAGS += -DZEROCOPY_ENABLED=1
endif

# Check for event-driven architecture enable
ifeq ($(EVENT),1)
	CFLAGS += -DEVENT_DRIVEN_ENABLED=1
endif

# Check for DispmanX support
ifeq ($(DISPMANX),1)
	CFLAGS += -DDISPMANX_ENABLED=1
endif

# RPi4-specific optimizations
RPI4_OPT ?= 0
ifeq ($(RPI4_OPT),1)
	# Cortex-A72 specific flags for RPi4
	CFLAGS += -mcpu=cortex-a72 -mfpu=neon-fp-armv8 -mfloat-abi=hard
	CFLAGS += -ftree-vectorize -funroll-loops -fprefetch-loop-arrays
	CFLAGS += -DRPI4_OPTIMIZED=1 -DDISPMANX_ENABLED=1 -DUSE_V4L2_DECODER=1
	LIBS += -lbcm_host
endif

# Maximum performance mode (combines all optimizations)
MAXPERF ?= 0
ifeq ($(MAXPERF),1)
	CFLAGS := -O3 -DNDEBUG -fomit-frame-pointer -march=native -mtune=native
	CFLAGS += -ffast-math -fno-math-errno -fno-trapping-math
	CFLAGS += -funroll-loops -fprefetch-loop-arrays -ftree-vectorize
	CFLAGS += -flto -fuse-linker-plugin
	CFLAGS += $(WARN) -std=$(CSTD) $(PKG_CFLAGS)
	LDFLAGS += -flto -Wl,-O3 -Wl,--as-needed
	ifneq (,$(shell command -v mold 2>/dev/null))
		LDFLAGS += -fuse-ld=mold
	else ifneq (,$(shell command -v ld.lld 2>/dev/null))
		LDFLAGS += -fuse-ld=lld
	endif
endif

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: $(APP)

$(APP): $(OBJECTS)
	@ if [ -z "$(PKG_LIBS)" ]; then echo "[warn] pkg-config not found or missing pc files; using fallback libs."; fi
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

# Compile each source file
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Dependency tracking
deps.mk: $(SOURCES)
	$(CC) -MM $(CFLAGS) $^ > $@

-include deps.mk

run: $(APP)
	@if [ -z "$(VIDEO)" ]; then \
		echo "Usage: make run VIDEO=/path/to/video [MPV_ARGS=...]"; exit 1; \
	fi
	@echo "Running $(APP) on $(VIDEO)";
	sudo ./$(APP) "$(VIDEO)" $(MPV_ARGS)

try-run: $(APP)
	@if [ -z "$(VIDEO)" ]; then \
		echo "Usage: make try-run VIDEO=/path/to/video"; exit 1; \
	fi
	@echo "Attempting non-root run (may fail if DRM master needed)";
	./$(APP) "$(VIDEO)" $(MPV_ARGS) || { echo "Hint: use 'sudo make run VIDEO=...' if permission denied."; exit 1; }

# Build mode convenience targets
release: CFLAGS := -O3 -DNDEBUG -flto $(WARN) -std=$(CSTD) $(PKG_CFLAGS)
release: LDFLAGS += -flto
release: clean $(APP)
	strip $(APP)
	@echo "Built and stripped release binary: $(APP)"
	@ls -lh $(APP)

debug: CFLAGS := -O0 -g3 $(WARN) -std=$(CSTD) $(PKG_CFLAGS)
debug: clean $(APP)
	@echo "Built debug binary: $(APP)"

sanitize: CFLAGS := -O1 -g -fsanitize=address,undefined $(WARN) -std=$(CSTD) $(PKG_CFLAGS)
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean $(APP)
	@echo "Built sanitize (ASAN+UBSAN) binary: $(APP)"

# Raspberry Pi 4 optimized build
rpi4: RPI4_OPT=1
rpi4: clean $(APP)

# Maximum performance build
maxperf: MAXPERF=1
maxperf: clean $(APP)

# RPi4 optimized with maximum performance
rpi4-maxperf: RPI4_OPT=1
rpi4-maxperf: MAXPERF=1
rpi4-maxperf: clean $(APP)
	@echo "Built RPi4 optimized binary with max performance: $(APP)"

# Release build for RPi4 with maximum performance and stripped binary
rpi4-release: RPI4_OPT=1
rpi4-release: MAXPERF=1
rpi4-release: clean $(APP)
	strip -s $(APP)
	@echo "Built and stripped RPi4 optimized binary with max performance: $(APP)"
	@ls -lh $(APP)

strip: $(APP)
	strip -s $(APP)
	@echo "Stripped binary: $(APP)"

install: $(APP)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(APP)

deps:
	@echo "Package Dependencies:"; \
	echo ""; \
	echo "Debian/Raspberry Pi OS:"; \
	echo "  sudo apt install libmpv-dev libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev libv4l-dev pkg-config build-essential"; \
	echo ""; \
	echo "Fedora:"; \
	echo "  sudo dnf install mpv-libs-devel libdrm-devel mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel libv4l-devel pkg-config gcc"; \
	echo ""; \
	echo "Arch Linux:"; \
	echo "  sudo pacman -S mpv libdrm libgbm libglvnd libv4l pkg-config base-devel"; \
	echo ""; \
	echo "Optional Dependencies:"; \
	echo "  • seatd        - For rootless DRM access"; \
	echo "  • clang        - Alternative compiler"; \
	echo "  • mold/lld     - Faster linkers"; \
	echo "  • libdrm-tests - For diagnostic tools"; \
	echo ""; \
	echo "Raspberry Pi specific:"; \
	echo "  For best performance, ensure dtoverlay=vc4-kms-v3d is in /boot/config.txt"; \
	echo "  For atomic modesetting (best zero-copy performance), add dtparam=atomic=on"

help:
	@echo "Pickle Video Player - Build Options"; \
	echo ""; \
	echo "Basic Targets:"; \
	echo "  all / (default)      Build $(APP)"; \
	echo "  run VIDEO=...        Run with sudo (DRM master)"; \
	echo "  try-run VIDEO=...    Attempt run without sudo"; \
	echo "  clean                Remove build artifacts"; \
	echo ""; \
	echo "Build Configuration:"; \
	echo "  release              Optimized build with -O3"; \
	echo "  debug                Debug build with symbols"; \
	echo "  sanitize             ASAN+UBSAN instrumented build"; \
	echo "  rpi4                 Optimized for Raspberry Pi 4"; \
	echo "  maxperf              Maximum performance build"; \
	echo "  rpi4-maxperf         Combined RPi4 + maximum performance"; \
	echo "  rpi4-release         RPi4 + maxperf + stripped (best for deployment)"; \
	echo "  strip                Strip binary for smaller size"; \
	echo ""; \
	echo "Feature Flags (1=enable, 0=disable):"; \
	echo "  ZEROCOPY=1           Enable zero-copy video path (default: $(ZEROCOPY))"; \
	echo "  EVENT=1              Enable event-driven architecture (default: $(EVENT))"; \
	echo "  LTO=1                Enable link-time optimization (default: $(LTO))"; \
	echo "  NO_MPV=1             Build with MPV disabled by default (default: $(NO_MPV))"; \
	echo "  PERF=1               High-performance build mode (default: $(PERF))"; \
	echo "  RPI4_OPT=1           Raspberry Pi 4 optimizations (default: $(RPI4_OPT))"; \
	echo "  MAXPERF=1            Maximum performance mode (default: $(MAXPERF))"; \
	echo ""; \
	echo "Installation:"; \
	echo "  install              Install to $(PREFIX)"; \
	echo "  uninstall            Uninstall from $(PREFIX)"; \
	echo ""; \
	echo "Utilities:"; \
	echo "  preflight            Environment readiness checks"; \
	echo "  deps                 Show package dependencies"; \
	echo "  run-help             Show runtime command-line options"; \
	echo ""; \
	echo "Environment Variables:"; \
	echo "  CROSS                Cross-compiler prefix"; \
	echo "  CC                   Compiler to use (default: gcc)"; \
	echo "  PKG_CONFIG           pkg-config binary to use"; \
	echo "  MPV_ARGS             Arguments to pass to MPV"; \
	echo "  PREFIX               Installation prefix (default: /usr/local)"; \
	echo ""; \
	echo "Examples:"; \
	echo "  make release                    # Build optimized version"; \
	echo "  make ZEROCOPY=0 EVENT=0         # Build without zero-copy or event system"; \
	echo "  make rpi4-release               # Build for RPi4 with all optimizations"; \
	echo "  make rpi4 && sudo make install  # Build for RPi4 and install"; \
	echo "  make run VIDEO=my_video.mp4     # Build and run with a video file"

# Show runtime command-line options
run-help: $(APP)
	@echo "Pickle Video Player - Runtime Options"; \
	echo ""; \
	echo "Basic Usage:"; \
	echo "  sudo ./pickle [options] video_file"; \
	echo ""; \
	echo "Command-line Options:"; \
	echo "  --help                Show this help message"; \
	echo "  --version             Show version information"; \
	echo "  --no-mpv              Disable MPV and use V4L2 decoder directly"; \
	echo "  --no-vsync            Disable VSync"; \
	echo "  --no-keystone         Disable keystone correction"; \
	echo "  --no-scanout          Disable direct scanout mode"; \
	echo "  --no-zero-copy        Disable zero-copy path"; \
	echo "  --no-event-driven     Disable event-driven architecture"; \
	echo "  --drm-device=PATH     Specify DRM device path (default: autodetect)"; \
	echo "  --keystone=FILE       Load keystone settings from file"; \
	echo "  --loop                Loop video playback"; \
	echo "  --stats               Show performance statistics"; \
	echo "  --debug               Enable debug output"; \
	echo ""; \
	echo "Keyboard Controls:"; \
	echo "  q                     Quit"; \
	echo "  h                     Show/hide help overlay"; \
	echo "  k                     Enable keystone mode"; \
	echo "  1-4                   Select keystone corner"; \
	echo "  Arrow keys             Adjust selected corner"; \
	echo "  r                     Reset keystone"; \
	echo ""; \
	echo "Examples:"; \
	echo "  sudo ./pickle video.mp4                # Play a video"; \
	echo "  sudo ./pickle --no-mpv video.mp4       # Use V4L2 decoder"; \
	echo "  sudo ./pickle --loop --stats video.mp4 # Loop with stats"

preflight:
	@bash tools/preflight.sh

clean:
	rm -f $(OBJECTS) $(APP)

# Clean all build artifacts including object files, binaries, and backups
distclean: clean
	rm -f *.o *.a *.so *.bak *~ *.orig

.PHONY: all run try-run preflight release debug sanitize rpi4 maxperf rpi4-maxperf rpi4-release strip deps help run-help clean distclean install uninstall
