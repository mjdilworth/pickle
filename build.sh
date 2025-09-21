#!/bin/bash
# build.sh - Helper script to build pickle with different optimization levels

set -e

show_help() {
    echo "Usage: $0 [OPTIONS]"
    echo "Build the pickle video player with different optimization levels"
    echo ""
    echo "Options:"
    echo "  -h, --help           Show this help message"
    echo "  -d, --debug          Build with debug symbols (default)"
    echo "  -r, --release        Build optimized release version"
    echo "  -m, --max-perf       Build with maximum performance optimizations"
    echo "  -p, --rpi4           Include Raspberry Pi 4 specific optimizations"
    echo "  -c, --clean          Clean before building"
    echo "  -i, --install        Install after building (requires sudo)"
    echo ""
    echo "Examples:"
    echo "  $0                   Build default debug version"
    echo "  $0 -r                Build release version"
    echo "  $0 -m -p             Build max performance version with RPi4 optimizations"
    echo "  $0 -r -c             Clean and build release version"
}

# Default options
DEBUG=1
RELEASE=0
MAXPERF=0
RPI4_OPT=0
CLEAN=0
INSTALL=0

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -d|--debug)
            DEBUG=1
            RELEASE=0
            MAXPERF=0
            shift
            ;;
        -r|--release)
            DEBUG=0
            RELEASE=1
            MAXPERF=0
            shift
            ;;
        -m|--max-perf)
            DEBUG=0
            RELEASE=1
            MAXPERF=1
            shift
            ;;
        -p|--rpi4)
            RPI4_OPT=1
            shift
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -i|--install)
            INSTALL=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Clean if requested
if [ $CLEAN -eq 1 ]; then
    echo "Cleaning build files..."
    make clean
fi

# Build command
BUILD_CMD="make"
if [ $DEBUG -eq 1 ]; then
    BUILD_CMD="$BUILD_CMD DEBUG=1"
fi
if [ $RELEASE -eq 1 ]; then
    BUILD_CMD="$BUILD_CMD RELEASE=1"
fi
if [ $MAXPERF -eq 1 ]; then
    BUILD_CMD="$BUILD_CMD MAXPERF=1"
fi
if [ $RPI4_OPT -eq 1 ]; then
    BUILD_CMD="$BUILD_CMD RPI4_OPT=1"
fi

# Display build type
if [ $MAXPERF -eq 1 ]; then
    echo "Building maximum performance version..."
elif [ $RELEASE -eq 1 ]; then
    echo "Building release version..."
else
    echo "Building debug version..."
fi

# Execute build
echo "Executing: $BUILD_CMD"
eval $BUILD_CMD

# Install if requested
if [ $INSTALL -eq 1 ]; then
    echo "Installing pickle..."
    sudo make install
fi

echo "Build complete!"
ls -lh pickle

# Check file size
echo "File details:"
file pickle

# Check optimization level in binary
echo "Checking optimization flags in binary:"
readelf -p .comment pickle | grep GCC || echo "No GCC version info found"

echo "Done!"