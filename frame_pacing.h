#ifndef PICKLE_FRAME_PACING_H
#define PICKLE_FRAME_PACING_H

#include <stdbool.h>
#include <time.h>
#include "error.h"

// Frame pacing context
typedef struct {
    // Display refresh information
    double refresh_rate;           // Refresh rate in Hz
    double frame_duration_ns;      // Duration of each frame in nanoseconds
    
    // Frame timing
    struct timespec last_frame_time;   // Time of last presented frame
    struct timespec next_frame_time;   // Target time for next frame
    
    // Statistics
    unsigned long frames_rendered;
    unsigned long frames_skipped;
    double avg_frame_time;
    double max_frame_time;
    double min_frame_time;
    
    // Frame pacing configuration
    bool enabled;                  // Whether frame pacing is enabled
    bool adaptive_vsync;           // Use adaptive vsync if available
    int max_frame_lateness_ms;     // Maximum milliseconds a frame can be late
    
    // Tracking
    bool first_frame;              // Is this the first frame?
} frame_pacing_context_t;

// Initialize frame pacing
pickle_result_t frame_pacing_init(frame_pacing_context_t *ctx, double refresh_rate);

// Calculate next frame time
void frame_pacing_next_frame(frame_pacing_context_t *ctx);

// Wait until it's time to present the next frame
// Returns: true if frame should be rendered, false if it should be skipped
bool frame_pacing_wait_next_frame(frame_pacing_context_t *ctx);

// Notify that a frame has been presented
void frame_pacing_frame_presented(frame_pacing_context_t *ctx);

// Get frame pacing statistics as formatted string
void frame_pacing_get_stats(frame_pacing_context_t *ctx, char *buffer, size_t buffer_size);

// Reset frame pacing statistics
void frame_pacing_reset_stats(frame_pacing_context_t *ctx);

#endif // PICKLE_FRAME_PACING_H