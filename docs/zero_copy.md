# Atomic Modesetting and Zero-Copy

For optimal performance, Pickle uses a zero-copy path that directly transfers decoded video frames to the display without CPU intervention. This is achieved using DMA-BUF for buffer sharing between the video decoder and display.

## Requirements for Zero-Copy

1. **DMA-BUF Support**: Your GPU and drivers must support DMA-BUF export/import.
2. **Atomic Modesetting**: For the best performance, your DRM driver should support atomic modesetting.

## Enabling Atomic Modesetting

Atomic modesetting provides tear-free, synchronized display updates. Without it, zero-copy will still work but with potential performance limitations.

### On Raspberry Pi 4/5

Add these lines to `/boot/config.txt`:
```
dtoverlay=vc4-kms-v3d
dtparam=atomic=on
```

### On Other Systems

For Intel GPUs, you may need to add the kernel parameter `i915.enable_atomic=1`.

### Diagnostic Tool

Run our diagnostic script to check for atomic modesetting support:
```
sudo ./tools/enable_atomic.sh
```

## Troubleshooting

If you see the message `Atomic modesetting not supported`, zero-copy will still work but with reduced efficiency. Follow the recommendations from the diagnostic script to enable atomic modesetting if your hardware supports it.

For the best performance:
1. Update your graphics drivers
2. Ensure the correct DRM device is selected (usually card1 on Raspberry Pi 4)
3. Enable atomic modesetting if available