#!/bin/bash

# Reset keystone config first
rm -f keystone.conf

# Start pickle with keystone test
(
    # Enable keystone with 'k'
    echo "k"
    sleep 0.2
    
    # Select corner 1 (top-left)
    echo "1"
    sleep 0.2
    
    # Send up arrow (ESC sequence)
    printf "\033[A"
    sleep 0.1
    printf "\033[A"
    sleep 0.1
    printf "\033[A"
    sleep 0.2
    
    # Select corner 4 (bottom-right)
    echo "4"
    sleep 0.2
    
    # Send down arrow  
    printf "\033[B"
    sleep 0.1
    printf "\033[B"
    sleep 0.2
    
    # Save config
    echo "S"
    sleep 0.5
    
    # Quit
    echo "q"
) | timeout 5s ./pickle -p -l rpi4-e.mp4 2>&1 | grep -E "Selected corner|Adjusted corner|saved|configuration"
