// Performance optimization: Set process priority (if requested)
const char *priority_env = getenv("PICKLE_PRIORITY");
if (priority_env && *priority_env) {
    int priority = atoi(priority_env);
    if (priority > 0) {
        // Set real-time priority if requested
        struct sched_param param;
        param.sched_priority = priority;
        if (sched_setscheduler(0, SCHED_RR, &param) != 0) {
            fprintf(stderr, "Warning: Failed to set real-time priority (needs root or CAP_SYS_NICE)\n");
        } else {
            fprintf(stderr, "Set real-time priority to %d\n", priority);
        }
    } else {
        // Set standard nice value (-20 to 19)
        if (setpriority(PRIO_PROCESS, 0, priority) != 0) {
            fprintf(stderr, "Warning: Failed to set process nice value\n");
        } else {
            fprintf(stderr, "Set nice value to %d\n", priority);
        }
    }
}

// Performance optimization: Set CPU affinity (if requested)
const char *affinity_env = getenv("PICKLE_CPU_AFFINITY");
if (affinity_env && *affinity_env) {
    cpu_set_t set;
    CPU_ZERO(&set);
    
    // Parse comma-separated list of CPU cores
    char *affinity_str = strdup(affinity_env);
    char *token = strtok(affinity_str, ",");
    while (token) {
        int cpu = atoi(token);
        if (cpu >= 0 && cpu < CPU_SETSIZE) {
            CPU_SET(cpu, &set);
            fprintf(stderr, "Adding CPU %d to affinity mask\n", cpu);
        }
        token = strtok(NULL, ",");
    }
    free(affinity_str);
    
    // Apply CPU affinity
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        fprintf(stderr, "Warning: Failed to set CPU affinity\n");
    } else {
        fprintf(stderr, "Set CPU affinity mask\n");
    }
}

// Read other performance-related environment variables
const char *skip_env = getenv("PICKLE_SKIP_UNCHANGED");
if (skip_env && *skip_env) {
    g_skip_unchanged_frames = atoi(skip_env);
}

const char *direct_env = getenv("PICKLE_DIRECT_RENDERING");
if (direct_env && *direct_env) {
    g_use_direct_rendering = atoi(direct_env);
}

const char *timing_env = getenv("PICKLE_FRAME_TIMING");
if (timing_env && *timing_env) {
    g_frame_timing_enabled = atoi(timing_env);
}

const char *disable_keystone_env = getenv("PICKLE_DISABLE_KEYSTONE");
if (disable_keystone_env && *disable_keystone_env) {
    g_disable_keystone = atoi(disable_keystone_env);
    if (g_disable_keystone) {
        fprintf(stderr, "Keystone correction completely disabled for maximum performance\n");
    }
}