#!/bin/bash
# Quick test for atomic modesetting support

if grep -q "Raspberry Pi" /proc/device-tree/model 2>/dev/null; then
  echo "Raspberry Pi detected, checking for atomic modesetting support..."
  
  # Check for vc4-kms-v3d overlay
  if grep -q "vc4-kms-v3d" /boot/config.txt 2>/dev/null; then
    echo "✅ vc4-kms-v3d overlay enabled"
  else
    echo "❌ vc4-kms-v3d overlay not enabled. Add 'dtoverlay=vc4-kms-v3d' to /boot/config.txt"
  fi
  
  # Check for atomic=on
  if grep -q "dtparam=atomic=on" /boot/config.txt 2>/dev/null; then
    echo "✅ Atomic modesetting enabled in config.txt"
  else
    echo "❌ Atomic modesetting not explicitly enabled. Add 'dtparam=atomic=on' to /boot/config.txt"
  fi
fi

# Direct check of DRM capabilities
for card in /dev/dri/card*; do
  if [ -e "$card" ]; then
    card_num=$(echo "$card" | grep -o "[0-9]\+")
    echo "Testing card$card_num for atomic modesetting support..."
    
    # Use a modified pickle command that just reports atomic support
    ATOMIC_SUPPORT=$(sudo ./pickle --test-atomic-only --drm-device=$card 2>&1 | grep "Atomic modesetting")
    
    if [[ "$ATOMIC_SUPPORT" == *"initialized successfully"* ]]; then
      echo "✅ Card$card_num supports atomic modesetting"
      exit 0
    else
      echo "❌ Card$card_num does not support atomic modesetting"
    fi
  fi
done

echo "For full diagnostics, run: sudo ./tools/enable_atomic.sh"
exit 1