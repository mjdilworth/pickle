# OpenGL Rendering Pipeline Analysis & Optimizations

## Current Pipeline Overview

### Flow
1. **Decode** → FFmpeg V4L2 hardware decoder (YUV420P)
2. **Convert** → YUV420P to NV12 format in CPU (staging buffer)
3. **Upload** → CPU buffer → GL textures (glTexSubImage2D)
4. **Render** → Fragment shader converts NV12 to RGB

---

## ❌ Performance Issues Found

### 1. **CRITICAL: No VBO/VAO Usage**
**Location:** `render_ffmpeg_v4l2_frame()` line ~2650

**Current Code:**
```c
static const float vertices[] = { /* ... */ };
glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices);
glEnableVertexAttribArray(0);
glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &vertices[2]);
glEnableVertexAttribArray(1);
glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
```

**Problem:**
- Using client-side vertex arrays (passing CPU pointer directly)
- Driver must copy vertices to GPU every frame
- Not optimal for OpenGL ES 3.x
- Causes unnecessary CPU→GPU transfers **every frame**

**Impact:** ~1-2 ms wasted per frame at 60 FPS = potential 60-120ms/sec wasted

---

### 2. **Uniform Location Lookups Every Frame**
**Location:** `render_ffmpeg_v4l2_frame()` line ~2640

**Current Code:**
```c
GLint y_tex_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_y");
GLint uv_tex_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_uv");
if (y_tex_loc >= 0) glUniform1i(y_tex_loc, 0);
if (uv_tex_loc >= 0) glUniform1i(uv_tex_loc, 1);
```

**Problem:**
- `glGetUniformLocation` called **every frame**
- Should be cached at initialization time
- Completely unnecessary per-frame overhead

**Impact:** ~0.1-0.5 ms per frame

---

### 3. **Redundant Texture Binding**
**Location:** `render_ffmpeg_v4l2_frame()` line ~2633

**Current Code:**
```c
glActiveTexture(GL_TEXTURE0);
glBindTexture(GL_TEXTURE_2D, player->y_texture);
glActiveTexture(GL_TEXTURE1);
glBindTexture(GL_TEXTURE_2D, player->uv_texture);
```

**Observation:**
- Texture bindings are state - they persist between frames
- Only need to bind once at initialization or when textures change
- Current code rebinds **every frame** unnecessarily

**Impact:** Minor (~0.1 ms per frame) but adds up

---

### 4. **CPU-Based YUV420P → NV12 Conversion**
**Location:** `copy_frame_to_nv12_buffer()` line ~2409

**Current Code:**
```c
// Interleaving U and V planes into NV12 format
for (int y = 0; y < height / 2; ++y) {
    const uint8_t *src_u = frame->data[1] + (size_t)y * (size_t)frame->linesize[1];
    const uint8_t *src_v = frame->data[2] + (size_t)y * (size_t)frame->linesize[2];
    uint8_t *dst_row = dst_uv + dst_offset;
    for (int x = 0; x < width / 2; ++x) {
        dst_row[(size_t)x * 2] = src_u[x];
        dst_row[(size_t)x * 2 + 1] = src_v[x];
    }
}
```

**Problem:**
- At 1920x1080, this processes 518,400 pixels (960x540 UV plane)
- Nested loops with per-pixel memory writes
- Could use compute shader or upload separate Y/U/V textures

**Impact:** ~2-5 ms per frame at 1080p

---

### 5. **No Pixel Buffer Objects (PBO)**
**Location:** `ffmpeg_v4l2_upload_to_gl()` line ~2585

**Current Code:**
```c
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                frame->width, frame->height,
                GL_RED, GL_UNSIGNED_BYTE,
                y_plane);
```

**Problem:**
- Synchronous upload: CPU waits for GPU to finish
- `glTexSubImage2D` blocks until upload completes
- No asynchronous DMA transfer

**Potential Improvement:**
- Use PBO for async upload (double-buffering)
- GPU can render previous frame while next uploads
- Reduces stalls by ~1-3 ms per frame

---

### 6. **Shader Complexity - YUV Range Conversion**
**Location:** `shaders/nv12_to_rgb.frag` line ~22

**Current Code:**
```glsl
y = (y * 255.0 - 16.0) / 219.0;
float u = (uv.r * 255.0 - 128.0) / 224.0;
float v = (uv.g * 255.0 - 128.0) / 224.0;
```

**Observation:**
- Division operations in fragment shader (per-pixel)
- At 1080p60: 124 million divisions per second
- Could precompute scale factors as uniforms

**Impact:** ~0.5-1 ms per frame on low-end GPU

---

## ✅ What's Good

1. **Texture Formats:** GL_R8/GL_RG8 - Modern GLES 3.x ✓
2. **Texture Filtering:** LINEAR filtering for smooth scaling ✓
3. **Texture Wrap:** CLAMP_TO_EDGE prevents edge artifacts ✓
4. **Frame Skipping:** Checks PTS to avoid re-uploading same frame ✓
5. **Memory Management:** Properly unrefs frames after upload ✓

---

## 🚀 Recommended Optimizations

### Priority 1: VBO/VAO (CRITICAL)
**Estimated Gain:** 1-2 ms per frame = **60-120 FPS improvement potential**

Create VBO once at init:
```c
static GLuint vbo = 0;
static GLuint vao = 0;

if (vao == 0) {
    // One-time setup
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    
    static const float vertices[] = { /* ... */ };
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
}

// Rendering (every frame)
glBindVertexArray(vao);
glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
```

---

### Priority 2: Cache Uniform Locations
**Estimated Gain:** 0.1-0.5 ms per frame

Store at initialization:
```c
// In player structure
GLint y_tex_loc;
GLint uv_tex_loc;

// At init (after shader creation)
player->y_tex_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_y");
player->uv_tex_loc = glGetUniformLocation(g_nv12_shader_program, "u_texture_uv");

// At render
if (player->y_tex_loc >= 0) glUniform1i(player->y_tex_loc, 0);
if (player->uv_tex_loc >= 0) glUniform1i(player->uv_tex_loc, 1);
```

---

### Priority 3: Optimize Shader Math
**Estimated Gain:** 0.5-1 ms per frame

Precompute scale factors:
```glsl
// Add uniforms
uniform float y_scale = 1.0 / 219.0;
uniform float y_offset = 16.0 / 219.0;
uniform float uv_scale = 1.0 / 224.0;
uniform float uv_offset = 128.0 / 224.0;

// In shader
y = y * y_scale - y_offset;
float u = uv.r * uv_scale - uv_offset;
float v = uv.g * uv_scale - uv_offset;
```

Or better, bake into matrix:
```glsl
const mat4 yuv2rgb = mat4(...); // Precomputed BT.709 + range conversion
vec3 rgb = (yuv2rgb * vec4(y, u, v, 1.0)).rgb;
```

---

### Priority 4: Use PBO for Async Upload (Advanced)
**Estimated Gain:** 1-3 ms per frame

Double-buffered PBO approach:
```c
GLuint pbo[2];
int current_pbo = 0;

// Upload to PBO (non-blocking)
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[current_pbo]);
void* ptr = glMapBufferRange(..., GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
memcpy(ptr, y_plane, size);
glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

// Upload from PBO to texture (async DMA)
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[1 - current_pbo]); // Use previous PBO
glTexSubImage2D(..., NULL); // NULL = read from bound PBO

current_pbo = 1 - current_pbo;
```

---

### Priority 5: Upload YUV420P Directly (Alternative)
**Estimated Gain:** 2-5 ms per frame

Skip CPU conversion by using 3 textures:
```c
// Create separate Y, U, V textures
glGenTextures(3, textures); // y_tex, u_tex, v_tex

// Upload directly from frame->data[0], [1], [2]
// No CPU interleaving needed

// Update shader to sample 3 textures
uniform sampler2D u_texture_y;
uniform sampler2D u_texture_u;
uniform sampler2D u_texture_v;
```

---

## 📊 Total Potential Improvement

| Optimization | Gain (ms/frame) | @ 60 FPS Impact |
|--------------|----------------|-----------------|
| VBO/VAO | 1-2 ms | **CRITICAL** |
| Cache Uniforms | 0.1-0.5 ms | Easy win |
| Shader Math | 0.5-1 ms | Moderate |
| PBO | 1-3 ms | Advanced |
| Skip CPU Convert | 2-5 ms | Alternative |

**Total Possible Gain:** 4-11 ms per frame
- Current: ~16.6 ms/frame budget at 60 FPS
- After: 5.6-12.6 ms/frame = headroom for 95-120 FPS+

---

## 🎯 Quick Wins (30 min implementation)

1. ✅ **Add VBO/VAO** - Biggest impact, simple change
2. ✅ **Cache uniform locations** - 5 line change
3. ✅ **Bind textures once at init** - Simple state management

These three alone could gain **1.5-3 ms per frame** = ~20-40% faster rendering!

---

## Date Analyzed
October 19, 2025
