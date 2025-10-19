# FFmpeg V4L2 Uniform Location Fix

## Problem
After implementing VBO/VAO optimizations, video displayed with strange colors because shader uniform locations were not being set correctly.

### Symptoms
- Video displayed but with incorrect/strange colors
- Keystone was disabled
- Uniform locations showing as -1 in logs: `Cached shader uniform locations: Y=-1, UV=-1`

### Root Cause
The VBO/VAO optimization code attempted to cache uniform locations in the player structure during `init_ffmpeg_v4l2_player()`, but:

1. **Wrong uniform names**: Code was looking for uniforms that didn't exist or were optimized away
2. **Timing issue**: The global uniform locations were already cached when `init_nv12_shader()` was called earlier in `pickle.c`
3. **Duplicate caching**: Unnecessarily caching uniforms in both global variables AND player structure
4. **Wrong uniform access**: Render function was using player structure uniforms instead of global ones

## Solution

### 1. Export Global Uniform Locations
Changed in `pickle.c`:
```c
// From:
static GLint  g_nv12_u_texture_y_loc = -1;
static GLint  g_nv12_u_texture_uv_loc = -1;

// To:
GLint  g_nv12_u_texture_y_loc = -1;   // Non-static, can be extern'd
GLint  g_nv12_u_texture_uv_loc = -1;  // Non-static, can be extern'd
```

### 2. Declare Externs in FFmpeg Player
Added to `ffmpeg_v4l2_player.c`:
```c
// Declare external shader program and uniforms (from pickle.c)
extern GLuint g_nv12_shader_program;
extern GLint g_nv12_u_texture_y_loc;
extern GLint g_nv12_u_texture_uv_loc;
```

### 3. Use Global Uniforms in Render Function
Updated `render_ffmpeg_v4l2_frame()`:
```c
// Use global uniform locations that were cached when shader was initialized
if (g_nv12_u_texture_y_loc >= 0) glUniform1i(g_nv12_u_texture_y_loc, 0);
if (g_nv12_u_texture_uv_loc >= 0) glUniform1i(g_nv12_u_texture_uv_loc, 1);
```

### 4. Removed Duplicate Caching
Removed from `init_ffmpeg_v4l2_player()`:
```c
// REMOVED - no longer needed:
player->y_tex_uniform_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_y");
player->uv_tex_uniform_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_uv");
```

### 5. Removed Structure Fields
Removed from `ffmpeg_v4l2_player_t`:
```c
// REMOVED:
GLint y_tex_uniform_loc;  // Cached uniform location
GLint uv_tex_uniform_loc; // Cached uniform location
```

### 6. Re-enabled Keystone
Removed keystone disable code from `pickle.c`:
```c
// REMOVED:
g_keystone.enabled = false;
LOG_INFO("Keystone disabled for FFmpeg V4L2 rendering (incompatible)");
```

## How It Works Now

### Initialization Sequence
1. `init_nv12_shader()` called in `pickle.c` (line ~3633)
   - Creates shader program → `g_nv12_shader_program`
   - Caches uniform locations → `g_nv12_u_texture_y_loc`, `g_nv12_u_texture_uv_loc`

2. `init_ffmpeg_v4l2_player()` called in `pickle.c` (line ~3625)
   - Creates VBO/VAO with vertex data
   - Binds Y and UV textures to units 0 and 1 (persistent)
   - Does NOT cache uniforms (already done globally)

### Rendering Sequence
1. `ffmpeg_v4l2_upload_to_gl()` uploads frame data to textures
2. `render_ffmpeg_v4l2_frame()` renders:
   - Uses `g_nv12_shader_program` (global)
   - Sets uniform samplers using `g_nv12_u_texture_y_loc` and `g_nv12_u_texture_uv_loc` (global)
   - Binds `player->vao` and draws (VBO/VAO optimization)

## Benefits
- **Correct colors**: Uniforms now properly linked to shader
- **Keystone works**: Can use keystone correction with FFmpeg V4L2 rendering
- **Simpler code**: Single source of truth for uniform locations
- **Maintains optimizations**: VBO/VAO and persistent texture bindings still active

## Files Modified
1. `pickle.c` - Removed `static` from uniform location variables, removed keystone disable
2. `ffmpeg_v4l2_player.c` - Added extern declarations, use global uniforms in render function, removed duplicate caching
3. `ffmpeg_v4l2_player.h` - Removed uniform location fields from structure

## Verification
```bash
$ ./pickle -p -l rpi4-e.mp4
[INFO]  Created VBO/VAO for optimized rendering: VAO=1, VBO=1
[INFO]  Bound textures to units 0 and 1 (persistent state)
[INFO]  FFmpeg V4L2 player initialized successfully
[INFO] NV12 shader initialized for FFmpeg V4L2 rendering
[INFO] FFmpeg V4L2 decoder initialized successfully: 1920x1080 @ 59.94 fps
Keystone correction enabled.  # <-- Keystone now works!
[INFO]  Frame #1 Y samples: [0]=14 [100]=16 [1000]=16, UV samples: [0]=128,128 [100]=128,128
[INFO]  Uploaded frame #1 to GL (Y tex: 1, UV tex: 2)
```

✅ Video displays with correct colors  
✅ Keystone correction available  
✅ VBO/VAO optimizations active  
✅ Hardware decoding working

---
*Fix Date: October 19, 2025*
*Issue: Strange colors and missing keystone*
*Status: ✅ Resolved*
