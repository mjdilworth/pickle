#!/bin/bash
set -e

echo "=== Updating system and installing dependencies ==="
sudo apt update
sudo apt install -y git build-essential pkg-config \
    libdrm-dev libudev-dev libv4l-dev \
    libx11-dev libegl1-mesa-dev libgles2-mesa-dev \
    libgbm-dev python3 python3-pip \
    meson ninja-build

echo "=== Building FFmpeg with v4l2-request ==="
cd ~
if [ ! -d ffmpeg-v4l2-request ]; then
    git clone https://github.com/raspberrypi/ffmpeg.git -b rpi-6.1 ffmpeg-v4l2-request
fi
cd ffmpeg-v4l2-request

./configure \
  --enable-gpl \
  --enable-libdrm \
  --enable-v4l2-request \
  --enable-libudev \
  --enable-shared \
  --disable-debug \
  --target-os=linux \
  --arch=arm64

make -j$(nproc)
sudo make install
sudo ldconfig

echo "=== Verifying FFmpeg V4L2 decoders ==="
ffmpeg -decoders | grep v4l2 || echo "⚠️ No v4l2 decoders found!"

echo "=== Building mpv ==="
cd ~
if [ ! -d mpv ]; then
    git clone https://github.com/mpv-player/mpv.git
fi
cd mpv
meson setup build --prefix=/usr --buildtype=release || true
ninja -C build
sudo ninja -C build install

echo "=== Done! Test with: mpv --hwdec=auto vid.mp4 ==="
