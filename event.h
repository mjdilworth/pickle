#ifndef EVENT_H
#define EVENT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/epoll.h>

/**
 * @file event.h
 * @brief Event-driven architecture for Pickle video player
 * 
 * This module implements an epoll-based event handling system to replace
 * the current poll()-based approach. It provides a more efficient and
 * flexible way to handle events from various sources (DRM, joystick, mpv, etc.)
 */

// Define event types for different sources
typedef enum {
    EVENT_TYPE_DRM,         // DRM page flip and other events
    EVENT_TYPE_MPV,         // MPV player events
    EVENT_TYPE_INPUT,       // Keyboard input events
    EVENT_TYPE_JOYSTICK,    // Joystick/gamepad events
    EVENT_TYPE_TIMER,       // Timer events
    EVENT_TYPE_CUSTOM,      // Custom events
    EVENT_TYPE_SIGNAL       // Signal events
} event_type_t;

// Event callback function type
typedef void (*event_callback_t)(int fd, uint32_t events, void *user_data);

// Event source structure
typedef struct {
    int fd;                 // File descriptor to monitor
    event_type_t type;      // Type of event source
    event_callback_t cb;    // Callback function
    void *user_data;        // User data to pass to callback
    uint32_t events;        // Events to monitor (EPOLLIN, EPOLLOUT, etc.)
    bool registered;        // Whether this source is registered
    int signal_fd;          // For signal events only
} event_source_t;

// Event context structure
typedef struct {
    int epoll_fd;           // Epoll file descriptor
    event_source_t *sources;// Array of event sources
    int max_sources;        // Maximum number of event sources
    int num_sources;        // Current number of event sources
    bool initialized;       // Whether the event system is initialized
    bool running;           // Whether the event loop is running
} event_ctx_t;

/**
 * Initialize the event system
 * 
 * @param max_sources Maximum number of event sources to support
 * @return A pointer to the event context, or NULL on failure
 */
event_ctx_t *event_init(int max_sources);

/**
 * Clean up the event system
 * 
 * @param ctx Event context
 */
void event_cleanup(event_ctx_t *ctx);

/**
 * Register a file descriptor for event monitoring
 * 
 * @param ctx Event context
 * @param fd File descriptor to monitor
 * @param type Type of event source
 * @param events Events to monitor (EPOLLIN, EPOLLOUT, etc.)
 * @param cb Callback function
 * @param user_data User data to pass to callback
 * @return The index of the registered source, or -1 on failure
 */
int event_register(event_ctx_t *ctx, int fd, event_type_t type, 
                   uint32_t events, event_callback_t cb, void *user_data);

/**
 * Unregister a file descriptor from event monitoring
 * 
 * @param ctx Event context
 * @param fd File descriptor to unregister
 * @return true if successful, false otherwise
 */
bool event_unregister(event_ctx_t *ctx, int fd);

/**
 * Register a signal for event monitoring
 * 
 * @param ctx Event context
 * @param signum Signal number
 * @param cb Callback function
 * @param user_data User data to pass to callback
 * @return The index of the registered source, or -1 on failure
 */
int event_register_signal(event_ctx_t *ctx, int signum, 
                          event_callback_t cb, void *user_data);

/**
 * Run the event loop once (non-blocking)
 * 
 * @param ctx Event context
 * @param timeout_ms Timeout in milliseconds, -1 for no timeout
 * @return Number of events processed, 0 on timeout, -1 on error
 */
int event_process(event_ctx_t *ctx, int timeout_ms);

/**
 * Run the event loop until stopped
 * 
 * @param ctx Event context
 * @return true if successful, false otherwise
 */
bool event_run(event_ctx_t *ctx);

/**
 * Stop the event loop
 * 
 * @param ctx Event context
 */
void event_stop(event_ctx_t *ctx);

/**
 * Create a timer event
 * 
 * @param ctx Event context
 * @param interval_ms Timer interval in milliseconds
 * @param cb Callback function
 * @param user_data User data to pass to callback
 * @return The timer file descriptor, or -1 on failure
 */
int event_create_timer(event_ctx_t *ctx, int interval_ms, 
                       event_callback_t cb, void *user_data);

/**
 * Modify timer interval
 * 
 * @param timer_fd Timer file descriptor
 * @param interval_ms New interval in milliseconds
 * @return true if successful, false otherwise
 */
bool event_modify_timer(int timer_fd, int interval_ms);

#endif /* EVENT_H */