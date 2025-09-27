# Pickle

Minimal fullscreen Raspberry Pi 4 DRM/KMS + GBM + EGL + libmpv hardware accelerated video player.

## Features
* Direct to KMS (no X / Wayland) using DRM master.
* Uses GBM + EGL (GLES2) for libmpv OpenGL render backend.
* Zero-copy buffer path using DMA-BUF for optimal performance.
* Atomic modesetting for tear-free video updates.
* Auto-selects first connected monitor and preferred mode.
* Keystone correction for projector use.
* Hardware-accelerated keystone using RPi4 HVS (Hardware Video Scaler).
* Compute shader-based keystone for efficient GPU acceleration.
* V4L2 direct decoder path for bypassing MPV for better performance.
* Plays one file then exits (Ctrl+C to stop early).
* Optional continuous playback looping.
* Optimized performance for Raspberry Pi 4 with CPU and memory reduction strategies.

## Dependencies (Raspberry Pi OS / Debian)
Install development headers:
```
sudo apt update
sudo apt install -y libmpv-dev libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev build-essential pkg-config
```

Make sure `dtoverlay=vc4-kms-v3d` (or `dtoverlay=vc4-kms-v3d-pi4`) is enabled in `/boot/firmware/config.txt` then reboot.

## Build
```
# Standard build
make

# Release build (smaller, optimized)
make RELEASE=1

# Maximum performance build with Raspberry Pi 4 optimizations
make RELEASE=1 MAXPERF=1 RPI4_OPT=1
```

Or manually:
```
gcc -O2 -Wall -std=c11 pickle.c -o pickle \
	-lmpv -ldrm -lgbm -lEGL -lGLESv2 -lpthread -lm
```

If `pkg-config` is preferred:
```
gcc -O2 -Wall -std=c11 pickle.c -o pickle $(pkg-config --cflags --libs mpv gbm egl glesv2) -ldrm
```

## Performance Tuning

Pickle includes several optimization features that can be controlled through environment variables:

### CPU and Process Priority
```
# Set real-time priority (1-99, requires root or CAP_SYS_NICE)
PICKLE_PRIORITY=10 sudo ./pickle video.mp4

# Assign process to specific CPU cores (e.g., cores 2 and 3)
PICKLE_CPU_AFFINITY=2,3 ./pickle video.mp4
```

### Rendering Optimizations
```
# Enable/disable frame change detection (1=enabled, 0=disabled)
PICKLE_SKIP_UNCHANGED=1 ./pickle video.mp4

# Enable/disable direct rendering path when possible (1=enabled, 0=disabled)
PICKLE_DIRECT_RENDERING=1 ./pickle video.mp4

# Completely disable keystone for maximum performance
PICKLE_DISABLE_KEYSTONE=1 ./pickle video.mp4
```

### Diagnostics
```
# Enable detailed frame timing logs
PICKLE_FRAME_TIMING=1 ./pickle video.mp4

# Enable performance statistics
PICKLE_STATS=1 ./pickle video.mp4
```

### Playback
```
# Enable looping
PICKLE_LOOP=1 ./pickle video.mp4
```

### Performance Tips for Raspberry Pi 4

1. Use the maximum performance build option:
   ```
   make RELEASE=1 MAXPERF=1 RPI4_OPT=1
   ```

2. Disable keystone correction if not needed:
   ```
   PICKLE_DISABLE_KEYSTONE=1 ./pickle video.mp4
   ```

3. Run with elevated priority and CPU affinity:
   ```
   PICKLE_PRIORITY=10 PICKLE_CPU_AFFINITY=2,3 sudo ./pickle video.mp4
   ```

4. Use small, efficiently encoded videos (H.264/H.265) for best results.

5. Make sure your Raspberry Pi has adequate cooling and is not throttling due to overheating.

## Run
You need DRM master access (non-root possible if logind/seatd grants it and your user is in correct groups).

Quick start (Makefile):
```
make
make preflight          # optional environment checks
make try-run VIDEO=vid.mp4   # attempt non-root run on a free TTY
make run VIDEO=vid.mp4       # uses sudo for root run
```

Command line options:
```
./pickle [options] video_file
  -l, --loop            Loop playback continuously
  -h, --help            Show this help message
```

Manual run (root):
```
sudo ./pickle /path/to/video.mp4
```
Or with looping:
```
sudo ./pickle -l /path/to/video.mp4
sudo ./pickle --loop /path/to/video.mp4
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

## Zero-Copy and Atomic Modesetting

Pickle implements two key optimizations for improved performance and display quality:

### DMA-BUF Zero-Copy Path

The DMA-BUF zero-copy path eliminates unnecessary buffer copies between the GPU and display controller:

- Directly shares buffers between GPU and display using Linux DMA-BUF mechanism
- Reduces memory bandwidth usage and CPU overhead
- Automatically detected and used when supported by hardware
- Falls back to standard path when unsupported

### Atomic Modesetting

Atomic modesetting provides tear-free display updates:

- Uses the modern DRM atomic API for synchronized display updates
- All display changes happen in one synchronized operation
- Eliminates tearing artifacts during video playback
- Automatically used when supported by the driver
- Falls back to legacy modesetting when unsupported

These features are automatically enabled when supported by your hardware and drivers, with no additional configuration required.

## V4L2 Direct Decoder Path

Pickle can utilize the V4L2 Media Memory-to-Memory (M2M) API for direct hardware video decoding on Raspberry Pi 4, bypassing MPV for improved performance:

- Directly accesses the V4L2 decoder hardware without going through MPV
- Reduces CPU usage and memory overhead
- Improves playback performance for high-resolution content
- Automatically falls back to MPV on unsupported platforms

To use the V4L2 decoder:
```
./pickle --v4l2 video.mp4
```

Requirements:
- Raspberry Pi 4 or newer with V4L2 codec driver support
- Linux kernel with V4L2 M2M support

The V4L2 decoder path is optimized for:
- H.264 and H.265 content
- Proper synchronization with display refresh
- Direct integration with the HVS keystone implementation

## Keystone Correction

Pickle supports keystone correction for projector use, allowing you to adjust the image geometry when projecting onto non-perpendicular surfaces:

1. Start with keystone mode enabled:
   ```
   PICKLE_KEYSTONE=1 ./pickle video.mp4
   ```
   
2. Use the following keyboard controls:
   - `k` - Toggle keystone mode on/off
   - `1`, `2`, `3`, `4` - Select corner to adjust (1 = top-left, 2 = top-right, etc.)
   - `w`, `a`, `s`, `d` - Move selected corner (up, left, down, right)
   - `+`, `-` - Increase/decrease adjustment step size
   - `r` - Reset keystone to default
   - `q` - Quit

3. Keystone settings are saved to `~/.config/pickle_keystone.conf` and loaded automatically on next run.

Environment variables for keystone:
   - `PICKLE_KEYSTONE=1` - Enable keystone correction
   - `PICKLE_KEYSTONE_STEP=n` - Set keystone adjustment step size (1-100)

### Hardware-Accelerated HVS Keystone

On Raspberry Pi 4, Pickle can use the Hardware Video Scaler (HVS) for hardware-accelerated keystone correction:

- Automatically detected and enabled on Raspberry Pi 4 hardware
- Significantly reduces CPU/GPU load during keystone correction
- Uses the same keystone settings and controls as the software implementation
- Seamlessly falls back to software implementation on non-RPi platforms

The HVS keystone feature has been removed (DispmanX deprecated).
Hardware keystone now uses DRM/KMS which requires:
- Modern Linux kernel with DRM/KMS support
- Running with `dtoverlay=vc4-kms-v3d` in config.txt (on Raspberry Pi)

To get the best performance with HVS keystone:
```
make RELEASE=1 MAXPERF=1 RPI4_OPT=1 rpi4-release
PICKLE_KEYSTONE=1 ./pickle video.mp4
```

## Visual Aids

Pickle provides several visual aids to help with video alignment and visibility:

1. Border around video (useful for seeing edges against dark backgrounds):
   ```
   PICKLE_SHOW_BORDER=5 ./pickle video.mp4  # 5 is the border width in pixels
   ```

2. Light background (helps with seeing dark video edges):
   ```
   PICKLE_SHOW_BACKGROUND=1 ./pickle video.mp4
   ```

Keyboard controls for visual aids:
   - `b` - Toggle border on/off
   - `[`, `]` - Decrease/increase border width
   - `g` - Toggle light background on/off

Environment variables for visual aids:
   - `PICKLE_SHOW_BORDER=width` - Show border with specified width (1-50 pixels)
   - `PICKLE_SHOW_BACKGROUND=1` - Show light background

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
./pickle vid.mp4
```

If you need to measure CPU usage differences, compare with and without `PERF=1` using `pidstat -p <pid> 1` or `perf top`.

Environment variables summary (performance-related):
* `PICKLE_FORCE_RENDER_LOOP=1`  Force legacy continuous rendering loop.
* `PICKLE_LOOP=1`               Loop playback continuously (can also use -l/--loop flag).
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

**Keystone Correction:**
* `PICKLE_KEYSTONE=1`         Enable keystone correction mode
* `PICKLE_KEYSTONE_STEP=n`    Set keystone adjustment step size (1-100, default 10)

**Visual Aids:**
* `PICKLE_SHOW_BORDER=n`      Show border around video with width n pixels (1-50)
* `PICKLE_SHOW_BACKGROUND=1`  Show light background for better edge visibility

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

Future potential enhancements:
* Use `drmModeAtomicCommit` with retained planes for overlay/subtitle blending.
* Switch to `drmModeAddFB2` with modifiers for direct scanout of more formats.
* Explicit FPS pacing (clock vs. display refresh) to reduce latency jitter.
* ✅ Batch event polling (epoll on DRM fd + signalfd) instead of select per frame.
* Add lock-free ring buffer for frame queue.
* Create Vulkan video decoder abstraction.

## Event-Driven Architecture

Pickle now uses an epoll-based event-driven architecture to efficiently handle events from various sources:

- **Efficient I/O Multiplexing**: Uses `epoll` instead of `poll` for better performance with multiple file descriptors
- **Callback-Based Design**: Event handlers are registered for each event source
- **Signal Integration**: Proper signal handling via `signalfd` for clean shutdown
- **Timer Support**: Precise timers using `timerfd` for regular operations like frame updates
- **Modular Structure**: Clean separation of event handling from application logic
- **Extensibility**: Easy to add new event sources and handlers

This implementation provides a more scalable and maintainable approach to event handling, particularly important for embedded systems like the Raspberry Pi.

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


bluetoothctl
agent on
default-agent
power on
discoverable on
pairable on
scan on

Device E4:17:D8:6B:3B:35 8BitDo Zero 2 gamepad
pair <MAC_ADDRESS>
trust <MAC_ADDRESS>
connect <MAC_ADDRESS>



sudo ddrescue -b 32M -v /dev/sda /home/dilly/Projects/pickle/images/rpi4_backup.img /home/dilly/Projects/pickle/images/rpi4_backup.log



ffmpeg -i vid.mp4 -c:v h264_videotoolbox -profile:v baseline -pix_fmt yuv420p -c:a copy rpi4-a.mp4

 ffmpeg -i vid.mp4 -c:v libx264 -b:v 8M -profile:v main -level 4.2 -pix_fmt yuv420p -crf 20 -bf 0 -c:a copy rpi4-e.mp4

best

ffmpeg -i vid.mp4   -c:v libx264 -profile:v baseline -level:v 4.2 -preset slow -crf 18   -maxrate 8M -bufsize 16M -r 60 -g 60 -bf 2 -pix_fmt yuv420p   -colorspace unknown -color_primaries unknown -color_trc unknown   -movflags +faststart   vid-a.mp4