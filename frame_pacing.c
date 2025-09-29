#include "frame_pacing.h"
#include "utils.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

// Utility function to get current time with nanosecond precision
static void get_current_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

// Utility function to add nanoseconds to a timespec
static void timespec_add_ns(struct timespec *ts, long ns) {
    ts->tv_nsec += ns;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

// Utility function to calculate difference between two timespecs in nanoseconds
static long timespec_diff_ns(const struct timespec *start, const struct timespec *end) {
    return (end->tv_sec - start->tv_sec) * 1000000000L + (end->tv_nsec - start->tv_nsec);
}

// Initialize frame pacing
pickle_result_t frame_pacing_init(frame_pacing_context_t *ctx, double refresh_rate) {
    if (!ctx) {
        return PICKLE_ERROR_INVALID_PARAM;
    }
    
    // Clear the context
    memset(ctx, 0, sizeof(frame_pacing_context_t));
    
    // Set default values
    ctx->refresh_rate = refresh_rate > 0 ? refresh_rate : 60.0;
    ctx->frame_duration_ns = 1000000000.0 / ctx->refresh_rate;
    ctx->enabled = true;
    ctx->adaptive_vsync = true;
    ctx->max_frame_lateness_ms = 2;  // Default: skip frames that are more than 2ms late
    ctx->first_frame = true;
    
    // Initialize statistics
    ctx->frames_rendered = 0;
    ctx->frames_skipped = 0;
    ctx->avg_frame_time = 0.0;
    ctx->max_frame_time = 0.0;
    ctx->min_frame_time = 1000000000.0;  // Start with a high value
    
    // Get current time as starting point
    get_current_time(&ctx->last_frame_time);
    ctx->next_frame_time = ctx->last_frame_time;
    
    return PICKLE_SUCCESS;
}

// Calculate next frame time
void frame_pacing_next_frame(frame_pacing_context_t *ctx) {
    if (!ctx->enabled) {
        return;
    }
    
    if (ctx->first_frame) {
        // For first frame, just use current time
        get_current_time(&ctx->next_frame_time);
        ctx->first_frame = false;
    } else {
        // Calculate next frame time based on last frame time plus frame duration
        ctx->next_frame_time = ctx->last_frame_time;
        timespec_add_ns(&ctx->next_frame_time, (long)ctx->frame_duration_ns);
    }
}

// Wait until it's time to present the next frame
bool frame_pacing_wait_next_frame(frame_pacing_context_t *ctx) {
    if (!ctx->enabled) {
        return true;  // Always render if pacing is disabled
    }
    
    struct timespec current_time;
    get_current_time(&current_time);
    
    // Calculate time until next frame in nanoseconds
    long time_until_next_frame_ns = timespec_diff_ns(&current_time, &ctx->next_frame_time);
    
    // If we're already late by more than max_frame_lateness_ms, skip this frame
    if (time_until_next_frame_ns < -(ctx->max_frame_lateness_ms * 1000000L)) {
        ctx->frames_skipped++;
        return false;
    }
    
    // If we need to wait, sleep until the next frame time
    if (time_until_next_frame_ns > 0) {
        struct timespec sleep_time;
        sleep_time.tv_sec = time_until_next_frame_ns / 1000000000L;
        sleep_time.tv_nsec = time_until_next_frame_ns % 1000000000L;
        nanosleep(&sleep_time, NULL);
    }
    
    return true;
}

// Notify that a frame has been presented
void frame_pacing_frame_presented(frame_pacing_context_t *ctx) {
    if (!ctx->enabled) {
        return;
    }
    
    // Store current time as last frame time
    get_current_time(&ctx->last_frame_time);
    
    // Update statistics
    ctx->frames_rendered++;
    
    // Calculate frame time and update statistics
    long frame_time_ns = timespec_diff_ns(&ctx->next_frame_time, &ctx->last_frame_time);
    double frame_time_abs = fabs((double)frame_time_ns);
    
    // Update running average
    ctx->avg_frame_time = (ctx->avg_frame_time * (double)(ctx->frames_rendered - 1) + 
                           frame_time_abs) / (double)ctx->frames_rendered;
    
    // Update min/max
    if (frame_time_abs > ctx->max_frame_time) {
        ctx->max_frame_time = frame_time_abs;
    }
    if (frame_time_abs < ctx->min_frame_time) {
        ctx->min_frame_time = frame_time_abs;
    }
    
    // Calculate next frame time
    frame_pacing_next_frame(ctx);
}

// Get frame pacing statistics as formatted string
void frame_pacing_get_stats(frame_pacing_context_t *ctx, char *buffer, size_t buffer_size) {
    if (!ctx || !buffer || buffer_size == 0) {
        return;
    }
    
    snprintf(buffer, buffer_size,
            "Frame Pacing Stats:\n"
            "  Frames rendered: %lu\n"
            "  Frames skipped: %lu\n"
            "  Avg frame deviation: %.2f ms\n"
            "  Min frame deviation: %.2f ms\n"
            "  Max frame deviation: %.2f ms\n"
            "  Target frame time: %.2f ms\n",
            ctx->frames_rendered,
            ctx->frames_skipped,
            ctx->avg_frame_time / 1000000.0,
            ctx->min_frame_time / 1000000.0,
            ctx->max_frame_time / 1000000.0,
            ctx->frame_duration_ns / 1000000.0);
}

// Reset frame pacing statistics
void frame_pacing_reset_stats(frame_pacing_context_t *ctx) {
    if (!ctx) {
        return;
    }
    
    ctx->frames_rendered = 0;
    ctx->frames_skipped = 0;
    ctx->avg_frame_time = 0.0;
    ctx->max_frame_time = 0.0;
    ctx->min_frame_time = 1000000000.0;
}