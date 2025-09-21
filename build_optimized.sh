#!/bin/bash
# add_performance_flags.sh - Script to add performance flags to compiled binary

set -e

echo "Compiling pickle with performance optimizations..."

# First make a clean build to ensure everything is properly compiled
make clean

# Compile with our performance options
make RELEASE=1 MAXPERF=1 RPI4_OPT=1

# Strip the binary to remove debug symbols
strip pickle

# Display the result
echo "Optimized binary details:"
ls -lh pickle
file pickle

echo "
Performance flags have been applied to the binary.

Use the following environment variables to optimize performance:
---------------------------------------------------------------
# Set real-time priority (1-99, requires root)
PICKLE_PRIORITY=10 sudo ./pickle video.mp4

# Assign process to specific CPU cores (e.g., cores 2 and 3)
PICKLE_CPU_AFFINITY=2,3 ./pickle video.mp4

# Disable frame change detection to always render
PICKLE_SKIP_UNCHANGED=0 ./pickle video.mp4

# Disable direct rendering path for problematic hardware
PICKLE_DIRECT_RENDERING=0 ./pickle video.mp4

# Hardware decoding for Raspberry Pi 4 (fix CUDA/libcuda errors)
PICKLE_HWDEC=v4l2m2m ./pickle video.mp4
"