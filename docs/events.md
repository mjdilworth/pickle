# Event-Driven Architecture in Pickle

Pickle now supports an event-driven architecture using epoll for more efficient event handling. This document describes how this system works and how to enable it.

## Overview

The event-driven architecture replaces the traditional polling loop with an epoll-based event system that:

1. Is more efficient for handling multiple event sources
2. Provides better scalability as more event sources are added
3. Has cleaner separation of concerns with event-specific callbacks
4. Reduces CPU usage in waiting for events

## How to Enable

The event-driven architecture is enabled by default, but can be controlled through the Makefile:

```
make EVENT=1     # Build with event-driven architecture (default)
make EVENT=0     # Build with traditional polling loop
```

You can also disable it at runtime with the `--no-event-driven` command line flag.

## Architecture Components

The event system consists of these main components:

1. **event.h/c**: Core event system implementation using epoll
2. **event_callbacks.h/c**: Event handlers for different event types
3. **pickle_events.h/c**: Integration layer connecting Pickle to the event system

## Supported Event Types

The following event types are supported:

- **DRM Events**: Page flip completion and VBlank events
- **MPV Events**: Playback events from the libmpv player
- **Input Events**: Keyboard and joystick input
- **Timer Events**: For periodic tasks like frame rate limiting
- **Signal Events**: For handling system signals like SIGINT

## Benefits

- **Reduced CPU Usage**: The event-driven approach avoids busy polling
- **Better Responsiveness**: Lower latency response to events
- **Cleaner Code**: Better separation of event handling logic
- **Future Extensibility**: Easier to add new event sources

## Implementation Details

The event system uses Linux's epoll API for efficient event notification. Event sources register callbacks that are executed when their events occur.

The main loop in pickle.c is replaced with a simple event processing loop that delegates all event handling to the event system.