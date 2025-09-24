#!/bin/bash
# test_vulkan_keystone.sh - Script to demonstrate Vulkan GPU vs CPU keystone correction
#
# This script runs two tests with identical keystone correction settings:
# 1. Using Vulkan GPU acceleration
# 2. Using CPU-only processing
# 
# It then displays the performance difference to verify GPU acceleration.
#

# ANSI color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Default settings
VIDEO_FILE="vid.mp4"
TEST_DURATION=10
SHOW_DIFF=1
KEYSTONE_PRESET="medium"

# Function to show help
show_help() {
    echo -e "${CYAN}Vulkan Keystone Test${NC}"
    echo "This script demonstrates the difference between GPU and CPU keystone correction."
    echo
    echo "Usage: $0 [options]"
    echo
    echo "Options:"
    echo "  -h, --help             Show this help message"
    echo "  -v, --video FILE       Video file to use (default: vid.mp4)"
    echo "  -t, --time SECONDS     Test duration in seconds (default: 10)"
    echo "  -k, --keystone PRESET  Keystone preset to use (default: medium)"
    echo "                         Options: light, medium, heavy, extreme"
    echo "  -n, --no-diff          Don't show the performance difference"
    echo
    echo "Examples:"
    echo "  $0                     # Run with default settings"
    echo "  $0 -v myvideo.mp4 -t 5  # Use specific video and shorter time"
    echo "  $0 -k heavy            # Use heavy keystone preset for more GPU load"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -v|--video)
            VIDEO_FILE="$2"
            shift 2
            ;;
        -t|--time)
            TEST_DURATION="$2"
            shift 2
            ;;
        -k|--keystone)
            KEYSTONE_PRESET="$2"
            shift 2
            ;;
        -n|--no-diff)
            SHOW_DIFF=0
            shift
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}"
            show_help
            exit 1
            ;;
    esac
done

# Check if video file exists
if [[ ! -f "$VIDEO_FILE" ]]; then
    echo -e "${RED}ERROR: Video file '$VIDEO_FILE' not found.${NC}"
    exit 1
fi

# Set keystone settings based on preset
case "$KEYSTONE_PRESET" in
    light)
        KEYSTONE_SETTINGS="0.05,0.05,-0.05,0.05,-0.05,-0.05,0.05,-0.05"
        ;;
    medium)
        KEYSTONE_SETTINGS="0.1,0.1,-0.1,0.1,-0.1,-0.1,0.1,-0.1"
        ;;
    heavy)
        KEYSTONE_SETTINGS="0.15,0.15,-0.15,0.15,-0.15,-0.15,0.15,-0.15"
        ;;
    extreme)
        KEYSTONE_SETTINGS="0.2,0.2,-0.2,0.2,-0.2,-0.2,0.2,-0.2"
        ;;
    *)
        echo -e "${RED}ERROR: Invalid keystone preset '$KEYSTONE_PRESET'${NC}"
        echo -e "${YELLOW}Valid options: light, medium, heavy, extreme${NC}"
        exit 1
        ;;
esac

# Create a temporary directory for test results
TEMP_DIR=$(mktemp -d)
GPU_LOG="$TEMP_DIR/gpu_perf.log"
CPU_LOG="$TEMP_DIR/cpu_perf.log"

echo -e "${CYAN}=== Vulkan GPU vs CPU Keystone Test ===${NC}"
echo -e "Video: ${YELLOW}$VIDEO_FILE${NC}"
echo -e "Duration: ${YELLOW}$TEST_DURATION seconds${NC}"
echo -e "Keystone Preset: ${YELLOW}$KEYSTONE_PRESET${NC}"
echo

# Function to extract average keystone correction time
extract_avg_time() {
    local log_file="$1"
    local avg_time=$(grep "keystone_correction_time" "$log_file" | awk '{ sum += $2; count++ } END { if (count > 0) print sum/count; else print "N/A" }')
    echo "$avg_time"
}

# Run test with GPU acceleration
echo -e "${CYAN}Test 1: With Vulkan GPU Acceleration${NC}"
echo -e "${YELLOW}Running test...${NC}"

# Enable detailed performance logging and GPU acceleration
export PICKLE_USE_VULKAN_GPU=1
export PICKLE_PERF_LOG=1
export PICKLE_KEYSTONE_CORNERS="$KEYSTONE_SETTINGS"

# Run pickle with keystone enabled and capture output
sudo PICKLE_USE_VULKAN_GPU=1 PICKLE_PERF_LOG=1 PICKLE_KEYSTONE_CORNERS="$KEYSTONE_SETTINGS" \
    ./pickle --vulkan "$VIDEO_FILE" > "$GPU_LOG" 2>&1

echo -e "${GREEN}GPU test completed.${NC}"

# Run test with CPU-only processing
echo -e "\n${CYAN}Test 2: With CPU-Only Processing${NC}"
echo -e "${YELLOW}Running test...${NC}"

# Disable GPU acceleration but keep performance logging
export PICKLE_USE_VULKAN_GPU=0
export PICKLE_PERF_LOG=1
export PICKLE_KEYSTONE_CORNERS="$KEYSTONE_SETTINGS"

# Run pickle with keystone enabled and capture output
sudo PICKLE_USE_VULKAN_GPU=0 PICKLE_PERF_LOG=1 PICKLE_KEYSTONE_CORNERS="$KEYSTONE_SETTINGS" \
    ./pickle --gles "$VIDEO_FILE" > "$CPU_LOG" 2>&1

echo -e "${GREEN}CPU test completed.${NC}"

# Extract performance metrics
GPU_AVG_TIME=$(extract_avg_time "$GPU_LOG")
CPU_AVG_TIME=$(extract_avg_time "$CPU_LOG")

# Show results
echo -e "\n${CYAN}=== Results ===${NC}"
echo -e "GPU Average Keystone Correction Time: ${GREEN}$GPU_AVG_TIME ms${NC}"
echo -e "CPU Average Keystone Correction Time: ${YELLOW}$CPU_AVG_TIME ms${NC}"

# Calculate and show difference if requested
if [[ $SHOW_DIFF -eq 1 && "$GPU_AVG_TIME" != "N/A" && "$CPU_AVG_TIME" != "N/A" ]]; then
    # Calculate performance difference
    PERF_DIFF=$(echo "$CPU_AVG_TIME - $GPU_AVG_TIME" | bc)
    PERF_RATIO=$(echo "scale=2; $CPU_AVG_TIME / $GPU_AVG_TIME" | bc 2>/dev/null)
    
    echo -e "\n${CYAN}=== Performance Difference ===${NC}"
    if (( $(echo "$PERF_DIFF > 0" | bc -l) )); then
        echo -e "${GREEN}GPU is faster by $PERF_DIFF ms per frame${NC}"
        echo -e "${GREEN}GPU is approximately ${PERF_RATIO}x faster than CPU${NC}"
        
        # Determine if this is significant
        if (( $(echo "$PERF_RATIO >= 1.5" | bc -l) )); then
            echo -e "\n${GREEN}✓ Vulkan GPU acceleration is working correctly and providing significant speedup!${NC}"
        elif (( $(echo "$PERF_RATIO >= 1.1" | bc -l) )); then
            echo -e "\n${YELLOW}✓ Vulkan GPU acceleration is working, but the performance gain is modest.${NC}"
            echo -e "${YELLOW}   Try a more complex keystone preset (-k heavy) for better comparison.${NC}"
        else
            echo -e "\n${YELLOW}? Vulkan GPU acceleration appears to be working, but performance gain is minimal.${NC}"
            echo -e "${YELLOW}   This could be due to:${NC}"
            echo -e "${YELLOW}   - Simple keystone settings not taxing the CPU enough${NC}"
            echo -e "${YELLOW}   - GPU overhead for simple operations${NC}"
            echo -e "${YELLOW}   - Driver or hardware limitations${NC}"
            echo -e "${YELLOW}   Try a more complex keystone preset (-k heavy) for better comparison.${NC}"
        fi
    elif (( $(echo "$PERF_DIFF < 0" | bc -l) )); then
        echo -e "${RED}CPU is faster by $(echo "$PERF_DIFF * -1" | bc) ms per frame${NC}"
        echo -e "${RED}CPU is approximately $(echo "scale=2; $GPU_AVG_TIME / $CPU_AVG_TIME" | bc 2>/dev/null)x faster than GPU${NC}"
        
        echo -e "\n${RED}✗ Vulkan GPU acceleration may not be working correctly.${NC}"
        echo -e "${RED}   This could be due to:${NC}"
        echo -e "${RED}   - GPU driver issues${NC}"
        echo -e "${RED}   - Data transfer overhead between CPU and GPU${NC}"
        echo -e "${RED}   - Simple operations that don't benefit from parallelization${NC}"
        echo -e "${RED}   - Debug builds with extra validation enabled${NC}"
    else
        echo -e "${YELLOW}GPU and CPU performance are approximately equal.${NC}"
        
        echo -e "\n${YELLOW}? Vulkan GPU acceleration may be working, but not providing performance benefits.${NC}"
        echo -e "${YELLOW}   This could be due to:${NC}"
        echo -e "${YELLOW}   - Data transfer overhead canceling out processing gains${NC}"
        echo -e "${YELLOW}   - Simple keystone settings not complex enough${NC}"
        echo -e "${YELLOW}   Try a more complex keystone preset (-k heavy) for better comparison.${NC}"
    fi
else
    echo -e "\n${RED}Could not calculate performance difference.${NC}"
    echo -e "${RED}Check the logs for errors.${NC}"
fi

# Clean up
rm -rf "$TEMP_DIR"

echo -e "\n${CYAN}Test completed.${NC}"
exit 0