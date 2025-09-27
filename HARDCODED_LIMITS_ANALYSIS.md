# Hardcoded Limits Analysis Report

## Critical FPS/Timing Limits (FIXED ✅)
1. **V4L2 timestamp increment**: `p->timestamp += 40000` (25fps) → Made adaptive ✅
2. **V4L2 frame update timing**: `elapsed > 40.0` (25fps) → Made adaptive ✅  
3. **V4L2 timer interval**: `40ms` (25fps) → Made adaptive ✅

## Polling/Timeout Limits
1. **Main loop timeout**: `timeout_ms = 16` (60Hz default) - Line 3015
   - Caps at 4ms min (250fps max), 100ms max (10fps min)
2. **V4L2 decoder poll**: `poll(fds, 1, 100)` - 100ms timeout
3. **Page flip timeout**: `{0, 100000}` - 100ms timeout for DRM events
4. **MPV FPS query**: Every 5 seconds - reasonable for overhead
5. **CPU stats update**: Every 0.5 seconds - reasonable for overhead

## Buffer Size Limits
1. **V4L2 buffer allocation**: `8, 8` buffers (input/output) - Line 1233
2. **V4L2 read buffer**: `1MB` (1024*1024) - Line 1253  
3. **V4L2 chunk size**: `64KB` (65536) - Line 1441
4. **MPV audio buffer**: `0.2` (200ms) - Line 996
5. **MPV queue size**: `"4"` (vsync) or `"1"` (no-vsync) - Line 965
6. **Event epoll**: `16` max events - Line 222 (event.c)
7. **FB ring size**: Varies by implementation
8. **Memory thresholds**: 
   - RSS > 500MB warning
   - Available < 100MB warning

## Sleep/Delay Limits  
1. **V4L2 frame rate limiting**: `sleep_ms < 100` cap - Line 1345
2. **Minimum sleep**: `5ms` minimum between frames - Line 1361
3. **Aggressive polling**: `usleep(1000)` - 1ms backoff - Line 3196
4. **Frame timing nanosleep**: Adaptive based on FPS ✅

## Performance Caps
1. **Timeout bounds**: 4ms-100ms (250fps-10fps range)
2. **Poll timeout**: 100ms max to prevent infinite blocking
3. **Memory warnings**: 500MB RSS, 100MB available thresholds

## Recommendations

### High Priority Fixes:
1. **V4L2 decoder poll timeout**: Change `100ms` to adaptive based on FPS
2. **Main loop timeout**: Make more aggressive for high-FPS content
3. **Page flip timeout**: Reduce from 100ms for better responsiveness

### Medium Priority:
1. **Buffer counts**: Make V4L2 buffer count configurable (currently 8/8)
2. **Chunk size**: Allow larger V4L2 chunks for high-bitrate content
3. **MPV queue size**: Consider dynamic adjustment based on performance

### Low Priority:
1. **Memory thresholds**: Make configurable via environment variables  
2. **Stats update intervals**: Allow user configuration

## Environment Variable Opportunities:
- `PICKLE_V4L2_BUFFERS=16` (default 8)
- `PICKLE_POLL_TIMEOUT_MS=50` (default 100)  
- `PICKLE_CHUNK_SIZE_KB=128` (default 64)
- `PICKLE_MPV_QUEUE_SIZE=2` (override auto-sizing)