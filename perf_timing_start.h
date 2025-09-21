// Performance timing
struct timeval start_time, end_time;
if (g_frame_timing_enabled) {
    gettimeofday(&start_time, NULL);
}