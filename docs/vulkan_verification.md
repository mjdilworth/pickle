# Verifying Vulkan GPU Acceleration for Keystone Correction

This document explains methods to verify that Vulkan hardware GPU acceleration is being used for keystone correction in Pickle.

## Prerequisites

Ensure you have the following:

- Vulkan-compatible GPU
- Vulkan drivers installed
- Pickle built with Vulkan support (`make VULKAN=1`)

## Method 1: Using the Vulkan Hardware Check Tool

The simplest way to check if your system supports Vulkan hardware acceleration:

```bash
# Build and run the Vulkan hardware check
make check-vulkan
```

This tool will display:
- List of available Vulkan-compatible devices
- Device types (integrated GPU, discrete GPU, etc.)
- Compute shader support (required for keystone correction)
- Other relevant hardware features

If this tool shows at least one GPU device with compute shader support, your system can use Vulkan hardware acceleration for keystone correction.

## Method 2: Verification Script

A comprehensive verification script is provided to test multiple aspects of Vulkan usage:

```bash
# Make the script executable (if needed)
chmod +x verify_vulkan_usage.sh

# Check Vulkan hardware capabilities
./verify_vulkan_usage.sh --check

# Monitor GPU usage during playback
./verify_vulkan_usage.sh --monitor --file your_video.mp4

# Compare performance between GPU and CPU
./verify_vulkan_usage.sh --perf --file your_video.mp4

# Run all verification methods
./verify_vulkan_usage.sh --check --monitor --perf
```

## Method 3: Environment Variables and Performance Logs

Pickle supports environment variables to control and monitor Vulkan usage:

```bash
# Force GPU acceleration and enable performance logging
sudo PICKLE_USE_VULKAN_GPU=1 PICKLE_PERF_LOG=1 ./pickle --vulkan video.mp4

# Force CPU-only processing and enable performance logging
sudo PICKLE_USE_VULKAN_GPU=0 PICKLE_PERF_LOG=1 ./pickle --gles video.mp4
```

The performance logs will show timing information that you can compare between GPU and CPU modes.

## How to Interpret Results

### Hardware Check

- If the hardware check shows Vulkan-compatible GPUs with compute shader support, your hardware supports GPU acceleration.
- If no devices are found or compute shader support is missing, you'll fall back to CPU-based processing.

### GPU Monitoring

When monitoring GPU usage:
- You should see increased GPU activity during keystone correction.
- The specific metrics depend on your GPU and monitoring tools.
- For NVIDIA: Look for increased GPU utilization and compute usage.
- For AMD: Look for increased GPU and compute shader usage.
- For Intel: Look for increased render/compute engine usage.

### Performance Comparison

When comparing GPU vs CPU performance:
- GPU acceleration should generally be faster, especially for higher resolutions.
- The logs will show timing information in milliseconds.
- Look for differences in the "keystone_correction_time" metric.
- Smaller values indicate better performance.

## Troubleshooting

If Vulkan hardware acceleration isn't working:

1. Check if your GPU supports Vulkan compute shaders
2. Ensure proper Vulkan drivers are installed
3. Check for any error messages in the Pickle output
4. Try updating your GPU drivers
5. Check if other Vulkan applications work correctly

## Additional Notes

- Performance benefits are most noticeable at higher resolutions
- Some integrated GPUs may show minimal performance improvement
- CPU usage should decrease when GPU acceleration is working correctly