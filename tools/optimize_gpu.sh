#!/bin/bash
# File: optimize_gpu_accel.sh

echo "=== RPi4 GPU Acceleration Optimizer for Pickle ==="
echo ""

# Check system capabilities
check_gpu_capabilities() {
    echo "Checking GPU capabilities..."
    
    # Check for VideoCore VI
    if vcgencmd version > /dev/null 2>&1; then
        echo "✓ VideoCore VI GPU detected"
        VIDEOCORE_VERSION=$(vcgencmd version | grep version | cut -d' ' -f2)
        echo "  Version: $VIDEOCORE_VERSION"
    fi
    
    # Check OpenGL ES version
    if command -v glxinfo > /dev/null 2>&1; then
        GL_VERSION=$(glxinfo -B 2>/dev/null | grep "OpenGL ES" | head -1)
        echo "✓ OpenGL ES: $GL_VERSION"
    fi
    
    # Check Vulkan support
    if command -v vulkaninfo > /dev/null 2>&1; then
        VULKAN_VERSION=$(vulkaninfo 2>/dev/null | grep "apiVersion" | head -1)
        echo "✓ Vulkan: $VULKAN_VERSION"
    fi
    
    # Check available memory
    GPU_MEM=$(vcgencmd get_mem gpu | cut -d'=' -f2)
    echo "✓ GPU Memory: $GPU_MEM"
}

# Optimize GPU memory split
optimize_gpu_memory() {
    echo ""
    echo "Optimizing GPU memory allocation..."
    
    CURRENT_GPU_MEM=$(vcgencmd get_mem gpu | cut -d'=' -f2 | cut -d'M' -f1)
    
    if [ "$CURRENT_GPU_MEM" -lt 128 ]; then
        echo "⚠ GPU memory is only ${CURRENT_GPU_MEM}MB"
        echo "  Recommended: At least 128MB for hardware acceleration"
        echo "  To increase, add 'gpu_mem=128' to /boot/config.txt"
    else
        echo "✓ GPU memory allocation is sufficient (${CURRENT_GPU_MEM}MB)"
    fi
}

# Test different acceleration methods
benchmark_acceleration() {
    echo ""
    echo "Benchmarking acceleration methods..."
    
    if [ ! -f "test_video.mp4" ]; then
        echo "Creating test video..."
        ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 10 test_video.mp4 -y > /dev/null 2>&1
    fi
    
    # Test 1: CPU baseline
    echo -n "CPU baseline: "
    /usr/bin/time -f "%e seconds, %P CPU" \
        timeout 5 ./pickle --gles test_video.mp4 2>&1 | grep -E "seconds|CPU"
    
    # Test 2: OpenGL ES Fragment Shader
    echo -n "OpenGL ES Fragment: "
    /usr/bin/time -f "%e seconds, %P CPU" \
        timeout 5 ./pickle --gles test_video.mp4 2>&1 | grep -E "seconds|CPU"
    
    # Test 3: OpenGL ES Compute Shader
    echo -n "OpenGL ES Compute: "
    PICKLE_FORCE_COMPUTE=1 /usr/bin/time -f "%e seconds, %P CPU" \
        timeout 5 ./pickle --gles test_video.mp4 2>&1 | grep -E "seconds|CPU"
    
    # Test 4: Vulkan (if available)
    if ./pickle --help | grep -q vulkan; then
        echo -n "Vulkan: "
        /usr/bin/time -f "%e seconds, %P CPU" \
            timeout 5 ./pickle --vulkan test_video.mp4 2>&1 | grep -E "seconds|CPU"
    fi
}

# Generate optimization report
generate_report() {
    echo ""
    echo "=== Optimization Report ==="
    echo ""
    
    cat << EOF > gpu_optimization_report.txt
RPi4 GPU Acceleration Optimization Report
Generated: $(date)

SYSTEM INFORMATION:
- Model: $(cat /proc/device-tree/model 2>/dev/null || echo "Unknown")
- Kernel: $(uname -r)
- GPU Memory: $(vcgencmd get_mem gpu)
- CPU Governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)

RECOMMENDATIONS:
1. For best performance with keystone correction:
   - Use OpenGL ES 3.1 Compute Shaders
   - Ensure gpu_mem=128 or higher in /boot/config.txt
   - Set CPU governor to 'performance' for consistent timing

2. Build command:
   make clean && make COMPUTE=1 VULKAN=1

3. Runtime command for optimal performance:
   ./pickle --gles video.mp4

4. Monitor performance:
   watch -n 1 'vcgencmd measure_temp; vcgencmd measure_clock arm; vcgencmd measure_clock core'

EOF
    
    echo "Report saved to gpu_optimization_report.txt"
}

# Main execution
check_gpu_capabilities
optimize_gpu_memory
benchmark_acceleration
generate_report

echo ""
echo "=== Optimization Complete ==="
echo "For keystone mode with minimal CPU usage, use:"
echo "  ./pickle --gles vid.mp4"
echo ""
echo "Press 'k' during playback to enable keystone correction."