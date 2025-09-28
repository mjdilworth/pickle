#!/bin/bash

# Fix Bluetooth gamepad power management issues
# This prevents Bluetooth gamepads from going to sleep

echo "Disabling power management for Bluetooth controllers..."

# Disable power management for all Bluetooth adapters
for control_file in /sys/class/bluetooth/hci*/power/control; do
    if [ -w "$control_file" ]; then
        echo "on" | sudo tee "$control_file" > /dev/null
        echo "Disabled power management: $control_file"
    fi
done

# Find and disable power management for 8BitDo Zero 2 gamepad specifically
for device_path in /sys/devices/platform/soc/*/bluetooth/hci*/hci*:*/0005:045E:02E0.*/power/control; do
    if [ -w "$device_path" ]; then
        echo "on" | sudo tee "$device_path" > /dev/null
        echo "Disabled power management for 8BitDo Zero 2: $device_path"
    fi
done

# Generic approach for any Bluetooth gamepad
for device_path in /sys/devices/platform/soc/*/bluetooth/hci*/hci*:*/*/power/control; do
    if [ -w "$device_path" ]; then
        # Check if this is a HID device (likely gamepad)
        device_dir=$(dirname "$device_path")
        if ls "$device_dir"/../input/input*/js* &>/dev/null; then
            echo "on" | sudo tee "$device_path" > /dev/null
            echo "Disabled power management for gamepad: $device_path"
        fi
    fi
done

echo "Bluetooth gamepad power management fixes applied."
echo "This will need to be run after each reboot or gamepad reconnection."