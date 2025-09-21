#!/bin/bash

# This script updates the v4l2_decoder.c file to use the correct log format

# Backup the original file
cp /home/dilly/Projects/pickle/v4l2_decoder.c /home/dilly/Projects/pickle/v4l2_decoder.c.bak

# Replace LOG_ERROR
sed -i 's/LOG_ERROR("V4L2", \(.*\))/LOG_ERROR(\1)/g' /home/dilly/Projects/pickle/v4l2_decoder.c

# Replace LOG_WARN
sed -i 's/LOG_WARN("V4L2", \(.*\))/LOG_WARN(\1)/g' /home/dilly/Projects/pickle/v4l2_decoder.c

# Replace LOG_INFO
sed -i 's/LOG_INFO("V4L2", \(.*\))/LOG_INFO(\1)/g' /home/dilly/Projects/pickle/v4l2_decoder.c

# Replace LOG_DEBUG
sed -i 's/LOG_DEBUG("V4L2", \(.*\))/LOG_DEBUG(\1)/g' /home/dilly/Projects/pickle/v4l2_decoder.c

echo "Log format update completed!"