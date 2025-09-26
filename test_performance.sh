#!/bin/bash

echo "=== PICKLE FPS PERFORMANCE TEST ==="
echo

echo "Testing normal mode (VSync enabled)..."
timeout 8s ./pickle vid.mp4 &
PID1=$!
sleep 2
echo "v" > /proc/$PID1/fd/0 2>/dev/null || echo "Normal mode test running..."
wait $PID1
echo

echo "Testing high-performance mode (VSync disabled)..."
timeout 8s ./pickle --high-performance vid.mp4 &
PID2=$!
sleep 2
echo "v" > /proc/$PID2/fd/0 2>/dev/null || echo "High-performance mode test running..."
wait $PID2
echo

echo "=== Performance comparison complete ==="
echo "Check the stats overlay (press 'v') to see FPS improvements!"
echo
echo "New performance options available:"
echo "  --no-vsync           : Disable VSync for maximum framerate"
echo "  --high-performance   : Enable all performance optimizations"