#!/bin/bash
# run_rpi_optimized.sh - Run pickle with optimal performance settings for Raspberry Pi

# Set default values
VIDEO_FILE=""
OPTIONS=""

# Parse arguments
while [ $# -gt 0 ]; do
    # If argument starts with -, it's an option
    if [[ "$1" == -* ]]; then
        OPTIONS="$OPTIONS $1"
    else
        # Otherwise, it's the video file
        VIDEO_FILE="$1"
    fi
    shift
done

# Check if a video file was provided
if [ -z "$VIDEO_FILE" ]; then
    echo "Usage: $0 [options] <video_file>"
    echo "Example: $0 -v video.mp4"
    echo "Options:"
    echo "  -v     Enable performance stats overlay"
    echo "  --loop Loop playback"
    exit 1
fi

# Check if the video file exists
if [ ! -f "$VIDEO_FILE" ]; then
    echo "Error: Video file '$VIDEO_FILE' not found"
    exit 1
fi

echo "======================================================================="
echo "Running pickle with optimized settings for Raspberry Pi"
echo "======================================================================="
echo "Note: You may see 'Cannot load libcuda.so.1' errors - these can be"
echo "safely ignored as they don't affect playback on Raspberry Pi."
echo "======================================================================="

# Create a temporary MPV config file
mkdir -p ~/.config/mpv
cat > ~/.config/mpv/mpv.conf << EOF
# Optimized MPV settings for Raspberry Pi
vo=gpu
hwdec=v4l2m2m
hwdec-codecs=all
profile=low-latency
cuda=no
cuda-decode=no
# Set message level to hide less important messages
msg-level=all=warn
EOF

# Build a CUDA stub library if it doesn't exist
if [ ! -f ./libcuda.so.1 ]; then
    echo "Building CUDA stub library..."
    cc -shared -fPIC -o libcuda.so.1 stub_libcuda.c
fi

# Run pickle with optimized settings and filter out CUDA errors
PICKLE_HWDEC=v4l2m2m \
PICKLE_SKIP_UNCHANGED=1 \
PICKLE_DIRECT_RENDERING=1 \
PICKLE_DISABLE_KEYSTONE=${PICKLE_DISABLE_KEYSTONE:-0} \
PICKLE_CPU_AFFINITY="${PICKLE_CPU_AFFINITY:-2,3}" \
LD_PRELOAD=./libcuda.so.1 ./pickle $OPTIONS "$VIDEO_FILE" 2> >(grep -v "Cannot load libcuda\|Could not dynamically load CUDA" >&2)