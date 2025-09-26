# Performance Statistics in Pickle

This document explains the performance statistics overlay in Pickle video player.

## Overview

The performance overlay displays real-time metrics:
- **FPS**: Frames per second
- **CPU**: CPU usage percentage
- **GPU**: GPU usage percentage (based on render time)
- **RAM**: Memory usage in MB

## How to Enable

Press the `v` key while playing a video, or start pickle with the `-v` flag:
```
./pickle -v video.mp4
```

## How Statistics Are Calculated

### FPS
The FPS counter tracks actual frames rendered per second, calculated by counting frames over a short time window.

### CPU Usage
CPU usage is calculated by reading `/proc/stat` and tracking the ratio of idle time to total time across all CPU cores.

### GPU Usage
GPU usage is an approximation based on the time taken to render each frame:

- For Raspberry Pi, 10ms render time is considered "full load" (100%)
- The calculation uses a formula: `gpu_usage = min(100%, (avg_render_time_ms / 10.0) * 100%)`
- This is a conservative estimate that works well for Raspberry Pi hardware
- It's not a perfect measure of GPU utilization but provides a relative indication of rendering load

### Memory Usage
Memory usage is read from `/proc/self/status` to show the current RAM consumption of the Pickle process.

## Interpreting the Results

- **High GPU, Low FPS**: The renderer is struggling to keep up, consider disabling keystone correction or reducing video resolution
- **Low GPU, Low FPS**: Check CPU usage and decoder stats, the bottleneck may be in video decoding
- **High GPU, Good FPS**: The GPU is working efficiently, this is ideal for smooth playback
- **Near 100% GPU**: Consider enabling PICKLE_SKIP_UNCHANGED=1 for better performance

## Optimizing Performance

For best performance on Raspberry Pi, use the run_rpi_optimized.sh script which sets optimal environment variables for playback:

```
./run_rpi_optimized.sh video.mp4
```

This script also suppresses harmless CUDA errors that might appear on non-NVIDIA hardware.