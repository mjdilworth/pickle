# Simple Makefile for pickle DRM+EGL+libmpv player
# Targets:
#   make        (default) build binary
#   make run VIDEO=path/to/file.mp4   run video (uses sudo by default)
#   make clean  remove build artifacts
# Variables can be overridden: CC, CFLAGS, LDFLAGS, PREFIX

APP      := pickle
SRC      := pickle.c
OBJ      := $(SRC:.c=.o)

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
NO_MPV  ?= 0   # build with -DPICKLE_NO_MPV_DEFAULT so runtime can skip mpv init
PERF    ?= 0   # high-performance build tweaks (e.g. make PERF=1)

PKGS       := mpv gbm egl glesv2 libdrm
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs $(PKGS) 2>/dev/null)

# Basic flags
CFLAGS  ?= $(OPT) $(DEBUG) $(WARN) -std=$(CSTD) $(PKG_CFLAGS)
LDFLAGS ?=
LIBS     := $(PKG_LIBS) -lpthread -lm

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
	ifneq ($(LTO),0)
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

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: $(APP)

$(APP): $(OBJ)
	@ if [ -z "$(PKG_LIBS)" ]; then echo "[warn] pkg-config returned no libs for $(PKGS)."; fi
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

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
release: CFLAGS := -O3 -DNDEBUG $(WARN) -std=$(CSTD) $(PKG_CFLAGS)
release: clean $(APP)
	@echo "Built release binary: $(APP)"

debug: CFLAGS := -O0 -g3 $(WARN) -std=$(CSTD) $(PKG_CFLAGS)
debug: clean $(APP)
	@echo "Built debug binary: $(APP)"

sanitize: CFLAGS := -O1 -g -fsanitize=address,undefined $(WARN) -std=$(CSTD) $(PKG_CFLAGS)
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean $(APP)
	@echo "Built sanitize (ASAN+UBSAN) binary: $(APP)"

strip: $(APP)
	strip $(APP)
	@echo "Stripped binary: $(APP)"

install: $(APP)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(APP)

deps:
	@echo "Debian/RPi OS packages:"; \
	echo "  sudo apt install libmpv-dev libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev pkg-config build-essential"; \
	echo "Optional: seatd for rootless DRM, clang, mold (linker)"

help:
	@echo "Targets:"; \
	echo "  all / (default)      Build $(APP)"; \
	echo "  run VIDEO=...        Run with sudo (DRM master)"; \
	echo "  try-run VIDEO=...    Attempt run without sudo"; \
	echo "  preflight            Environment readiness checks"; \
	echo "  release|debug        Optimized / debug builds"; \
	echo "  PERF=1              High-performance build mode"; \
	echo "  sanitize             ASAN+UBSAN build"; \
	echo "  strip                Strip binary"; \
	echo "  install/uninstall    Install to $(PREFIX)"; \
	echo "  deps                 Show package dependencies"; \
	echo "Variables:"; \
	echo "  CROSS, LTO=1, NO_MPV=1, MPV_ARGS=..."; \
	echo "Example:"; \
	echo "  make release && sudo make install"

preflight:
	@bash tools/preflight.sh

clean:
	rm -f $(OBJ) $(APP)

.PHONY: all run clean install uninstall
