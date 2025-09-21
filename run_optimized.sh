#!/bin/bash
# run_optimized.sh - Run pickle with optimal performance settings for Raspberry Pi 4

# Check if a video file was provided
if [ $# -lt 1 ]; then
    echo "Usage: $0 <video_file> [additional pickle options]"
    echo "Example: $0 video.mp4 --loop"
    exit 1
fi

VIDEO_FILE="$1"
shift  # Remove the first argument, leaving any additional options

# Check if the video file exists
if [ ! -f "$VIDEO_FILE" ]; then
    echo "Error: Video file '$VIDEO_FILE' not found"
    exit 1
fi

echo "Running pickle with optimized settings for Raspberry Pi 4..."

# Use sudo if we're setting real-time priority
if [ "${PICKLE_PRIORITY:-0}" -gt 0 ]; then
    echo "Using real-time priority ${PICKLE_PRIORITY} (requires root privileges)"
    sudo PICKLE_HWDEC=v4l2m2m \
         PICKLE_SKIP_UNCHANGED=1 \
         PICKLE_DIRECT_RENDERING=1 \
         PICKLE_DISABLE_KEYSTONE=1 \
         PICKLE_CPU_AFFINITY="${PICKLE_CPU_AFFINITY:-2,3}" \
         ./pickle "$@" "$VIDEO_FILE"
else
    # Run without sudo
    PICKLE_HWDEC=v4l2m2m \
    PICKLE_SKIP_UNCHANGED=1 \
    PICKLE_DIRECT_RENDERING=1 \
    PICKLE_DISABLE_KEYSTONE=1 \
    PICKLE_CPU_AFFINITY="${PICKLE_CPU_AFFINITY:-2,3}" \
    ./pickle "$@" "$VIDEO_FILE"
fi