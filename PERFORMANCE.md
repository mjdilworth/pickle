# Pickle Performance Optimizations

This directory contains optimizations for the Pickle video player to improve performance, reduce CPU usage, and lower resource consumption.

## Optimized Build Options

### Building with Performance Optimizations

```bash
# Standard build
make

# Release build (smaller, optimized)
make RELEASE=1

# Maximum performance build with Raspberry Pi 4 optimizations
make RELEASE=1 MAXPERF=1 RPI4_OPT=1

# Or use the provided script
./build_optimized.sh
```

### Compilation Flags

The following options can be combined to optimize the build:

- `RELEASE=1` - Optimized release build (-O3, NDEBUG, strip debug symbols)
- `MAXPERF=1` - Maximum performance flags (native optimizations, fast math, LTO, loop optimizations)
- `RPI4_OPT=1` - Raspberry Pi 4 specific optimizations (Cortex-A72 flags, NEON, etc.)

## Runtime Performance Environment Variables

Pickle supports several environment variables to tune performance:

### CPU and Process Priority

```bash
# Set real-time priority (1-99, requires root or CAP_SYS_NICE)
PICKLE_PRIORITY=10 sudo ./pickle video.mp4

# Assign process to specific CPU cores (e.g., cores 2 and 3)
PICKLE_CPU_AFFINITY=2,3 ./pickle video.mp4
```

### Rendering Optimizations

```bash
# Enable/disable frame change detection (1=enabled, 0=disabled)
PICKLE_SKIP_UNCHANGED=1 ./pickle video.mp4

# Enable/disable direct rendering path when possible (1=enabled, 0=disabled)
PICKLE_DIRECT_RENDERING=1 ./pickle video.mp4

# Disable keystone for maximum performance
PICKLE_DISABLE_KEYSTONE=1 ./pickle video.mp4

# Set specific hardware decoder (important for Raspberry Pi 4)
git bracn video.mp4
```

### Hardware Decoding Options

For Raspberry Pi 4, you may encounter CUDA-related errors when using the default hardware decoder. These occur because MPV tries to use NVIDIA acceleration that isn't available on the RPi.

If you see errors like:
```
[mpv-log] error: AVHWDeviceContext: Cannot load libcuda.so.1
[mpv-log] error: AVHWDeviceContext: Could not dynamically load CUDA
```

Use one of these hardware decoder options specifically optimized for Raspberry Pi:

- `PICKLE_HWDEC=v4l2m2m` - Use Video4Linux2 Memory-to-Memory (best for RPi4)
- `PICKLE_HWDEC=drm` - Use Direct Rendering Manager decoder
- `PICKLE_HWDEC=rkmpp` - If using Rockchip-based boards
- `PICKLE_HWDEC=vaapi` - For systems with VA-API support
- `PICKLE_HWDEC=disabled` - Disable hardware decoding entirely (fallback)

### Advanced Performance Tuning

```bash
# V4L2 buffer configuration (1-32 buffers each)
PICKLE_V4L2_INPUT_BUFFERS=16 PICKLE_V4L2_OUTPUT_BUFFERS=16 ./pickle video.mp4

# V4L2 buffer sizes (1-64MB read buffer, 1KB-1MB chunk size)
PICKLE_V4L2_BUFFER_SIZE_MB=4 PICKLE_V4L2_CHUNK_SIZE_KB=128 ./pickle video.mp4

# Combined high-performance V4L2 settings
PICKLE_V4L2_INPUT_BUFFERS=16 PICKLE_V4L2_OUTPUT_BUFFERS=16 PICKLE_V4L2_BUFFER_SIZE_MB=8 PICKLE_V4L2_CHUNK_SIZE_KB=256 ./pickle video.mp4
```

### Diagnostics

```bash
# Enable detailed frame timing logs
PICKLE_FRAME_TIMING=1 ./pickle video.mp4

# Enable performance statistics
PICKLE_STATS=1 ./pickle video.mp4
```

## Performance Tips for Raspberry Pi 4

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

## Benchmark Script

The included `benchmark.sh` script can be used to compare different optimization settings:

```bash
# Run default benchmark
./benchmark.sh video.mp4

# Run 5 tests of 10 seconds each
./benchmark.sh -d 10 -t 5 video.mp4

# Test with all optimizations
./benchmark.sh -k -a -p video.mp4

# Save results to file
./benchmark.sh -o results.txt video.mp4
```

## Files

- `build_optimized.sh` - Script to build pickle with optimizations
- `benchmark.sh` - Script to benchmark different optimization configurations
- `performance.patch` - Patch file with performance optimizations (for reference)