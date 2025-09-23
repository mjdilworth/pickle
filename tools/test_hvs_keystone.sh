#!/bin/bash
# Test script for HVS keystone correction on Raspberry Pi

# Check if running on Raspberry Pi
if ! grep -q "Raspberry Pi" /proc/device-tree/model 2>/dev/null; then
    echo "This script should be run on a Raspberry Pi device."
    exit 1
fi

# Check if DispmanX is available
if ! command -v vcgencmd &> /dev/null; then
    echo "vcgencmd not found. Please install Raspberry Pi utilities."
    exit 1
fi

# Check if HVS is enabled
if [ "$(vcgencmd get_config dispmanx 2>/dev/null)" != "dispmanx=1" ]; then
    echo "Warning: DispmanX may not be enabled. Check your Raspberry Pi configuration."
fi

# Build with DispmanX support
echo "Building with DispmanX support..."
make clean
make DISPMANX=1 -j4

# Test with a sample video
if [ ! -f "vid.mp4" ]; then
    echo "Sample video not found. Please provide a test video file named 'vid.mp4'."
    exit 1
fi

# Run with keystone configuration
echo "Running with HVS keystone correction..."
echo "Press 'K' to enable keystone correction, '1'-'4' to select corners, and arrow keys to adjust."
echo "Press 'Q' to quit."

# Run with verbose output to see HVS messages
./pickle vid.mp4 -v

# Check the log for HVS keystone messages
echo "Checking log for HVS keystone messages..."
grep "HVS" output.log

# Print summary
if grep -q "HVS keystone transformation applied" output.log; then
    echo "SUCCESS: HVS keystone correction was applied successfully."
else
    echo "FAILURE: HVS keystone correction did not apply correctly."
    echo "Check the log file for details."
fi