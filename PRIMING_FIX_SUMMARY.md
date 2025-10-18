# FFmpeg V4L2 Decoder Priming Fix

## Problem
The decoder was failing with hundreds of `[ERROR] [FFmpeg] non-existing PPS 0 referenced` errors, causing it to fall back to the software MPV decoder.

## Root Cause
The `prime_decoder()` function was:
1. Reading and sending packets directly to the codec context
2. Seeking back to the start position after priming
3. This caused the stream to restart **without** re-sending the SPS/PPS parameter sets that were only sent during priming

The BSF chain (`h264_mp4toannexb` → `filter_units` → `h264_metadata`) injects SPS/PPS from extradata **once** at the start. When priming sought back, the decoder received slice NALs without parameter sets.

## Solution
**Removed the `prime_decoder()` function entirely.**

The BSF chain already handles everything correctly:
- Converts avcC format to Annex-B format
- Injects SPS (NAL 7) and PPS (NAL 8) before the first keyframe
- Inserts AUD (NAL 9) delimiters
- Removes unnecessary access unit delimiters (type 6)

## Changes Made
1. Removed call to `prime_decoder()` in `init_ffmpeg_v4l2_player()`
2. Deleted the entire `prime_decoder()` function
3. Added comment explaining why priming was removed

## Results
**Before fix:**
```
[ERROR] [FFmpeg] non-existing PPS 0 referenced
[ERROR] [FFmpeg] non-existing PPS 0 referenced
... (repeated hundreds of times)
[WARN] V4L2 M2M decoder failed to produce first frame after 244 packets
[WARN] Stream is still not producing frames - falling back to software decoder
```

**After fix:**
```
[INFO] [AUD] First IDR observed after AUD insertion
[INFO] [BSF] NAL sequence (first 4): 9,7,8,5  (AUD, SPS, PPS, IDR slice)
[INFO] [BSF] NAL sequence (first 2): 9,1       (AUD, slice)
```

No PPS errors, decoder works correctly with the hardware accelerated V4L2 M2M path.

## Files Modified
- `ffmpeg_v4l2_player.c`: Removed priming function and call

## Date
October 18, 2025
