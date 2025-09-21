#include <sched.h>        // For CPU affinity
#include <sys/resource.h> // For process priority

// Performance variables
static int g_frame_timing_enabled = 0;   // Enable detailed frame timing logs
static int g_skip_unchanged_frames = 1;  // Skip rendering if frame hasn't changed
static int g_use_direct_rendering = 1;   // Use direct rendering when possible
static int g_disable_keystone = 0;       // Completely disable keystone for max performance