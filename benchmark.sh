#!/bin/bash
# benchmark.sh - Simple benchmark script for pickle performance testing

set -e

show_help() {
    echo "Usage: $0 [OPTIONS] VIDEO_FILE"
    echo "Run performance benchmarks on pickle video player"
    echo ""
    echo "Options:"
    echo "  -h, --help           Show this help message"
    echo "  -d, --duration SEC   Run for specified seconds (default: 30)"
    echo "  -t, --tests N        Number of test iterations (default: 3)"
    echo "  -k, --keystone       Test with keystone enabled (default: disabled)"
    echo "  -a, --affinity       Test with CPU affinity (cores 2,3)"
    echo "  -p, --priority       Test with real-time priority"
    echo "  -o, --output FILE    Save results to file"
    echo ""
    echo "Examples:"
    echo "  $0 video.mp4                Run default benchmark"
    echo "  $0 -d 10 -t 5 video.mp4     Run 5 tests of 10 seconds each"
    echo "  $0 -k -a -p video.mp4       Test with all optimizations"
}

# Default options
DURATION=30
TESTS=3
KEYSTONE=0
AFFINITY=0
PRIORITY=0
OUTPUT=""
VIDEO=""

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -d|--duration)
            DURATION="$2"
            shift 2
            ;;
        -t|--tests)
            TESTS="$2"
            shift 2
            ;;
        -k|--keystone)
            KEYSTONE=1
            shift
            ;;
        -a|--affinity)
            AFFINITY=1
            shift
            ;;
        -p|--priority)
            PRIORITY=1
            shift
            ;;
        -o|--output)
            OUTPUT="$2"
            shift 2
            ;;
        *)
            if [ -z "$VIDEO" ]; then
                VIDEO="$1"
                shift
            else
                echo "Unknown option: $1"
                show_help
                exit 1
            fi
            ;;
    esac
done

# Check if video file is specified
if [ -z "$VIDEO" ]; then
    echo "Error: No video file specified"
    show_help
    exit 1
fi

# Check if video file exists
if [ ! -f "$VIDEO" ]; then
    echo "Error: Video file '$VIDEO' not found"
    exit 1
fi

# Check for required commands
for cmd in timeout top grep; do
    if ! command -v $cmd &> /dev/null; then
        echo "Error: Required command '$cmd' not found"
        exit 1
    fi
done

# Create temp directory for results
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

# Prepare results header
RESULTS_FILE="$TEMP_DIR/results.txt"
echo "Pickle Performance Benchmark" > $RESULTS_FILE
echo "=========================" >> $RESULTS_FILE
echo "Video: $VIDEO" >> $RESULTS_FILE
echo "Date: $(date)" >> $RESULTS_FILE
echo "System: $(uname -a)" >> $RESULTS_FILE
echo "CPU: $(grep 'model name' /proc/cpuinfo | head -1 | cut -d ':' -f 2 || echo 'Unknown')" >> $RESULTS_FILE
echo "RAM: $(free -h | grep Mem | awk '{print $2}')" >> $RESULTS_FILE
echo "=========================" >> $RESULTS_FILE
echo "" >> $RESULTS_FILE

# Add top monitoring function
monitor_process() {
    local pid=$1
    local output_file=$2
    local duration=$3
    
    # Sample CPU usage every second
    for ((i=1; i<=$duration; i++)); do
        top -b -n 1 -p $pid | grep $pid >> "$output_file" || true
        sleep 1
    done
}

run_test() {
    local test_name="$1"
    local env_vars="$2"
    local cmd_prefix="$3"
    
    echo "Running test: $test_name"
    echo "Test: $test_name" >> $RESULTS_FILE
    echo "Command: $cmd_prefix $env_vars ./pickle $VIDEO" >> $RESULTS_FILE
    
    # Run tests
    for ((i=1; i<=$TESTS; i++)); do
        echo "  Iteration $i/$TESTS..."
        
        # Create output files for this test
        OUT_FILE="$TEMP_DIR/test_${i}.txt"
        CPU_FILE="$TEMP_DIR/cpu_${i}.txt"
        
        # Start pickle in background
        if [ -n "$cmd_prefix" ]; then
            eval "$cmd_prefix $env_vars ./pickle $VIDEO > $OUT_FILE 2>&1 &"
        else
            eval "$env_vars ./pickle $VIDEO > $OUT_FILE 2>&1 &"
        fi
        PICKLE_PID=$!
        
        # Monitor CPU usage
        monitor_process $PICKLE_PID $CPU_FILE $DURATION &
        MONITOR_PID=$!
        
        # Wait for monitoring to complete
        wait $MONITOR_PID
        
        # Kill pickle if still running
        kill $PICKLE_PID 2>/dev/null || true
        wait $PICKLE_PID 2>/dev/null || true
        
        # Calculate average CPU usage
        if [ -s "$CPU_FILE" ]; then
            CPU_SUM=0
            CPU_COUNT=0
            while read line; do
                CPU_USAGE=$(echo $line | awk '{print $9}')
                CPU_SUM=$(echo "$CPU_SUM + $CPU_USAGE" | bc)
                CPU_COUNT=$((CPU_COUNT + 1))
            done < "$CPU_FILE"
            
            if [ $CPU_COUNT -gt 0 ]; then
                AVG_CPU=$(echo "scale=1; $CPU_SUM / $CPU_COUNT" | bc)
                echo "  Average CPU: ${AVG_CPU}%"
                echo "Average CPU: ${AVG_CPU}%" >> $RESULTS_FILE
            fi
        fi
        
        # Count frames rendered (if log contains this info)
        FRAMES=$(grep -c "rendered frame" $OUT_FILE || echo "N/A")
        echo "  Frames rendered: $FRAMES"
        echo "Frames rendered: $FRAMES" >> $RESULTS_FILE
        
        # Calculate approximate FPS
        if [ "$FRAMES" != "N/A" ] && [ $FRAMES -gt 0 ]; then
            FPS=$(echo "scale=1; $FRAMES / $DURATION" | bc)
            echo "  Approximate FPS: $FPS"
            echo "Approximate FPS: $FPS" >> $RESULTS_FILE
        fi
        
        # Add separator between iterations
        echo "" >> $RESULTS_FILE
    done
    
    echo "" >> $RESULTS_FILE
    echo "------------------------" >> $RESULTS_FILE
    echo "" >> $RESULTS_FILE
}

# Run baseline test
run_test "Baseline" "PICKLE_DEBUG=1" ""

# Run with frame skip optimization
run_test "Frame Skip" "PICKLE_DEBUG=1 PICKLE_SKIP_UNCHANGED=1" ""

# Run with direct rendering
run_test "Direct Rendering" "PICKLE_DEBUG=1 PICKLE_DIRECT_RENDERING=1" ""

# Run with disabled keystone
run_test "Disabled Keystone" "PICKLE_DEBUG=1 PICKLE_DISABLE_KEYSTONE=1" ""

# Run with all optimizations
run_test "All Optimizations" "PICKLE_DEBUG=1 PICKLE_SKIP_UNCHANGED=1 PICKLE_DIRECT_RENDERING=1 PICKLE_DISABLE_KEYSTONE=1" ""

# Run with CPU affinity if requested
if [ $AFFINITY -eq 1 ]; then
    run_test "CPU Affinity" "PICKLE_DEBUG=1 PICKLE_SKIP_UNCHANGED=1 PICKLE_DIRECT_RENDERING=1 PICKLE_DISABLE_KEYSTONE=1 PICKLE_CPU_AFFINITY=2,3" ""
fi

# Run with priority if requested
if [ $PRIORITY -eq 1 ]; then
    run_test "Real-time Priority" "PICKLE_DEBUG=1 PICKLE_SKIP_UNCHANGED=1 PICKLE_DIRECT_RENDERING=1 PICKLE_DISABLE_KEYSTONE=1 PICKLE_PRIORITY=10" "sudo"
fi

# Run with keystone if requested
if [ $KEYSTONE -eq 1 ]; then
    run_test "With Keystone" "PICKLE_DEBUG=1 PICKLE_SKIP_UNCHANGED=1 PICKLE_DIRECT_RENDERING=1 PICKLE_KEYSTONE=1" ""
fi

# Display summary
echo ""
echo "Benchmark complete! Summary:"
cat $RESULTS_FILE

# Save output if requested
if [ -n "$OUTPUT" ]; then
    cp $RESULTS_FILE "$OUTPUT"
    echo "Results saved to $OUTPUT"
fi