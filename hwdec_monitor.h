#ifndef HWDEC_MONITOR_H
#define HWDEC_MONITOR_H

#include <stdbool.h>
#include <mpv/client.h>

// Hardware decoder monitoring state
typedef struct {
    bool hwdec_checked;
    bool hwdec_failed;
    char *current_file;
} hwdec_monitor_t;

// Function declarations
void hwdec_monitor_init(hwdec_monitor_t *monitor);
void hwdec_monitor_cleanup(hwdec_monitor_t *monitor);
void hwdec_monitor_reset(hwdec_monitor_t *monitor);
bool hwdec_monitor_check_failure(hwdec_monitor_t *monitor, mpv_handle *handle, const char *filename);
void hwdec_monitor_log_message(const mpv_event_log_message *lm);

#endif // HWDEC_MONITOR_H