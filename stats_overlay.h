#ifndef STATS_OVERLAY_H
#define STATS_OVERLAY_H

#include <sys/time.h>
#include <time.h>

// Stats overlay structure
typedef struct {
    // FPS tracking
    int frame_count;
    struct timeval last_fps_update;
    float current_fps;
    
    // CPU usage tracking
    float cpu_usage;
    struct timeval last_cpu_update;
    unsigned long long last_total_time;
    unsigned long long last_idle_time;
    
    // GPU usage (approximate based on render time)
    // For Raspberry Pi, values are scaled where 10ms render time = 100% usage
    float gpu_usage;
    struct timeval last_render_start;
    struct timeval last_render_end;
    float avg_render_time_ms;
    
    // Memory usage
    float memory_usage_mb;
    
    // Display position
    int x_pos;
    int y_pos;
} stats_overlay_t;

// Global stats overlay instance
extern stats_overlay_t g_stats_overlay;

// Function declarations
void stats_overlay_init(stats_overlay_t *stats);
void stats_overlay_update(stats_overlay_t *stats);
void stats_overlay_render_frame_start(stats_overlay_t *stats);
void stats_overlay_render_frame_end(stats_overlay_t *stats);
void stats_overlay_render_text(stats_overlay_t *stats, int screen_width, int screen_height);

#endif // STATS_OVERLAY_H