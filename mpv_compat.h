#ifndef MPV_COMPAT_H
#define MPV_COMPAT_H

#include "mpv.h"

// Compatibility macros to translate between field names
// This allows the rest of the code to work with the old field names
// while using the structure defined in mpv.h

#define mpv_compat_init(player) do { \
    memset((player), 0, sizeof(mpv_player_t)); \
} while(0)

// Field translation macros
#define mpv handle
#define rctx render_ctx
#define using_libmpv initialized  // not a perfect match but close enough for init/cleanup

#endif /* MPV_COMPAT_H */