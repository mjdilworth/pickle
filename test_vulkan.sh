#!/bin/bash
# Test script for Vulkan implementation in Pickle

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}Pickle Vulkan Testing Script${NC}"
echo "This script will test Vulkan support in Pickle"
echo

# Check if Vulkan is supported
echo -e "${YELLOW}Checking Vulkan support...${NC}"
if command -v vulkaninfo > /dev/null 2>&1; then
    echo -e "${GREEN}vulkaninfo found - Vulkan SDK is installed${NC}"
    VULKAN_SUPPORTED=1
else
    echo -e "${RED}vulkaninfo not found - Vulkan SDK is not installed${NC}"
    echo "Please install Vulkan SDK to test Vulkan support"
    VULKAN_SUPPORTED=0
fi

# Check if test video exists
TEST_VIDEO="vid.mp4"
if [ -f "$TEST_VIDEO" ]; then
    echo -e "${GREEN}Test video found: ${TEST_VIDEO}${NC}"
else
    echo -e "${RED}Test video not found: ${TEST_VIDEO}${NC}"
    echo "Please provide a test video file"
    exit 1
fi

# Build pickle with Vulkan support
echo -e "${YELLOW}Building pickle with Vulkan support...${NC}"
make clean
make VULKAN=1

if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed${NC}"
    exit 1
else
    echo -e "${GREEN}Build successful${NC}"
fi

# Test OpenGL ES rendering (baseline)
echo -e "${YELLOW}Testing OpenGL ES rendering...${NC}"
echo "Running: ./pickle --gles $TEST_VIDEO"
if [ -t 1 ]; then
    # Only run for a few seconds if on an interactive terminal
    timeout 5s ./pickle --gles $TEST_VIDEO || true
else
    ./pickle --gles $TEST_VIDEO &
    sleep 5
    kill $! 2>/dev/null || true
fi

echo
echo -e "${YELLOW}Press Enter to continue to Vulkan testing...${NC}"
read

# Test Vulkan rendering if supported
if [ $VULKAN_SUPPORTED -eq 1 ]; then
    echo -e "${YELLOW}Testing Vulkan rendering...${NC}"
    echo "Running: ./pickle --vulkan $TEST_VIDEO"
    if [ -t 1 ]; then
        # Only run for a few seconds if on an interactive terminal
        timeout 5s ./pickle --vulkan $TEST_VIDEO || true
    else
        ./pickle --vulkan $TEST_VIDEO &
        sleep 5
        kill $! 2>/dev/null || true
    fi
fi

echo
echo -e "${YELLOW}Testing complete${NC}"
echo "Please verify that both rendering backends displayed the video correctly."