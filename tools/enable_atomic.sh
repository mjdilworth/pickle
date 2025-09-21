#!/bin/bash
# Script to enable atomic modesetting for Pickle video player
# This script checks for atomic modesetting support and provides guidance on enabling it

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}Pickle Zero-Copy Optimization Helper${NC}"
echo "This script will check your system for atomic modesetting support"
echo

# Check if running as root
if [ "$EUID" -ne 0 ]; then
  echo -e "${YELLOW}Not running as root. Some checks may be limited.${NC}"
fi

# Check for libdrm-tests
if ! which modetest &>/dev/null; then
  echo -e "${YELLOW}modetest not found. For better diagnostics, install libdrm-tests:${NC}"
  echo "  sudo apt install libdrm-tests    # For Debian/Ubuntu/Raspberry Pi OS"
  echo "  sudo dnf install libdrm-utils    # For Fedora/RHEL"
  echo
fi

# List available DRM devices
echo -e "${BLUE}Available DRM devices:${NC}"
ls -la /dev/dri/card* 2>/dev/null || echo "  No DRM devices found!"

# Try to determine the driver for each card
echo
echo -e "${BLUE}Checking driver information:${NC}"
for card in /dev/dri/card*; do
  if [ -e "$card" ]; then
    card_num=$(echo "$card" | grep -o "[0-9]\+")
    echo -e "Card $card_num:"
    
    # Check if we can access the driver info
    if [ -r "/sys/class/drm/card$card_num/device/driver" ]; then
      driver=$(readlink -f "/sys/class/drm/card$card_num/device/driver" | xargs basename)
      echo -e "  Driver: ${GREEN}$driver${NC}"
    else
      echo -e "  Driver: ${YELLOW}Unknown (cannot access)${NC}"
    fi
    
    # Try to use modetest to check for atomic support
    if which modetest &>/dev/null; then
      if modetest -c $card_num -a 2>&1 | grep -q "atomic test succeeded"; then
        echo -e "  Atomic support: ${GREEN}YES${NC}"
      else
        echo -e "  Atomic support: ${RED}NO${NC}"
      fi
    else
      echo -e "  Atomic support: ${YELLOW}Unknown (modetest not available)${NC}"
    fi
  fi
done

echo
echo -e "${BLUE}Recommendations for enabling atomic modesetting:${NC}"

# Raspberry Pi specific recommendations
if grep -q "Raspberry Pi" /proc/device-tree/model 2>/dev/null || grep -q "BCM" /proc/cpuinfo 2>/dev/null; then
  echo -e "${YELLOW}Raspberry Pi detected.${NC}"
  echo "For Raspberry Pi 4/5 with vc4 driver, add these to /boot/config.txt:"
  echo "  dtoverlay=vc4-kms-v3d"
  echo "  dtparam=atomic=on"
  echo
  echo "Then reboot with: sudo reboot"
fi

# For Intel GPUs
if lspci 2>/dev/null | grep -i vga | grep -i intel &>/dev/null; then
  echo -e "${YELLOW}Intel GPU detected.${NC}"
  echo "Ensure you have the latest i915 driver and add this kernel parameter:"
  echo "  i915.enable_atomic=1"
  echo
  echo "Add to GRUB_CMDLINE_LINUX in /etc/default/grub, then run:"
  echo "  sudo update-grub && sudo reboot"
fi

# For AMD GPUs
if lspci 2>/dev/null | grep -i vga | grep -i amd &>/dev/null; then
  echo -e "${YELLOW}AMD GPU detected.${NC}"
  echo "For AMD GPUs with the amdgpu driver, atomic modesetting should be enabled by default."
  echo "Ensure you have the latest amdgpu driver."
fi

# General recommendations
echo
echo -e "${BLUE}General recommendations:${NC}"
echo "1. Update your graphics drivers and kernel to the latest version"
echo "2. Make sure you're using the correct DRM device (card0 or card1)"
echo "3. When running Pickle, specify the DRM device with:"
echo "   sudo ./pickle --drm-device=/dev/dri/card1 vid.mp4"
echo
echo "For more information on atomic modesetting, see:"
echo "  https://en.wikipedia.org/wiki/Direct_Rendering_Manager#Atomic_mode_setting"