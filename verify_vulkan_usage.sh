#!/bin/bash
# verify_vulkan_usage.sh - Script to verify Vulkan is being used for keystone correction
# 
# This script helps detect if hardware GPU acceleration is being used by:
# 1. Monitoring GPU usage during video playback
# 2. Comparing performance with and without Vulkan
# 3. Checking for Vulkan hardware capabilities
#

# ANSI color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Help message
show_help() {
    echo -e "${CYAN}Pickle Vulkan Usage Verification Tool${NC}"
    echo "This script helps verify if Vulkan hardware acceleration is being used for keystone correction."
    echo
    echo "Usage: $0 [options]"
    echo
    echo "Options:"
    echo "  -h, --help         Show this help message"
    echo "  -c, --check        Run Vulkan hardware check"
    echo "  -m, --monitor      Monitor GPU usage during playback"
    echo "  -p, --perf         Run performance comparison (GPU vs CPU)"
    echo "  -v, --verbose      Enable verbose output"
    echo "  -f, --file FILE    Video file to use for testing (default: vid.mp4)"
    echo
    echo "Examples:"
    echo "  $0 --check                 # Check Vulkan hardware capabilities"
    echo "  $0 --monitor --file myvid.mp4  # Monitor GPU usage with specific video"
    echo "  $0 --perf                  # Compare GPU vs CPU performance"
    echo "  $0 --check --monitor --perf  # Run all verification methods"
}

# Default values
VIDEO_FILE="vid.mp4"
CHECK_HW=0
MONITOR_GPU=0
RUN_PERF=0
VERBOSE=0

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -c|--check)
            CHECK_HW=1
            shift
            ;;
        -m|--monitor)
            MONITOR_GPU=1
            shift
            ;;
        -p|--perf)
            RUN_PERF=1
            shift
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -f|--file)
            VIDEO_FILE="$2"
            shift 2
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}"
            show_help
            exit 1
            ;;
    esac
done

# If no options specified, show help
if [[ $CHECK_HW -eq 0 && $MONITOR_GPU -eq 0 && $RUN_PERF -eq 0 ]]; then
    show_help
    exit 0
fi

# Function to print verbose messages
verbose() {
    if [[ $VERBOSE -eq 1 ]]; then
        echo -e "${BLUE}[INFO] $1${NC}"
    fi
}

# Function to print section headers
section() {
    echo -e "\n${CYAN}=== $1 ===${NC}"
}

# Check if we have the required tools
check_deps() {
    local missing=0
    
    # Check for make
    if ! command -v make &> /dev/null; then
        echo -e "${RED}ERROR: 'make' not found. Please install build-essential.${NC}"
        missing=1
    fi
    
    # Check for Vulkan development files
    if ! pkg-config --exists vulkan; then
        echo -e "${YELLOW}WARNING: Vulkan development files not found.${NC}"
        echo -e "${YELLOW}Install with: sudo apt install libvulkan-dev vulkan-tools${NC}"
    fi
    
    # Check for common monitoring tools
    if [[ $MONITOR_GPU -eq 1 ]]; then
        if ! command -v nvidia-smi &> /dev/null && ! command -v radeontop &> /dev/null && ! command -v intel_gpu_top &> /dev/null; then
            echo -e "${YELLOW}WARNING: No GPU monitoring tools found.${NC}"
            echo -e "${YELLOW}Consider installing one of these:${NC}"
            echo -e "${YELLOW}- NVIDIA: nvidia-smi (included with NVIDIA drivers)${NC}"
            echo -e "${YELLOW}- AMD: sudo apt install radeontop${NC}"
            echo -e "${YELLOW}- Intel: sudo apt install intel-gpu-tools${NC}"
            echo -e "${YELLOW}- Raspberry Pi: sudo apt install raspberrypi-tools${NC}"
        fi
    fi
    
    if [[ $missing -eq 1 ]]; then
        echo -e "${RED}Please install the missing dependencies and try again.${NC}"
        exit 1
    fi
}

# Function to check Vulkan hardware
check_vulkan_hardware() {
    section "Checking Vulkan Hardware Capabilities"
    
    verbose "Building Vulkan hardware check tool..."
    make check-vulkan
    
    if [[ $? -ne 0 ]]; then
        echo -e "${RED}Failed to build Vulkan hardware check tool.${NC}"
        return 1
    fi
    
    return 0
}

# Function to monitor GPU usage during playback
monitor_gpu_usage() {
    section "Monitoring GPU Usage During Playback"
    
    # Check if video file exists
    if [[ ! -f "$VIDEO_FILE" ]]; then
        echo -e "${RED}ERROR: Video file '$VIDEO_FILE' not found.${NC}"
        return 1
    fi
    
    echo -e "${YELLOW}Starting video playback with keystone enabled...${NC}"
    echo -e "${YELLOW}Please observe GPU activity during playback.${NC}"
    echo
    
    # Determine which GPU monitoring tool to use
    if command -v nvidia-smi &> /dev/null; then
        echo -e "${GREEN}Using NVIDIA GPU monitoring${NC}"
        # Start nvidia-smi in a background process
        nvidia-smi dmon -s u -d 1 &
        MONITOR_PID=$!
        
        # Enable Vulkan explicitly
        export PICKLE_USE_VULKAN_GPU=1
        
        # Run pickle with Vulkan GPU acceleration (background)
        verbose "Running pickle with Vulkan GPU acceleration..."
        sudo PICKLE_USE_VULKAN_GPU=1 ./pickle --vulkan "$VIDEO_FILE"
        
        # Stop the monitor
        kill $MONITOR_PID
        
    elif command -v radeontop &> /dev/null; then
        echo -e "${GREEN}Using AMD GPU monitoring${NC}"
        # Start radeontop in a background process
        radeontop -d - &
        MONITOR_PID=$!
        
        # Enable Vulkan explicitly
        export PICKLE_USE_VULKAN_GPU=1
        
        # Run pickle with Vulkan GPU acceleration (background)
        verbose "Running pickle with Vulkan GPU acceleration..."
        sudo PICKLE_USE_VULKAN_GPU=1 ./pickle --vulkan "$VIDEO_FILE"
        
        # Stop the monitor
        kill $MONITOR_PID
        
    elif command -v intel_gpu_top &> /dev/null; then
        echo -e "${GREEN}Using Intel GPU monitoring${NC}"
        
        # Start intel_gpu_top in a background terminal
        xterm -e "intel_gpu_top -s 100" &
        MONITOR_PID=$!
        
        # Enable Vulkan explicitly
        export PICKLE_USE_VULKAN_GPU=1
        
        # Run pickle with Vulkan GPU acceleration
        verbose "Running pickle with Vulkan GPU acceleration..."
        sudo PICKLE_USE_VULKAN_GPU=1 ./pickle --vulkan "$VIDEO_FILE"
        
        # Stop the monitor
        kill $MONITOR_PID
        
    else
        echo -e "${YELLOW}No GPU monitoring tool found. Running without monitoring.${NC}"
        
        # Enable Vulkan explicitly
        export PICKLE_USE_VULKAN_GPU=1
        
        # Run pickle with Vulkan GPU acceleration
        verbose "Running pickle with Vulkan GPU acceleration..."
        sudo PICKLE_USE_VULKAN_GPU=1 ./pickle --vulkan "$VIDEO_FILE"
    fi
    
    echo
    echo -e "${GREEN}Playback completed.${NC}"
    echo -e "${YELLOW}Check for GPU activity spikes during keystone correction.${NC}"
    
    return 0
}

# Function to run performance comparison
run_performance_comparison() {
    section "Performance Comparison (GPU vs CPU)"
    
    # Check if video file exists
    if [[ ! -f "$VIDEO_FILE" ]]; then
        echo -e "${RED}ERROR: Video file '$VIDEO_FILE' not found.${NC}"
        return 1
    fi
    
    echo -e "${YELLOW}Running performance comparison between GPU and CPU...${NC}"
    echo
    
    # First run with GPU acceleration
    echo -e "${CYAN}Test 1: With Vulkan GPU Acceleration${NC}"
    export PICKLE_USE_VULKAN_GPU=1
    verbose "Running pickle with Vulkan GPU acceleration and performance logging..."
    sudo PICKLE_USE_VULKAN_GPU=1 PICKLE_PERF_LOG=1 ./pickle --vulkan "$VIDEO_FILE"
    
    # Second run without GPU acceleration
    echo -e "\n${CYAN}Test 2: Without Vulkan GPU Acceleration (CPU only)${NC}"
    export PICKLE_USE_VULKAN_GPU=0
    verbose "Running pickle with CPU-only processing and performance logging..."
    sudo PICKLE_USE_VULKAN_GPU=0 PICKLE_PERF_LOG=1 ./pickle --gles "$VIDEO_FILE"
    
    echo
    echo -e "${GREEN}Performance comparison completed.${NC}"
    echo -e "${YELLOW}Check for significant performance differences between GPU and CPU modes.${NC}"
    echo -e "${YELLOW}If GPU acceleration is working, the GPU mode should be faster.${NC}"
    
    return 0
}

# Main execution
check_deps

# Check Vulkan hardware if requested
if [[ $CHECK_HW -eq 1 ]]; then
    check_vulkan_hardware
fi

# Monitor GPU usage if requested
if [[ $MONITOR_GPU -eq 1 ]]; then
    monitor_gpu_usage
fi

# Run performance comparison if requested
if [[ $RUN_PERF -eq 1 ]]; then
    run_performance_comparison
fi

echo -e "\n${GREEN}All requested verification steps completed.${NC}"
exit 0