#include "event.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <fcntl.h>

// Define logging macros similar to other Pickle components
#define LOG_EVENT(fmt, ...) fprintf(stderr, "[EVENT] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

// Debug flag (should be externally defined)
extern int g_debug;

// Event system initialization
event_ctx_t *event_init(int max_sources) {
    // Allocate and initialize event context
    event_ctx_t *ctx = (event_ctx_t *)malloc(sizeof(event_ctx_t));
    if (!ctx) {
        LOG_ERROR("Failed to allocate memory for event context");
        return NULL;
    }
    
    memset(ctx, 0, sizeof(event_ctx_t));
    
    // Create epoll instance
    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0) {
        LOG_ERROR("epoll_create1 failed: %s", strerror(errno));
        free(ctx);
        return NULL;
    }
    
    // Allocate memory for event sources
    ctx->sources = (event_source_t *)calloc((size_t)max_sources, sizeof(event_source_t));
    if (!ctx->sources) {
        LOG_ERROR("Failed to allocate memory for event sources");
        close(ctx->epoll_fd);
        free(ctx);
        return NULL;
    }
    
    ctx->max_sources = max_sources;
    ctx->num_sources = 0;
    ctx->initialized = true;
    ctx->running = false;
    
    LOG_EVENT("Event system initialized with max %d sources", max_sources);
    return ctx;
}

// Event system cleanup
void event_cleanup(event_ctx_t *ctx) {
    if (!ctx) return;
    
    // Unregister all event sources
    for (int i = 0; i < ctx->num_sources; i++) {
        if (ctx->sources[i].registered) {
            epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, ctx->sources[i].fd, NULL);
            
            // Close signal_fd if this is a signal event
            if (ctx->sources[i].type == EVENT_TYPE_SIGNAL && ctx->sources[i].signal_fd >= 0) {
                close(ctx->sources[i].signal_fd);
            }
        }
    }
    
    // Close epoll fd and free memory
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
    }
    
    free(ctx->sources);
    free(ctx);
    
    LOG_EVENT("Event system cleaned up");
}

// Find an event source by file descriptor
static int find_source_by_fd(event_ctx_t *ctx, int fd) {
    for (int i = 0; i < ctx->num_sources; i++) {
        if (ctx->sources[i].fd == fd && ctx->sources[i].registered) {
            return i;
        }
    }
    return -1;
}

// Register a file descriptor for event monitoring
int event_register(event_ctx_t *ctx, int fd, event_type_t type, 
                   uint32_t events, event_callback_t cb, void *user_data) {
    if (!ctx || fd < 0 || !cb) {
        LOG_ERROR("Invalid parameters for event_register");
        return -1;
    }
    
    // Check if we've reached the maximum number of sources
    if (ctx->num_sources >= ctx->max_sources) {
        LOG_ERROR("Maximum number of event sources reached");
        return -1;
    }
    
    // Check if the fd is already registered
    if (find_source_by_fd(ctx, fd) >= 0) {
        LOG_ERROR("File descriptor %d already registered", fd);
        return -1;
    }
    
    // Register with epoll
    struct epoll_event ev;
    ev.events = events;
    ev.data.u32 = (uint32_t)ctx->num_sources;
    
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl add failed for fd %d: %s", fd, strerror(errno));
        return -1;
    }
    
    // Add to our sources array
    ctx->sources[ctx->num_sources].fd = fd;
    ctx->sources[ctx->num_sources].type = type;
    ctx->sources[ctx->num_sources].cb = cb;
    ctx->sources[ctx->num_sources].user_data = user_data;
    ctx->sources[ctx->num_sources].events = events;
    ctx->sources[ctx->num_sources].registered = true;
    ctx->sources[ctx->num_sources].signal_fd = -1;
    
    LOG_EVENT("Registered fd %d as event source %d of type %d", 
              fd, ctx->num_sources, type);
    
    return ctx->num_sources++;
}

// Unregister a file descriptor from event monitoring
bool event_unregister(event_ctx_t *ctx, int fd) {
    if (!ctx || fd < 0) {
        LOG_ERROR("Invalid parameters for event_unregister");
        return false;
    }
    
    int idx = find_source_by_fd(ctx, fd);
    if (idx < 0) {
        LOG_ERROR("File descriptor %d not registered", fd);
        return false;
    }
    
    // Remove from epoll
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        LOG_ERROR("epoll_ctl del failed for fd %d: %s", fd, strerror(errno));
        return false;
    }
    
    // If this is a signal event, close the signal fd
    if (ctx->sources[idx].type == EVENT_TYPE_SIGNAL && ctx->sources[idx].signal_fd >= 0) {
        close(ctx->sources[idx].signal_fd);
    }
    
    // Mark as unregistered
    ctx->sources[idx].registered = false;
    
    LOG_EVENT("Unregistered fd %d (event source %d)", fd, idx);
    
    return true;
}

// Register a signal for event monitoring
int event_register_signal(event_ctx_t *ctx, int signum, 
                          event_callback_t cb, void *user_data) {
    if (!ctx || signum <= 0 || !cb) {
        LOG_ERROR("Invalid parameters for event_register_signal");
        return -1;
    }
    
    // Block the signal
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, signum);
    
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_ERROR("sigprocmask failed: %s", strerror(errno));
        return -1;
    }
    
    // Create a signalfd
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) {
        LOG_ERROR("signalfd failed: %s", strerror(errno));
        return -1;
    }
    
    // Register the signalfd with epoll
    int idx = event_register(ctx, sfd, EVENT_TYPE_SIGNAL, EPOLLIN, cb, user_data);
    if (idx < 0) {
        close(sfd);
        LOG_ERROR("Failed to register signalfd");
        return -1;
    }
    
    // Store the signal fd for cleanup
    ctx->sources[idx].signal_fd = sfd;
    
    LOG_EVENT("Registered signal %d with fd %d as event source %d", 
              signum, sfd, idx);
    
    return idx;
}

// Process events once
int event_process(event_ctx_t *ctx, int timeout_ms) {
    if (!ctx) {
        LOG_ERROR("Invalid parameters for event_process");
        return -1;
    }
    
    // Wait for events
    struct epoll_event events[16];
    int nfds = epoll_wait(ctx->epoll_fd, events, 16, timeout_ms);
    
    if (nfds < 0) {
        if (errno == EINTR) {
            // Interrupted by signal, not an error
            return 0;
        }
        LOG_ERROR("epoll_wait failed: %s", strerror(errno));
        return -1;
    }
    
    // Process events
    for (int i = 0; i < nfds; i++) {
        uint32_t idx = events[i].data.u32;
        
        if (idx >= (uint32_t)ctx->num_sources || !ctx->sources[idx].registered) {
            LOG_ERROR("Invalid event source index: %u", idx);
            continue;
        }
        
        // Call the callback
        ctx->sources[idx].cb(ctx->sources[idx].fd, events[i].events, 
                             ctx->sources[idx].user_data);
    }
    
    return nfds;
}

// Run the event loop until stopped
bool event_run(event_ctx_t *ctx) {
    if (!ctx) {
        LOG_ERROR("Invalid parameters for event_run");
        return false;
    }
    
    ctx->running = true;
    LOG_EVENT("Starting event loop");
    
    while (ctx->running) {
        if (event_process(ctx, -1) < 0) {
            // Fatal error
            ctx->running = false;
            LOG_ERROR("Event loop exiting due to error");
            return false;
        }
    }
    
    LOG_EVENT("Event loop stopped");
    return true;
}

// Stop the event loop
void event_stop(event_ctx_t *ctx) {
    if (!ctx) return;
    
    ctx->running = false;
    LOG_EVENT("Event loop stop requested");
}

// Create a timer event
int event_create_timer(event_ctx_t *ctx, int interval_ms, 
                       event_callback_t cb, void *user_data) {
    if (!ctx || interval_ms <= 0 || !cb) {
        LOG_ERROR("Invalid parameters for event_create_timer");
        return -1;
    }
    
    // Create a timerfd
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        LOG_ERROR("timerfd_create failed: %s", strerror(errno));
        return -1;
    }
    
    // Set the timer interval
    struct itimerspec its;
    its.it_interval.tv_sec = interval_ms / 1000;
    its.it_interval.tv_nsec = (interval_ms % 1000) * 1000000;
    its.it_value = its.it_interval;  // Start immediately
    
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        LOG_ERROR("timerfd_settime failed: %s", strerror(errno));
        close(tfd);
        return -1;
    }
    
    // Register the timerfd with epoll
    int idx = event_register(ctx, tfd, EVENT_TYPE_TIMER, EPOLLIN, cb, user_data);
    if (idx < 0) {
        close(tfd);
        LOG_ERROR("Failed to register timerfd");
        return -1;
    }
    
    LOG_EVENT("Created timer with interval %d ms as fd %d (event source %d)", 
              interval_ms, tfd, idx);
    
    return tfd;
}

// Modify timer interval
bool event_modify_timer(int timer_fd, int interval_ms) {
    if (timer_fd < 0 || interval_ms <= 0) {
        LOG_ERROR("Invalid parameters for event_modify_timer");
        return false;
    }
    
    // Set the new timer interval
    struct itimerspec its;
    its.it_interval.tv_sec = interval_ms / 1000;
    its.it_interval.tv_nsec = (interval_ms % 1000) * 1000000;
    its.it_value = its.it_interval;  // Start immediately
    
    if (timerfd_settime(timer_fd, 0, &its, NULL) < 0) {
        LOG_ERROR("timerfd_settime failed: %s", strerror(errno));
        return false;
    }
    
    LOG_EVENT("Modified timer fd %d to interval %d ms", timer_fd, interval_ms);
    
    return true;
}