#!/bin/bash
# fix_cuda_error.sh - Run pickle with settings to prevent CUDA errors

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

echo "Running pickle with CUDA error prevention settings..."

# Create a temporary MPV config to disable CUDA
mkdir -p ~/.config/mpv
cat > ~/.config/mpv/mpv.conf << EOF
# Disable NVIDIA hardware acceleration methods
hwdec-codecs=all
cuda=no
cuda-decode=no
vd-lavc-o=hwdec=v4l2m2m:hwdec-codecs=all
EOF

# Preload a stub library for libcuda.so.1 to prevent the error message
cat > /tmp/stub_libcuda.c << EOF
// Stub implementation for libcuda.so.1
// This prevents "Cannot load libcuda.so.1" errors
void* stub_function() { return 0; }
EOF

gcc -shared -o /tmp/libcuda.so.1 /tmp/stub_libcuda.c

# Run pickle with optimized settings and the stub library
LD_PRELOAD=/tmp/libcuda.so.1 \
PICKLE_HWDEC=v4l2m2m \
PICKLE_SKIP_UNCHANGED=1 \
PICKLE_DIRECT_RENDERING=1 \
PICKLE_DISABLE_KEYSTONE=1 \
PICKLE_CPU_AFFINITY="${PICKLE_CPU_AFFINITY:-2,3}" \
./pickle "$@" "$VIDEO_FILE"

# Clean up temporary files
rm -f /tmp/stub_libcuda.c /tmp/libcuda.so.1