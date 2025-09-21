# Testing Instructions for Event-Driven Architecture

## Build and Integration

1. Make sure you've added all the new files to the build:
   - `event.h` and `event.c`
   - `event_callbacks.h` and `event_callbacks.c`
   - `pickle_events.h` and `pickle_events.c`

2. Update the `Makefile` to include these files in the `SOURCES` and `HEADERS` variables.

3. Follow the instructions in `event_integration_instructions.c` to integrate the event-driven architecture into `pickle.c`.

4. Build the project:
   ```
   make clean
   make
   ```

## Testing Scenarios

Test the following scenarios to ensure the event-driven architecture works correctly:

1. **Basic Playback**
   ```
   ./pickle vid.mp4
   ```
   - Video should play normally
   - Keystone controls should work
   - Help overlay should display when 'h' is pressed
   - 'q' should quit the application

2. **Keystone Mode**
   ```
   PICKLE_KEYSTONE=1 ./pickle vid.mp4
   ```
   - Keystone mode should be enabled
   - Corner adjustment with 1-4 keys and arrow keys/WASD should work
   - Border toggle should work with 'b'
   - Corner marker toggle should work with 'c'

3. **Joystick Control** (if available)
   - Joystick controls should work for keystone adjustment
   - START+SELECT held for 2 seconds should quit

4. **V4L2 Decoder** (if enabled)
   ```
   PICKLE_USE_V4L2=1 ./pickle vid.mp4
   ```
   - V4L2 decoder should work
   - Frame updates should happen at regular intervals

5. **Signal Handling**
   - Press Ctrl+C to send SIGINT
   - Application should clean up and exit gracefully

## Performance Measurement

Compare the performance of the event-driven architecture with the previous poll-based implementation:

```
PICKLE_STATS=1 PICKLE_TIMING=1 ./pickle vid.mp4
```

Look for improvements in:
- CPU usage
- Frame timing consistency
- Response to input events

## Debugging

If you encounter issues:

1. Enable debug logging:
   ```
   PICKLE_DEBUG=1 ./pickle vid.mp4
   ```

2. Check for any event-related errors in the log output.

3. Verify that all event sources are registered correctly by examining the "[EVENT]" log messages.

4. If a specific event type isn't working, check the corresponding callback function in `event_callbacks.c`.

## Known Limitations

- The timer-based V4L2 frame updates are currently fixed at 25fps (40ms intervals). This could be made adaptive based on video framerate.
- Signal handling is currently limited to SIGINT, SIGTERM, and SIGUSR1. Additional signals may need to be handled in future versions.