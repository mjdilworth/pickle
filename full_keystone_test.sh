#!/bin/bash

echo "=== Keystone Comprehensive Test ==="
echo ""

# Test 1: Reset and verify initial state
echo "Test 1: Initial state (no keystone config)"
rm -f keystone.conf
timeout 1s ./pickle -p -l rpi4-e.mp4 >/dev/null 2>&1 &
sleep 0.5
if [ -f keystone.conf ]; then
    echo "✓ Initial keystone.conf created"
    grep "corner" keystone.conf
else
    echo "✗ keystone.conf not created"
fi
echo ""

# Test 2: Make adjustments and verify they persist
echo "Test 2: Corner adjustments"
(
    echo "1"          # Select corner 1
    sleep 0.1
    printf "\033[A"  # Up arrow
    sleep 0.1
    printf "\033[A"  # Up arrow  
    sleep 0.1
    echo "2"         # Select corner 2
    sleep 0.1
    printf "\033[C"  # Right arrow
    sleep 0.1
    echo "S"         # Save
    sleep 0.2
    echo "q"         # Quit
) | timeout 3s ./pickle -p -l rpi4-e.mp4 2>&1 | grep -E "Adjusted corner"

echo ""
echo "Saved corner values:"
grep "^corner" keystone.conf
echo ""

# Test 3: Verify loading on restart
echo "Test 3: Verify configuration loading on restart"
timeout 1s ./pickle -p -l rpi4-e.mp4 2>&1 | grep "Loaded keystone"

