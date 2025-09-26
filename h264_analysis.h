#ifndef H264_ANALYSIS_H
#define H264_ANALYSIS_H

#include <stdbool.h>
#include <stdint.h>

// H.264 profile information structure
typedef struct {
    char *codec_name;
    char *format_name;
    int64_t width;
    int64_t height;
    double fps;
    bool is_h264;
    bool hw_compatible;
    char *compatibility_warning;
} h264_analysis_result_t;

// Function declarations
bool analyze_h264_profile(const char *filename, h264_analysis_result_t *result);
void free_h264_analysis_result(h264_analysis_result_t *result);
bool is_h264_hw_compatible(h264_analysis_result_t *result);
void log_h264_compatibility_info(const h264_analysis_result_t *result, const char *filename);

#endif // H264_ANALYSIS_H