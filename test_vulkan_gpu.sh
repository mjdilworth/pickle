#!/bin/bash

# Script to test Vulkan GPU acceleration for keystone correction

# Usage information
function show_help {
    echo "Usage: $0 [OPTIONS]"
    echo "Tests Vulkan GPU acceleration for keystone correction"
    echo ""
    echo "OPTIONS:"
    echo "  -h, --help           Show this help message"
    echo "  -f, --file FILE      Video file to play"
    echo "  -c, --cpu            Force CPU-based keystone correction"
    echo "  -g, --gpu            Force GPU-based keystone correction (default)"
    echo "  -t, --test           Run performance comparison test"
    echo ""
}

# Default options
VIDEO_FILE="vid.mp4"
USE_GPU=1
RUN_TEST=0

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    key="$1"
    case $key in
        -h|--help)
            show_help
            exit 0
            ;;
        -f|--file)
            VIDEO_FILE="$2"
            shift
            shift
            ;;
        -c|--cpu)
            USE_GPU=0
            shift
            ;;
        -g|--gpu)
            USE_GPU=1
            shift
            ;;
        -t|--test)
            RUN_TEST=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Make sure the video file exists
if [ ! -f "$VIDEO_FILE" ]; then
    echo "Error: Video file '$VIDEO_FILE' not found."
    exit 1
fi

# Set environment variables based on options
export PICKLE_USE_VULKAN_GPU=$USE_GPU

if [ $RUN_TEST -eq 1 ]; then
    echo "===== RUNNING PERFORMANCE TEST ====="
    echo "First running with CPU-based keystone correction..."
    PICKLE_USE_VULKAN_GPU=0 ./pickle "$VIDEO_FILE" 2>&1 | grep -i "keystone performance" > cpu_perf.log &
    PICKLE_PID=$!
    
    # Let it run for 10 seconds
    sleep 10
    kill $PICKLE_PID 2>/dev/null
    
    echo "Now running with GPU-based keystone correction..."
    PICKLE_USE_VULKAN_GPU=1 ./pickle "$VIDEO_FILE" 2>&1 | grep -i "keystone performance" > gpu_perf.log &
    PICKLE_PID=$!
    
    # Let it run for 10 seconds
    sleep 10
    kill $PICKLE_PID 2>/dev/null
    
    echo "===== PERFORMANCE COMPARISON ====="
    echo "CPU-based keystone correction:"
    cat cpu_perf.log | tail -n 5
    echo ""
    echo "GPU-based keystone correction:"
    cat gpu_perf.log | tail -n 5
    echo "=================================="
else
    # Run pickle with the specified options
    echo "Running pickle with video: $VIDEO_FILE"
    if [ $USE_GPU -eq 1 ]; then
        echo "Using GPU-based keystone correction"
    else
        echo "Using CPU-based keystone correction"
    fi
    
    ./pickle "$VIDEO_FILE"
fi