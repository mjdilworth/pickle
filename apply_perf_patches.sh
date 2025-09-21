#!/bin/bash
# apply_perf_patches.sh - Apply performance optimizations to pickle code

set -e

echo "Applying performance optimizations to pickle..."

# 1. Add performance includes to the top of pickle.c
INCLUDE_LINE=$(grep -n "signal.h" pickle.c | head -1 | cut -d':' -f1)
INCLUDE_LINE=$((INCLUDE_LINE + 1))
sed -i "${INCLUDE_LINE}r perf_includes.h" pickle.c
echo "✓ Added performance includes"

# 2. Add performance options in main() function
OPTION_LINE=$(grep -n "const char \*loop_env = getenv" pickle.c | head -1 | cut -d':' -f1)
OPTION_LINE=$((OPTION_LINE + 5))
sed -i "${OPTION_LINE}r perf_options.h" pickle.c
echo "✓ Added performance options"

# 3. Add timing start in render_frame_fixed function
RENDER_LINE=$(grep -n "static bool render_frame_fixed" pickle.c | head -1 | cut -d':' -f1)
RENDER_LINE=$((RENDER_LINE + 1))
sed -i "${RENDER_LINE}r perf_timing_start.h" pickle.c
echo "✓ Added performance timing start"

# 4. Add keystone disable check in render_frame_fixed
KEYSTONE_LINE=$(grep -n "Early exit optimization: Skip keystone" pickle.c | head -1 | cut -d':' -f1)
KEYSTONE_LINE=$((KEYSTONE_LINE - 1))
sed -i "${KEYSTONE_LINE}r perf_disable_keystone.h" pickle.c
echo "✓ Added keystone disable check"

# 5. Add timing end in render_frame_fixed
SWAP_LINE=$(grep -n "eglSwapBuffers" pickle.c | head -1 | cut -d':' -f1)
SWAP_LINE=$((SWAP_LINE + 1))
sed -i "${SWAP_LINE}r perf_timing_end.h" pickle.c
echo "✓ Added performance timing end"

echo "Performance optimizations applied successfully!"
echo "Now you can compile with: make RELEASE=1 MAXPERF=1 RPI4_OPT=1"