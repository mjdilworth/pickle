#ifndef FRAME_PACING_OPTIMIZE_H
#define FRAME_PACING_OPTIMIZE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

// Frame pacing optimization for reducing overruns
typedef struct {
    double target_fps;           // Target FPS based on renderer capability
    double measured_render_fps;  // Measured rendering FPS
    double adaptation_factor;    // How aggressively to adapt (0.0-1.0)
    uint64_t frames_rendered;    // Total frames rendered
    uint64_t frames_dropped;     // Total frames dropped
    bool adaptive_enabled;       // Whether adaptive pacing is enabled
    struct timeval last_adaptation; // Last time we adapted
} frame_pacing_optimizer_t;

// Global frame pacing optimizer
extern frame_pacing_optimizer_t g_frame_pacer;

// Initialize decoder pacing optimizer
void decoder_pacing_init(double initial_target_fps);

// Update decoder pacing based on current performance
void decoder_pacing_update(double current_fps, uint64_t dropped_frames);

// Get recommended decoder queue size based on performance
int decoder_pacing_get_queue_size(void);

// Check if we should throttle decoder
bool decoder_pacing_should_throttle(void);

// Get adaptive timeout for main loop
int decoder_pacing_get_timeout_ms(void);

#endif // FRAME_PACING_OPTIMIZE_H