# Pickle

Minimal fullscreen Raspberry Pi 4 DRM/KMS + GBM + EGL + libmpv hardware accelerated video player.

## Features
* Direct to KMS (no X / Wayland) using DRM master.
* Uses GBM + EGL (GLES2) for libmpv OpenGL render backend.
* Auto-selects first connected monitor and preferred mode.
* Plays one file then exits (Ctrl+C to stop early).

## Dependencies (Raspberry Pi OS / Debian)
Install development headers:
```
sudo apt update
sudo apt install -y libmpv-dev libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev build-essential pkg-config
```

Make sure `dtoverlay=vc4-kms-v3d` (or `dtoverlay=vc4-kms-v3d-pi4`) is enabled in `/boot/firmware/config.txt` then reboot.

## Build
```
gcc -O2 -Wall -std=c11 pickle.c -o pickle \
	-lmpv -ldrm -lgbm -lEGL -lGLESv2 -lpthread -lm
```

If `pkg-config` is preferred:
```
gcc -O2 -Wall -std=c11 pickle.c -o pickle $(pkg-config --cflags --libs mpv gbm egl glesv2) -ldrm
```

## Run
You need DRM master access (non-root possible if logind/seatd grants it and your user is in correct groups).

Quick start (Makefile):
```
make
make preflight          # optional environment checks
make try-run VIDEO=vid.mp4   # attempt non-root run on a free TTY
make run VIDEO=vid.mp4       # uses sudo for root run
```

Manual run (root):
```
sudo ./pickle /path/to/video.mp4
```

To avoid sudo:
1. Add your user to groups: `sudo usermod -aG video,render $USER` then re-login.
2. Switch to a text console (Ctrl+Alt+F2) so no compositor holds DRM master.
3. Ensure `XDG_RUNTIME_DIR` exists (typically `/run/user/UID`) for audio.
4. Use `make try-run VIDEO=...`.

### Using the Makefile
```
make            # build
make run VIDEO=/path/to/video.mp4   # build then run (uses sudo)
make clean      # remove objects and binary
sudo make install PREFIX=/usr/local # optional install
```

## Notes
* Simplified: no audio device selection, hotplug handling, or vsync pacing beyond page flip.
* Uses `hwdec=auto-safe`; adjust via code if you need a specific decoder.
* Tested conceptually; minor adjustments may be needed depending on your distribution's driver stack.

## Performance
Several optimizations are implemented to reduce CPU overhead and syscalls:

1. Framebuffer caching: Each GBM BO acquires a DRM framebuffer ID once and reuses it (no per-frame `drmModeAddFB`/`drmModeRmFB` churn).
2. Event-driven rendering: The main loop only renders when mpv indicates a new frame/update rather than spinning continuously. This slashes idle CPU usage on still frames.
3. Optional continuous loop: Set `PICKLE_FORCE_RENDER_LOOP=1` to restore a tight loop if you suspect missed subtitle/OSD updates in your mpv build.
4. High-performance build mode: `make PERF=1` adds aggressive flags (`-O3 -march=native -ffast-math -fomit-frame-pointer -DNDEBUG`). Combine with `LTO=1` for link-time optimization.
5. Linker speed-ups: PERF build auto-selects `mold` or `lld` if installed for faster incremental builds.

Suggested usage for maximum performance:
```
make clean
make PERF=1 LTO=1
sudo ./pickle video.mp4
```

If you need to measure CPU usage differences, compare with and without `PERF=1` using `pidstat -p <pid> 1` or `perf top`.

Environment variables summary (performance-related):
* `PICKLE_FORCE_RENDER_LOOP=1`  Force legacy continuous rendering loop.
* `PICKLE_LOG_MPV=1`           Verbose mpv logs (costs some performance when very chatty).
* `PICKLE_STATS=1`             Enable periodic and final playback stats.
* `PICKLE_STATS_INTERVAL=1.0`  Stats logging interval in seconds (default 2.0; min 0.05 accepted).

## Environment Variables (Production)
The player supports several environment variables for production deployment:

**Video Output:**
* `PICKLE_VO=<value>`         mpv video output (default: libmpv, alternatives: gpu)
* `PICKLE_GPU_CONTEXT=<ctx>`  Override GPU context when using vo=gpu (x11egl, wayland, headless, etc.)
* `PICKLE_FORCE_HEADLESS=1`   Force gpu-context=headless regardless of DRM master status
* `PICKLE_DISABLE_HEADLESS=1` Disable automatic headless fallback
* `PICKLE_KEEP_ATOMIC=1`      Don't disable DRM atomic operations (may cause conflicts)

**Audio Control:**
* `PICKLE_NO_AUDIO=1`         Disable audio completely
* `PICKLE_FORCE_AUDIO=1`      Enable audio even under root without XDG_RUNTIME_DIR

**Hardware Decode:**
* `PICKLE_HWDEC=<value>`      Hardware decoder (default: auto-safe, alternatives: auto, vaapi, v4l2m2m, etc.)

**Advanced:**
* `PICKLE_GL_ADV=1`           Enable mpv advanced control (experimental)
* `PICKLE_FORCE_RENDER_LOOP=1` Force continuous rendering instead of event-driven
* `PICKLE_LOG_MPV=1`          Enable verbose mpv logging

**Deprecated (will show warnings):**
* `PICKLE_FORCE_LIBMPV=1`     Use PICKLE_VO=libmpv instead
* `PICKLE_NO_CUSTOM_CTX=1`    gpu-context=custom disabled by default now

**Production Recommendations:**
1. **Default setup** works out of the box (vo=libmpv avoids GPU context conflicts)
2. **For GPU acceleration:** `PICKLE_VO=gpu PICKLE_GPU_CONTEXT=headless`
3. **Headless deployment:** `PICKLE_NO_AUDIO=1 PICKLE_VO=gpu PICKLE_FORCE_HEADLESS=1`
4. **Debug failing playback:** `PICKLE_LOG_MPV=1 PICKLE_STATS=1`

Future potential enhancements (not yet implemented):
* Use `drmModeAtomicCommit` with retained planes for overlay/subtitle blending.
* Switch to `drmModeAddFB2` with modifiers for direct scanout of more formats.
* Explicit FPS pacing (clock vs. display refresh) to reduce latency jitter.
* Batch event polling (epoll on DRM fd + signalfd) instead of select per frame.

Contributions or further tuning ideas welcome.

## Troubleshooting
If you see `drmModeGetResources failed` or `Failed to locate a usable DRM device`:
1. Ensure KMS overlay is enabled in `/boot/firmware/config.txt` (RPi OS): `dtoverlay=vc4-kms-v3d` (or `vc4-kms-v3d-pi4`). Reboot after changing.
2. Run with sudo, or make sure you're in the `video` group: `sudo usermod -aG video $USER` then log out/in.
3. Check that `/dev/dri/card0` exists: `ls -l /dev/dri/`.
4. Confirm a connected display: `modetest -M vc4` (from `libdrm-tests` package) and look for a connected connector with modes.
5. If using Wayland/X simultaneously, ensure no compositor has locked KMS master (try a pure console tty: Ctrl+Alt+F2).
6. For debug, set env `LIBMVP_VERBOSE=1` (if mpv built with logging) or temporarily add `fprintf` lines.
7. Use `make preflight` to run automated checks (groups, devices, overlay, pkg-config, runtime dir).

### Common mpv end reasons
`eof` (0) playback finished normally.
`quit` (2) user / API stop.
`error` (3) decode or init failure (often audio init if XDG_RUNTIME_DIR missing under sudo).
`redirect` (4) URL redirected (network streams) — rare for local files; if seen with local input, re-check mapping/logging version.

If you see `error` early:
* Try `PICKLE_NO_AUDIO=1 make try-run VIDEO=...` to test whether audio init is failing.
* Run with `PICKLE_LOG_MPV=1` for verbose mpv internal logs.
* Enable stats for runtime insight: `PICKLE_STATS=1 PICKLE_STATS_INTERVAL=0.5 ./pickle video.mp4`


## License
Provided as-is; integrate into your own project under your chosen license (original author of this scaffold: AI assistant output).

