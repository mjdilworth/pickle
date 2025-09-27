#!/bin/bash

# Test V4L2 decoder with continuous data feeding
# This will help verify that the decoder works with sustained input

echo "Creating a simple test stream for V4L2 decoder..."

# Generate some test H.264 data using ffmpeg if possible
if command -v ffmpeg >/dev/null 2>&1; then
    echo "Generating test H.264 stream with ffmpeg..."
    ffmpeg -f lavfi -i testsrc2=size=320x240:duration=5:rate=30 -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -pix_fmt yuv420p test_simple.h264 -y 2>/dev/null
    if [ -f test_simple.h264 ]; then
        echo "Generated test_simple.h264 ($(du -h test_simple.h264 | cut -f1))"
        echo "Testing with proper H.264 stream..."
        timeout 15s ./pickle test_simple.h264
        exit $?
    fi
fi

echo "No ffmpeg available or generation failed, testing with existing file..."
echo "This will test V4L2 initialization even with non-H.264 data"
timeout 15s ./pickle test.mp4

echo "V4L2 decoder test completed"