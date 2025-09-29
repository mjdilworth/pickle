#include "decoder_pacing.h"
#include "log.h"
#include <sys/time.h>
#include <stdio.h>
#include <math.h>

// Global frame pacing optimizer
frame_pacing_optimizer_t g_frame_pacer = {0};

void decoder_pacing_init(double initial_target_fps) {
    g_frame_pacer.target_fps = initial_target_fps;
    g_frame_pacer.measured_render_fps = initial_target_fps;
    g_frame_pacer.adaptation_factor = 0.1; // Conservative adaptation
    g_frame_pacer.frames_rendered = 0;
    g_frame_pacer.frames_dropped = 0;
    g_frame_pacer.adaptive_enabled = true;
    gettimeofday(&g_frame_pacer.last_adaptation, NULL);
    
    LOG_INFO("Decoder pacing initialized: target=%.1f FPS", initial_target_fps);
}

void decoder_pacing_update(double current_render_fps, uint64_t frames_dropped) {
    if (!g_frame_pacer.adaptive_enabled) return;
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    // Only adapt every 2 seconds to avoid oscillation
    double elapsed = (double)(now.tv_sec - g_frame_pacer.last_adaptation.tv_sec) + 
                    (double)(now.tv_usec - g_frame_pacer.last_adaptation.tv_usec) / 1000000.0;
    
    if (elapsed < 2.0) return;
    
    // Update measurements
    g_frame_pacer.measured_render_fps = current_render_fps;
    uint64_t new_drops = frames_dropped - g_frame_pacer.frames_dropped;
    g_frame_pacer.frames_dropped = frames_dropped;
    
    // Calculate drop rate
    double drop_rate = (double)new_drops / (current_render_fps * elapsed);
    
    // Adapt target FPS based on performance
    if (drop_rate > 0.1) {
        // High drop rate - reduce target FPS
        g_frame_pacer.target_fps *= (1.0 - g_frame_pacer.adaptation_factor);
        LOG_INFO("High drops (%.1f%%), reducing target FPS to %.1f", drop_rate * 100, g_frame_pacer.target_fps);
    } else if (drop_rate < 0.02 && current_render_fps >= g_frame_pacer.target_fps * 0.95) {
        // Low drops and good performance - slightly increase target
        g_frame_pacer.target_fps *= (1.0 + g_frame_pacer.adaptation_factor * 0.5);
        if (g_frame_pacer.target_fps > 60.0) g_frame_pacer.target_fps = 60.0; // Cap at 60 FPS
        LOG_INFO("Good performance, increasing target FPS to %.1f", g_frame_pacer.target_fps);
    }
    
    g_frame_pacer.last_adaptation = now;
}

int decoder_pacing_get_queue_size(void) {
    // Adaptive queue size based on target FPS
    if (g_frame_pacer.target_fps >= 45.0) {
        return 2; // Small queue for high FPS
    } else if (g_frame_pacer.target_fps >= 30.0) {
        return 3; // Medium queue
    } else {
        return 4; // Larger queue for low FPS
    }
}

bool decoder_pacing_should_throttle(void) {
    // Throttle if we're significantly above target
    return g_frame_pacer.measured_render_fps > g_frame_pacer.target_fps * 1.2;
}

int decoder_pacing_get_timeout_ms(void) {
    // Adaptive timeout based on target FPS
    double target_frame_time = 1000.0 / g_frame_pacer.target_fps;
    int timeout = (int)(target_frame_time * 0.5); // Half frame time
    
    // Bounds
    if (timeout < 1) timeout = 1;
    if (timeout > 33) timeout = 33;
    
    return timeout;
}