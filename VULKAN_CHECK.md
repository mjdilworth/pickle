# Quick Guide: Is Vulkan GPU Acceleration Working?

This is a simplified guide to check if Vulkan hardware acceleration is being used for keystone correction in Pickle.

## Three Easy Ways to Check

### 1. Use the Check Tool (Easiest)

Run this command to check if your system supports Vulkan hardware acceleration:

```bash
make check-vulkan
```

Look for these results:
- ✅ "Vulkan hardware acceleration is AVAILABLE"
- ❌ "No Vulkan-compatible devices found"

### 2. Compare Performance (Most Reliable)

Run this command to compare GPU vs CPU performance:

```bash
./verify_vulkan_usage.sh --perf
```

What to look for:
- If GPU mode is faster than CPU mode, GPU acceleration is working
- The logs will show timing information in milliseconds
- Smaller values mean better performance

### 3. Check GPU Activity (Visual Method)

Run this command to see GPU activity during playback:

```bash
./verify_vulkan_usage.sh --monitor
```

What to look for:
- GPU usage should increase during video playback
- You should see spikes in GPU compute activity
- CPU usage should be lower compared to non-GPU mode

## Force GPU or CPU Mode

You can explicitly control whether Pickle uses GPU or CPU:

```bash
# Force GPU mode
sudo PICKLE_USE_VULKAN_GPU=1 ./pickle --vulkan video.mp4

# Force CPU mode 
sudo PICKLE_USE_VULKAN_GPU=0 ./pickle --gles video.mp4
```

## Common Issues

1. **No performance difference between GPU and CPU modes:**
   - Your GPU might not support Vulkan compute shaders
   - GPU drivers might be outdated or missing

2. **No GPU activity detected:**
   - Vulkan drivers might not be properly installed
   - Hardware might not support the required features

3. **Error messages during startup:**
   - Check for missing Vulkan libraries
   - Update your GPU drivers

## Need More Help?

For more detailed information, check the full documentation at:
- `docs/vulkan_verification.md`