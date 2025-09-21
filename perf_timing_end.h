// Performance timing
if (g_frame_timing_enabled) {
    gettimeofday(&end_time, NULL);
    long render_time_us = (end_time.tv_sec - start_time.tv_sec) * 1000000 + (end_time.tv_usec - start_time.tv_usec);
    fprintf(stderr, "[TIMING] Frame render time: %.2f ms\n", render_time_us / 1000.0f);
}