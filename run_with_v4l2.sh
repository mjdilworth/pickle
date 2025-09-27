#!/bin/bash

# Script to run pickle with V4L2 hardware decoding enabled

# Build with V4L2 support if not already built
if [ ! -f "./pickle" ] || ! ldd ./pickle 2>/dev/null | grep -q v4l2; then
    echo "Building pickle with V4L2 hardware decoding support..."
    V4L2=1 make
    if [ $? -ne 0 ]; then
        echo "Build failed!"
        exit 1
    fi
fi

# Run pickle with V4L2 hardware decoding
echo "Running pickle with V4L2 hardware decoding..."
echo "Hardware decoders available:"
echo "  - /dev/video10: bcm2835-codec (H.264)"  
echo "  - /dev/video19: rpi-hevc-dec (HEVC)"
echo ""

V4L2=1 ./pickle "$@"