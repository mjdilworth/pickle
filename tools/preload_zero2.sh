#!/bin/bash
# Preload 8BitDo Zero 2 MAC and mark it as trusted on RPi4

# Replace this with your controller's MAC address
DEVICE_MAC="E4:17:D8:58:F0:35"
RETRIES=5                       # Number of connection attempts
DELAY=2                         # Seconds to wait between attempts

bluetoothctl <<EOF
agent on
default-agent
trust $DEVICE_MAC
quit
EOF

echo "Attempting to connect 8BitDo Zero 2 ($DEVICE_MAC)..."

for i in $(seq 1 $RETRIES); do
    echo "Connection attempt $i..."
    bluetoothctl connect $DEVICE_MAC
    sleep $DELAY
    # Check if device is connected
    STATUS=$(bluetoothctl info $DEVICE_MAC | grep "Connected: yes")
    if [ ! -z "$STATUS" ]; then
        echo "Device connected successfully!"
        exit 0
    fi
done

echo "Failed to connect after $RETRIES attempts. The device may not be on."
exit 1