# FFmpeg V4L2 M2M Decoder Threshold Fix

## Date
2025-10-17

## Problem
The `test_video.mp4` file was falling back to MPV unnecessarily even though the FFmpeg V4L2 M2M hardware decoder could successfully decode it. The video would start decoding successfully but then trigger a premature fallback to MPV before completing.

## Root Cause
The `MAX_V4L2_FAILURES` threshold was set too low at 100 consecutive failures. The V4L2 M2M decoder (BCM2835 codec on Raspberry Pi 4) has normal buffering behavior where it returns EAGAIN multiple times while processing frames, especially:
- During initial stream buffering
- At stream boundaries (near EOF)
- During seek operations in loop mode
- When transitioning between keyframes

With a 100-failure threshold and ~1-5ms per iteration, the decoder would only get ~0.5 seconds to recover before triggering fallback, which wasn't enough time for normal V4L2 buffering behavior.

## Solution
Increased `MAX_V4L2_FAILURES` from 100 to 500 consecutive failures, providing:
- ~2.5 seconds maximum wait time (500 failures × ~5ms)
- Sufficient time for V4L2 decoder buffering to complete
- Still provides safety fallback if decoder genuinely stalls
- Maintains robust error handling for incompatible streams

## Changes Made

### File: `pickle.c`
```c
// Before:
static const int MAX_V4L2_FAILURES = 100;

// After:
static const int MAX_V4L2_FAILURES = 500; // Max failures before triggering fallback
```

Added detailed comment explaining the rationale for the threshold value.

## Test Results

### test_video.mp4 (Working Case)
- **Before Fix**: Fell back to MPV after ~20-50 frames
- **After Fix**: Completes successfully with V4L2 decoder
  ```
  [INFO]  EOF reached in ffmpeg_v4l2_player - 297 frames decoded
  [MAIN] EOF reached, exiting normally
  [INFO] End of video reached
  ```
- **Result**: ✅ Plays 297/300 frames with hardware decoder, exits cleanly at EOF

### test_video.mp4 with Loop Mode (-l flag)
- **Before Fix**: Would fall back to MPV on first loop iteration
- **After Fix**: Successfully loops continuously
  ```
  [INFO]  EOF reached in ffmpeg_v4l2_player - 294 frames decoded
  [MAIN] EOF reached in loop mode, resetting...
  ```
- **Result**: ✅ Properly detects EOF and resets decoder for looping

### rpi4-e.mp4 (Incompatible Stream)
- **Before Fix**: Would try for 100 iterations then fallback
- **After Fix**: Still properly falls back due to early failure detection
  ```
  [WARN]  Attempting to flush V4L2 decoder before fallback
  [WARN] FFmpeg V4L2 failed (early failure), attempting fallback to MPV
  ```
- **Result**: ✅ Early failure detection (20 packets without frame) still works

## Additional Fixes Included

### EOF Error Handling
Modified error handling in `ffmpeg_v4l2_player.c` to not treat AVERROR_EOF as a fatal error:
```c
// Before:
if (ret < 0 && ret != AVERROR(EAGAIN)) {
    player->fatal_error = true;
}

// After:
if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
    player->fatal_error = true;
}
```

### EOF Detection in Main Loop
Reordered EOF detection in `pickle.c` to check for EOF **before** incrementing failure counter:
```c
// Check for EOF first (before incrementing failure counter)
if (ffmpeg_v4l2_player.eof_reached && g_loop_playback) {
    // Reset for loop playback
    ffmpeg_v4l2_reset(&ffmpeg_v4l2_player);
    g_v4l2_consecutive_failures = 0;
    render_success = true;
} else if (ffmpeg_v4l2_player.eof_reached) {
    // End of video in non-loop mode - exit cleanly
    LOG_INFO("End of video reached");
    g_stop = 1;
    break;
} else {
    // Not EOF - this is a real failure, increment counter
    g_v4l2_consecutive_failures++;
    // ...
}
```

This ensures that normal EOF conditions don't trigger the fallback mechanism.

## Performance Impact
- **Minimal**: The increased threshold only affects the failure case
- **No impact on normal playback**: Successfully decoded frames reset the counter to 0
- **Slightly longer stall detection**: ~2 seconds instead of ~0.5 seconds before fallback
- **Trade-off**: Better compatibility with V4L2 buffering vs. slightly slower failure detection

## Backward Compatibility
- ✅ All existing functionality preserved
- ✅ Fallback mechanism still works for genuinely broken streams
- ✅ Early failure detection (20 packets) unchanged
- ✅ MPV fallback path unchanged

## Future Improvements
Consider making `MAX_V4L2_FAILURES` configurable via environment variable:
```c
const char *threshold_env = getenv("PICKLE_V4L2_FAILURE_THRESHOLD");
int threshold = threshold_env ? atoi(threshold_env) : 500;
```

This would allow fine-tuning for different hardware or use cases without recompiling.

## Verification Commands
```bash
# Test normal playback (should complete without MPV fallback)
./pickle test_video.mp4

# Test loop mode (should loop continuously)
./pickle -l test_video.mp4

# Test incompatible file (should fall back to MPV)
./pickle rpi4-e.mp4

# Test with debug output
PICKLE_DEBUG=1 ./pickle test_video.mp4
```

## Related Issues
- Fixes issue where test_video.mp4 unnecessarily falls back to MPV
- Maintains robust fallback for BCM2835 V4L2 decoder incompatibilities
- Improves loop mode reliability
