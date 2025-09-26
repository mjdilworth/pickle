#!/bin/bash
# This script fixes the field name differences between pickle.c and mpv.h

# Fix all mpv field references to handle
sed -i 's/\->mpv/\->handle/g' pickle.c

# Fix all rctx field references to render_ctx
sed -i 's/\->rctx/\->render_ctx/g' pickle.c

# Fix player.mpv to player.handle
sed -i 's/player\.mpv/player\.handle/g' pickle.c

# Fix player.rctx to player.render_ctx
sed -i 's/player\.rctx/player\.render_ctx/g' pickle.c

# Fix using_libmpv to initialized
sed -i 's/\->using_libmpv/\->initialized/g' pickle.c