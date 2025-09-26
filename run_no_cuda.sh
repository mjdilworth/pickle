#!/bin/bash

# Create a temporary MPV config to disable CUDA
mkdir -p ~/.config/mpv
cat > ~/.config/mpv/mpv.conf << EOF
# Disable CUDA and other NVIDIA hardware acceleration
cuda=no
cuda-decode=no
hwdec=v4l2m2m,drm,no
vd-lavc-o=hwdec=v4l2m2m:hwdec-codecs=all
# Use V4L2 hardware acceleration which is best for Raspberry Pi 4
EOF

# Run pickle with V4L2 hardware acceleration
PICKLE_HWDEC=v4l2m2m ./pickle "$@"