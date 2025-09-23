# HVS Keystone Correction for Raspberry Pi

This document describes how to use the Hardware Video Scaler (HVS) keystone correction feature in the Pickle video player on Raspberry Pi devices.

## Overview

Keystone correction is a feature that allows you to correct geometric distortion in projected images. When a projector is not perfectly perpendicular to the projection surface, the image appears as a trapezoid instead of a rectangle. Keystone correction transforms the image to compensate for this distortion.

The Pickle player implements keystone correction in three ways:
1. **GPU-based correction** (all platforms): Uses OpenGL shaders to correct distortion
2. **Compute shader-based correction** (requires OpenGL ES 3.1): Uses compute shaders for faster correction
3. **HVS-based correction** (Raspberry Pi only): Uses the dedicated Hardware Video Scaler for hardware-accelerated correction

## Hardware Requirements

HVS keystone correction requires:
- Raspberry Pi 4 or newer
- VideoCore VI GPU
- DispmanX support

## Enabling HVS Keystone

To enable HVS keystone correction:

1. Make sure the DispmanX feature is enabled in the build (default on Raspberry Pi):
   ```
   make DISPMANX=1
   ```

2. Enable keystone correction in the player by pressing `K` during playback

3. Adjust the keystone corners using:
   - `1`-`4`: Select a corner
   - Arrow keys: Move the selected corner
   - `S`: Save keystone configuration to `keystone.conf`
   - `L`: Load keystone configuration from `keystone.conf`

## Performance Benefits

Using HVS keystone correction provides several benefits:
- Lower CPU/GPU usage compared to shader-based correction
- Better image quality due to hardware-accelerated scaling
- Reduced power consumption
- Higher frame rates during playback

## Troubleshooting

If HVS keystone correction is not working:

1. Check if your system is supported:
   ```
   cat /proc/device-tree/model
   ```
   Should show "Raspberry Pi 4" or newer

2. Make sure DispmanX is available:
   ```
   vcgencmd get_config dispmanx
   ```
   Should return "dispmanx=1"

3. Check the Pickle log output for any errors:
   ```
   grep "HVS" output.log
   ```

4. If HVS is not available, Pickle will automatically fall back to GPU-based keystone correction.

## Technical Details

The HVS keystone correction uses the DispmanX API to create a display element with a quadrilateral destination shape. The hardware automatically maps the rectangular source image to this quadrilateral, providing efficient and high-quality geometric correction.

The implementation can be found in `hvs_keystone.c` and `hvs_keystone.h`.