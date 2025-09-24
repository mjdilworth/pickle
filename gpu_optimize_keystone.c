// File: gpu_optimize_keystone.c
#include "gpu_optimize_keystone.h"
#include "log.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <drm_fourcc.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>

// EGL extension function pointer type
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy, EGLImageKHR image);

// Rendering path options
typedef enum {
    RENDER_PATH_NONE = 0,
    RENDER_PATH_VULKAN,     // Vulkan compute shader (best performance)
    RENDER_PATH_COMPUTE,    // OpenGL ES compute shader (good performance)
    RENDER_PATH_FRAGMENT,   // OpenGL ES fragment shader (decent performance)
    RENDER_PATH_SOFTWARE    // Software fallback (poor performance)
} render_path_t;

// Performance metrics structure
typedef struct {
    uint64_t frames_processed;
    double total_gpu_time_ms;
    double total_cpu_time_ms;
    double min_frame_time;
    double max_frame_time;
    double avg_frame_time;
    struct {
        time_t tv_sec;
        long tv_nsec;
    } last_frame_time;  // Simplified timespec for portability
    double frame_times[60];  // Rolling window of frame times
    int frame_time_idx;
} perf_metrics_t;

// GPU optimization context
typedef struct {
    // Capabilities
    render_path_t active_path;
    bool has_vulkan_support;
    bool has_compute_shader;
    bool has_atomic_modesetting;
    bool use_dmabuf;
    bool use_drm_planes;
    
    // OpenGL resources
    GLuint compute_program;       // True compute shader program
    GLuint fragment_program;      // Fragment shader program (fallback)
    GLuint vao;                   // Vertex array object for fragment shader
    GLuint vbo;                   // Vertex buffer for fragment shader
    GLuint input_texture;
    GLuint output_texture;
    
    // Temporary objects for texture compatibility
    GLuint temp_fbo;
    GLuint temp_texture;
    GLuint compute_output_texture; // For compute shader output
    
    // Compute shader specific
    GLuint ssbo;                   // Shader storage buffer object
    
    // DMA-BUF handles
    int dmabuf_fd;
    EGLImageKHR egl_image;
    
    // Atomic modesetting resources
    int drm_fd;
    uint32_t drm_plane_id;
    
    // Performance metrics
    perf_metrics_t perf;
    
    // Configuration
    int preferred_path;  // User preference for rendering path
} gpu_optimize_ctx_t;

static gpu_optimize_ctx_t g_gpu_opt = {0};

// GPU-optimized keystone shaders (support both compute and fragment shaders)

// True compute shader for hardware that supports it
static const char* optimized_keystone_compute_shader = 
    "#version 310 es\n"
    "layout(local_size_x = 16, local_size_y = 16) in;\n"
    "layout(binding = 0, rgba8) readonly uniform highp image2D inputImage;\n"
    "layout(binding = 1, rgba8) writeonly uniform highp image2D outputImage;\n"
    "\n"
    "uniform mat3 keystoneMatrix;\n"
    "uniform vec2 texSize;    // Size of the texture for normalization\n"
    "\n"
    "// Helper function for bilinear sampling from input texture\n"
    "vec4 bilinearSample(vec2 uv) {\n"
    "    // Compute texture coordinates in pixel space\n"
    "    vec2 pixelCoord = uv * texSize;\n"
    "    \n"
    "    // Get the four surrounding pixel coordinates\n"
    "    ivec2 p00 = ivec2(floor(pixelCoord));\n"
    "    ivec2 p10 = ivec2(p00.x + 1, p00.y);\n"
    "    ivec2 p01 = ivec2(p00.x, p00.y + 1);\n"
    "    ivec2 p11 = ivec2(p00.x + 1, p00.y + 1);\n"
    "    \n"
    "    // Ensure we don't read outside the texture bounds\n"
    "    p00 = clamp(p00, ivec2(0), ivec2(texSize) - ivec2(1));\n"
    "    p10 = clamp(p10, ivec2(0), ivec2(texSize) - ivec2(1));\n"
    "    p01 = clamp(p01, ivec2(0), ivec2(texSize) - ivec2(1));\n"
    "    p11 = clamp(p11, ivec2(0), ivec2(texSize) - ivec2(1));\n"
    "    \n"
    "    // Get interpolation factors\n"
    "    vec2 f = fract(pixelCoord);\n"
    "    \n"
    "    // Sample the four pixels\n"
    "    vec4 c00 = imageLoad(inputImage, p00);\n"
    "    vec4 c10 = imageLoad(inputImage, p10);\n"
    "    vec4 c01 = imageLoad(inputImage, p01);\n"
    "    vec4 c11 = imageLoad(inputImage, p11);\n"
    "    \n"
    "    // Bilinear interpolation\n"
    "    vec4 cx0 = mix(c00, c10, f.x);\n"
    "    vec4 cx1 = mix(c01, c11, f.x);\n"
    "    return mix(cx0, cx1, f.y);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    // Get the current pixel coordinate\n"
    "    ivec2 outputCoord = ivec2(gl_GlobalInvocationID.xy);\n"
    "    \n"
    "    // Make sure we're within bounds\n"
    "    if (outputCoord.x >= int(texSize.x) || outputCoord.y >= int(texSize.y)) {\n"
    "        return;\n"
    "    }\n"
    "    \n"
    "    // Convert to normalized coordinates [0,1]\n"
    "    vec2 normalizedCoord = vec2(outputCoord) / texSize;\n"
    "    \n"
    "    // Convert to [-1, 1] range for transformation\n"
    "    vec2 positionNdc = normalizedCoord * 2.0 - 1.0;\n"
    "    \n"
    "    // Apply inverse keystone transformation\n"
    "    vec3 sourcePos = keystoneMatrix * vec3(positionNdc, 1.0);\n"
    "    vec2 sourceCoord = sourcePos.xy / sourcePos.z;\n"
    "    \n"
    "    // Convert back to [0, 1] range for texture lookup\n"
    "    vec2 sourceTexCoord = (sourceCoord * 0.5 + 0.5);\n"
    "    \n"
    "    // Sample and write output\n"
    "    vec4 color;\n"
    "    if (sourceTexCoord.x >= 0.0 && sourceTexCoord.x <= 1.0 && \n"
    "        sourceTexCoord.y >= 0.0 && sourceTexCoord.y <= 1.0) {\n"
    "        color = bilinearSample(sourceTexCoord);\n"
    "    } else {\n"
    "        // Out of bounds - write transparent black\n"
    "        color = vec4(0.0, 0.0, 0.0, 0.0);\n"
    "    }\n"
    "    \n"
    "    // Write to the output image\n"
    "    imageStore(outputImage, outputCoord, color);\n"
    "}\n";

// Fragment shader fallback for hardware without compute shader support
static const char* optimized_keystone_fragment_shader = 
    "#version 300 es\n"
    "precision highp float;\n"
    "\n"
    "in vec2 texCoord_fs;\n"
    "out vec4 fragColor;\n"
    "\n"
    "uniform sampler2D inputTexture;\n"
    "uniform mat3 keystoneMatrix;\n"
    "\n"
    "void main() {\n"
    "    // Normalize coordinates to [0, 1] range - already in texCoord_fs\n"
    "    vec2 normalizedCoord = texCoord_fs;\n"
    "    \n"
    "    // Convert to [-1, 1] range for better numeric precision\n"
    "    vec2 positionNdc = normalizedCoord * 2.0 - 1.0;\n"
    "    \n"
    "    // Apply inverse keystone transformation\n"
    "    vec3 sourcePos = keystoneMatrix * vec3(positionNdc, 1.0);\n"
    "    vec2 sourceCoord = sourcePos.xy / sourcePos.z;\n"
    "    \n"
    "    // Convert back to [0, 1] range for texture lookup\n"
    "    vec2 sourceTexCoord = (sourceCoord * 0.5 + 0.5);\n"
    "    \n"
    "    // Read the color from the input texture\n"
    "    vec4 color;\n"
    "    if (sourceTexCoord.x >= 0.0 && sourceTexCoord.x <= 1.0 && \n"
    "        sourceTexCoord.y >= 0.0 && sourceTexCoord.y <= 1.0) {\n"
    "        color = texture(inputTexture, sourceTexCoord);\n"
    "    } else {\n"
    "        // Out of bounds - return transparent black\n"
    "        color = vec4(0.0, 0.0, 0.0, 0.0);\n"
    "    }\n"
    "    \n"
    "    // Output the color\n"
    "    fragColor = color;\n"
    "}\n";

// Vertex shader for rendering a fullscreen quad
static const char* vertex_shader_source =
    "#version 300 es\n"
    "precision highp float;\n"
    "\n"
    "in vec2 position;\n"
    "in vec2 texCoord;\n"
    "out vec2 texCoord_fs;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "    texCoord_fs = texCoord;\n"
    "}\n";

// Check if atomic modesetting is supported
static bool check_atomic_modesetting(void) {
    // Check if atomic modesetting should be forced on/off
    const char* atomic_env = getenv("PICKLE_ATOMIC_MODESETTING");
    if (atomic_env) {
        return atoi(atomic_env) != 0;
    }

    // Simplified check - we'll just look for the drm.atomic=1 flag in dmesg
    FILE* fp = popen("dmesg | grep -i 'drm.atomic=1' | wc -l", "r");
    if (fp) {
        int count = 0;
        if (fscanf(fp, "%d", &count) == 1) {
            pclose(fp);
            return count > 0;
        }
        pclose(fp);
    }
    
    // Alternative: check if atomic is mentioned in kernel parameters
    fp = popen("cat /proc/cmdline | grep -i 'drm.atomic=1' | wc -l", "r");
    if (fp) {
        int count = 0;
        if (fscanf(fp, "%d", &count) == 1) {
            pclose(fp);
            return count > 0;
        }
        pclose(fp);
    }
    
    return false;
}

// Check if Vulkan is available and should be used
static bool check_vulkan_support(void) {
    // First check if VULKAN_ENABLED was compiled in
#ifdef VULKAN_ENABLED
    // Then check environment variable
    const char* env_var = getenv("PICKLE_USE_VULKAN_GPU");
    if (env_var) {
        return atoi(env_var) != 0;
    }
    return true;  // Default to using Vulkan if available
#else
    return false;
#endif
}

// Initialize performance metrics
static void init_performance_metrics(perf_metrics_t* perf) {
    memset(perf, 0, sizeof(perf_metrics_t));
    perf->min_frame_time = 9999.0;  // Start with a high value
    
    // Get current time
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    perf->last_frame_time.tv_sec = ts.tv_sec;
    perf->last_frame_time.tv_nsec = ts.tv_nsec;
}

// Start frame timing
static void perf_frame_start(perf_metrics_t* perf) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    perf->last_frame_time.tv_sec = ts.tv_sec;
    perf->last_frame_time.tv_nsec = ts.tv_nsec;
}

// End frame timing
static void perf_frame_end(perf_metrics_t* perf) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    // Calculate frame time in milliseconds
    double frame_time = ((double)(now.tv_sec - perf->last_frame_time.tv_sec)) * 1000.0 +
                       ((double)(now.tv_nsec - perf->last_frame_time.tv_nsec)) / 1000000.0;
    
    // Update metrics
    perf->frames_processed++;
    perf->total_gpu_time_ms += frame_time;
    
    // Update min/max
    if (frame_time < perf->min_frame_time) perf->min_frame_time = frame_time;
    if (frame_time > perf->max_frame_time) perf->max_frame_time = frame_time;
    
    // Update rolling average
    perf->frame_times[perf->frame_time_idx] = frame_time;
    perf->frame_time_idx = (perf->frame_time_idx + 1) % 60;
    
    // Calculate average
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < 60; i++) {
        if (perf->frame_times[i] > 0.0) {
            sum += perf->frame_times[i];
            count++;
        }
    }
    if (count > 0) {
        perf->avg_frame_time = sum / count;
    }
    
    // Log performance every 60 frames
    if (perf->frames_processed % 60 == 0) {
        LOG_INFO("GPU Performance: avg=%.2fms min=%.2fms max=%.2fms FPS=%.1f",
                 perf->avg_frame_time, perf->min_frame_time, perf->max_frame_time,
                 1000.0 / perf->avg_frame_time);
    }
}

bool gpu_optimize_init(void) {
    LOG_INFO("Initializing GPU-optimized keystone processing");
    
    // Initialize the context
    memset(&g_gpu_opt, 0, sizeof(g_gpu_opt));
    
    // Initialize performance metrics
    init_performance_metrics(&g_gpu_opt.perf);
    
    // Check for Vulkan support first (preferred path)
    g_gpu_opt.has_vulkan_support = check_vulkan_support();
    if (g_gpu_opt.has_vulkan_support) {
        LOG_INFO("Vulkan support detected, will try Vulkan acceleration first");
    }
    
    // Check for compute shader support (OpenGL ES 3.1+)
    const char* version = (const char*)glGetString(GL_VERSION);
    if (version && (strstr(version, "OpenGL ES 3.1") || strstr(version, "OpenGL ES 3.2"))) {
        g_gpu_opt.has_compute_shader = true;
        LOG_INFO("OpenGL ES compute shader support detected: %s", version);
        
        // Get compute shader capabilities
        GLint work_group_count[3] = {0};
        GLint work_group_size[3] = {0};
        GLint max_invocations = 0;
        
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &work_group_count[0]);
        glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &work_group_size[0]);
        glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &max_invocations);
        
        LOG_INFO("Compute shader capabilities: work_group_count=%d, work_group_size=%d, max_invocations=%d",
                work_group_count[0], work_group_size[0], max_invocations);
    }
    
    // Check for EGL DMA-BUF support
    const char* egl_extensions = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (egl_extensions && strstr(egl_extensions, "EGL_EXT_image_dma_buf_import")) {
        g_gpu_opt.use_dmabuf = true;
        LOG_INFO("DMA-BUF import support detected");
    }
    
    // Check for atomic modesetting support
    g_gpu_opt.has_atomic_modesetting = check_atomic_modesetting();
    if (g_gpu_opt.has_atomic_modesetting) {
        LOG_INFO("Atomic modesetting support detected");
    } else {
        LOG_WARN("Atomic modesetting not supported - performance may be limited");
    }
    
    // Determine the best render path based on capabilities
    if (g_gpu_opt.has_vulkan_support) {
        g_gpu_opt.active_path = RENDER_PATH_VULKAN;
        LOG_INFO("Using Vulkan compute shader rendering path (highest performance)");
    } else if (g_gpu_opt.has_compute_shader) {
        g_gpu_opt.active_path = RENDER_PATH_COMPUTE;
        LOG_INFO("Using OpenGL ES compute shader rendering path");
    } else {
        g_gpu_opt.active_path = RENDER_PATH_FRAGMENT;
        LOG_INFO("Using OpenGL ES fragment shader rendering path (fallback)");
    }
    
    // Initialize shaders based on selected path
    if (g_gpu_opt.active_path == RENDER_PATH_COMPUTE) {
        // Initialize compute shader program
        GLuint compute_shader = glCreateShader(GL_COMPUTE_SHADER);
        glShaderSource(compute_shader, 1, &optimized_keystone_compute_shader, NULL);
        glCompileShader(compute_shader);
        
        // Check compilation
        GLint success;
        GLchar infoLog[512];
        glGetShaderiv(compute_shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(compute_shader, 512, NULL, infoLog);
            LOG_WARN("Compute shader compilation failed: %s", infoLog);
            glDeleteShader(compute_shader);
            
            // Fall back to fragment shader
            g_gpu_opt.active_path = RENDER_PATH_FRAGMENT;
            LOG_INFO("Falling back to fragment shader implementation");
        } else {
            // Create program and link
            g_gpu_opt.compute_program = glCreateProgram();
            glAttachShader(g_gpu_opt.compute_program, compute_shader);
            glLinkProgram(g_gpu_opt.compute_program);
            
            // Check link status
            glGetProgramiv(g_gpu_opt.compute_program, GL_LINK_STATUS, &success);
            if (!success) {
                glGetProgramInfoLog(g_gpu_opt.compute_program, 512, NULL, infoLog);
                LOG_WARN("Compute shader program linking failed: %s", infoLog);
                glDeleteProgram(g_gpu_opt.compute_program);
                
                // Fall back to fragment shader
                g_gpu_opt.active_path = RENDER_PATH_FRAGMENT;
                LOG_INFO("Falling back to fragment shader implementation");
            } else {
                LOG_INFO("Compute shader program compiled successfully");
            }
            
            // Clean up shader object
            glDeleteShader(compute_shader);
        }
    }
    
    // Initialize fragment shader if needed
    if (g_gpu_opt.active_path == RENDER_PATH_FRAGMENT) {
        // Create and compile vertex shader
        GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
        glCompileShader(vertex_shader);
        
        // Create and compile fragment shader
        GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragment_shader, 1, &optimized_keystone_fragment_shader, NULL);
        glCompileShader(fragment_shader);
        
        // Check fragment shader compilation
        GLint success;
        GLchar infoLog[512];
        glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(fragment_shader, 512, NULL, infoLog);
            LOG_WARN("Fragment shader compilation failed: %s", infoLog);
            glDeleteShader(vertex_shader);
            glDeleteShader(fragment_shader);
            return false;
        }
        
        // Create and link program
        g_gpu_opt.fragment_program = glCreateProgram();
        glAttachShader(g_gpu_opt.fragment_program, vertex_shader);
        glAttachShader(g_gpu_opt.fragment_program, fragment_shader);
        glLinkProgram(g_gpu_opt.fragment_program);
        
        // Check program linking
        glGetProgramiv(g_gpu_opt.fragment_program, GL_LINK_STATUS, &success);
        if (success) {
            LOG_INFO("Fragment shader program compiled successfully");
            
            // Create VAO and VBO for fullscreen quad
            glGenVertexArrays(1, &g_gpu_opt.vao);
            glBindVertexArray(g_gpu_opt.vao);
            
            glGenBuffers(1, &g_gpu_opt.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, g_gpu_opt.vbo);
            
            // Define fullscreen quad vertices with texcoords
            const GLfloat quad_vertices[] = {
                // Position (x,y)  // TexCoord (u,v)
                -1.0f, -1.0f,      0.0f, 0.0f,  // bottom-left
                 1.0f, -1.0f,      1.0f, 0.0f,  // bottom-right
                 1.0f,  1.0f,      1.0f, 1.0f,  // top-right
                -1.0f,  1.0f,      0.0f, 1.0f   // top-left
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
            
            // Position attribute
            GLint pos_loc = glGetAttribLocation(g_gpu_opt.fragment_program, "position");
            glVertexAttribPointer((GLuint)pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);
            glEnableVertexAttribArray((GLuint)pos_loc);
            
            // TexCoord attribute
            GLint tex_loc = glGetAttribLocation(g_gpu_opt.fragment_program, "texCoord");
            glVertexAttribPointer((GLuint)tex_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 
                                (void*)(2 * sizeof(GLfloat)));
            glEnableVertexAttribArray((GLuint)tex_loc);
            
            glBindVertexArray(0);
            
            // Create temporary resources
            glGenFramebuffers(1, &g_gpu_opt.temp_fbo);
            if (g_gpu_opt.temp_fbo == 0) {
                LOG_WARN("Failed to create temporary framebuffer for GPU optimization");
            }
        } else {
            glGetProgramInfoLog(g_gpu_opt.fragment_program, 512, NULL, infoLog);
            LOG_WARN("Fragment shader program linking failed: %s", infoLog);
            g_gpu_opt.active_path = RENDER_PATH_NONE;
        }
        
        // Clean up shader objects
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
    }
    
    // Create any additional resources needed
    if (g_gpu_opt.active_path != RENDER_PATH_NONE) {
        // Initialize common resources
        // ...
    }
    
    // TODO: Compute shader path needs additional fixes to work correctly:
    // 1. Check texture format compatibility with image load/store operations
    // 2. Ensure proper texture binding and access permissions
    // 3. Consider pre-creating texture storage with glTexStorage2D for image bindings
    // 4. Add more detailed error handling for compute shader operations
    // 5. Make sure driver supports the necessary features for compute shader image I/O
    
    return g_gpu_opt.active_path != RENDER_PATH_NONE;
}

// Zero-copy texture import from MPV using DMA-BUF
GLuint gpu_optimize_import_mpv_texture(mpv_render_context *ctx) {
    if (!g_gpu_opt.use_dmabuf) return 0;
    
    // This requires MPV to be configured with hwdec that produces DMA-BUF
    // For RPi4, we need hwdec=v4l2m2m-copy or drm-copy
    
    // Note: Direct texture extraction from MPV is complex and requires
    // custom MPV build with specific patches. For now, return 0.
    return 0;
}

// Process frame using GPU with minimal CPU involvement
bool gpu_optimize_process_frame(GLuint input_texture, GLuint output_texture, 
                             const float* keystone_matrix) {
    // Basic validation
    if (input_texture == 0 || output_texture == 0 || !keystone_matrix) {
        LOG_WARN("Invalid parameters in gpu_optimize_process_frame");
        return false;
    }
    
    // Performance timing
    perf_frame_start(&g_gpu_opt.perf);
    
    // Log every 100th frame for performance monitoring
    static int frame_counter = 0;
    bool log_this_frame = (frame_counter++ % 100 == 0);
    if (log_this_frame) {
        LOG_DEBUG("Processing frame %d with GPU optimization (path=%d)", 
                 frame_counter, g_gpu_opt.active_path);
    }
    
    // Verify textures are valid
    GLboolean is_texture_input, is_texture_output;
    
    is_texture_input = glIsTexture(input_texture);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR || !is_texture_input) {
        LOG_WARN("Input texture is invalid (id=%u): GL error 0x%x", input_texture, error);
        return false;
    }
    
    is_texture_output = glIsTexture(output_texture);
    error = glGetError();
    if (error != GL_NO_ERROR || !is_texture_output) {
        LOG_WARN("Output texture is invalid (id=%u): GL error 0x%x", output_texture, error);
        return false;
    }
    
    // Get texture dimensions
    GLint width = 0, height = 0;
    glBindTexture(GL_TEXTURE_2D, output_texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
    
    error = glGetError();
    if (error != GL_NO_ERROR || width <= 0 || height <= 0) {
        LOG_WARN("Error getting texture dimensions: 0x%x (width=%d, height=%d)", error, width, height);
        width = 1920;  // Fallback to standard size if dimension query fails
        height = 1080;
    }
    
    // Save current OpenGL state
    GLint last_fbo_int;
    GLuint last_fbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo_int);
    last_fbo = (GLuint)last_fbo_int;
    GLint last_viewport[4];
    glGetIntegerv(GL_VIEWPORT, last_viewport);
    
    // For now, just use the fragment shader path
    // We'll gradually implement and test other paths
    
    // Use appropriate rendering path
    if (g_gpu_opt.active_path == RENDER_PATH_COMPUTE) {
        // For now, fall back to fragment shader to ensure stability
        // Comment out the next two lines to enable compute shader when fully debugged
        LOG_INFO("Temporarily using fragment shader instead of compute shader (WIP)");
        g_gpu_opt.active_path = RENDER_PATH_FRAGMENT;
        
        /* TODO: Fix compute shader path - currently causes segfault
        // Use compute shader for processing
        if (g_gpu_opt.compute_program == 0) {
            LOG_WARN("Compute shader not initialized, falling back to fragment shader");
            g_gpu_opt.active_path = RENDER_PATH_FRAGMENT;
        } else {
            // Execute the compute shader path
            // Bind the compute shader program
            glUseProgram(g_gpu_opt.compute_program);
            error = glGetError();
            if (error != GL_NO_ERROR) {
                LOG_WARN("Error using compute program: 0x%x", error);
                glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
                glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
                return false;
            }
            
            // Set uniforms for the compute shader
            GLint matrix_loc = glGetUniformLocation(g_gpu_opt.compute_program, "keystoneMatrix");
            if (matrix_loc != -1) {
                glUniformMatrix3fv(matrix_loc, 1, GL_FALSE, keystone_matrix);
            } else {
                LOG_WARN("Could not find keystoneMatrix uniform in compute shader");
            }
            
            // Set texture size uniform
            GLint size_loc = glGetUniformLocation(g_gpu_opt.compute_program, "texSize");
            if (size_loc != -1) {
                glUniform2f(size_loc, (float)width, (float)height);
            } else {
                LOG_WARN("Could not find texSize uniform in compute shader");
            }
            
            // Bind input and output textures as image units
            glBindImageTexture(0, input_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
            glBindImageTexture(1, output_texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
            
            // Calculate dispatch size based on texture dimensions and work group size
            // Using 16x16 work group size as defined in the shader
            GLuint group_size_x = 16;
            GLuint group_size_y = 16;
            GLuint dispatch_x = ((GLuint)width + group_size_x - 1) / group_size_x;
            GLuint dispatch_y = ((GLuint)height + group_size_y - 1) / group_size_y;
            
            LOG_INFO("Dispatching compute shader with dimensions: %ux%u groups for %dx%d texture",
                   dispatch_x, dispatch_y, width, height);
            
            // Dispatch the compute shader
            glDispatchCompute(dispatch_x, dispatch_y, 1);
            
            // Memory barrier to ensure writes are visible
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            
            // Check for errors
            error = glGetError();
            if (error != GL_NO_ERROR) {
                LOG_WARN("Error in compute shader dispatch: 0x%x", error);
                g_gpu_opt.active_path = RENDER_PATH_FRAGMENT;
                LOG_INFO("Falling back to fragment shader");
            } else {
                // Reset OpenGL state and return success
                glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
                glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
                
                // End performance timing
                perf_frame_end(&g_gpu_opt.perf);
                return true;
            }
        }
        */
    }
    
    // Fall back to fragment shader if compute shader is not used
    if (g_gpu_opt.active_path == RENDER_PATH_FRAGMENT) {
        // Initialize fragment shader if it wasn't initialized yet
        if (g_gpu_opt.fragment_program == 0) {
            // Create and compile vertex shader
            GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
            glCompileShader(vertex_shader);
            
            // Create and compile fragment shader
            GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(fragment_shader, 1, &optimized_keystone_fragment_shader, NULL);
            glCompileShader(fragment_shader);
            
            // Check fragment shader compilation
            GLint success;
            GLchar infoLog[512];
            glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
            if (!success) {
                glGetShaderInfoLog(fragment_shader, 512, NULL, infoLog);
                LOG_WARN("Fragment shader compilation failed: %s", infoLog);
                glDeleteShader(vertex_shader);
                glDeleteShader(fragment_shader);
                return false;
            }
            
            // Create and link program
            g_gpu_opt.fragment_program = glCreateProgram();
            glAttachShader(g_gpu_opt.fragment_program, vertex_shader);
            glAttachShader(g_gpu_opt.fragment_program, fragment_shader);
            glLinkProgram(g_gpu_opt.fragment_program);
            
            // Check program linking
            glGetProgramiv(g_gpu_opt.fragment_program, GL_LINK_STATUS, &success);
            if (success) {
                LOG_INFO("Fragment shader program compiled successfully");
                
                // Create VAO and VBO for fullscreen quad
                glGenVertexArrays(1, &g_gpu_opt.vao);
                glBindVertexArray(g_gpu_opt.vao);
                
                glGenBuffers(1, &g_gpu_opt.vbo);
                glBindBuffer(GL_ARRAY_BUFFER, g_gpu_opt.vbo);
                
                // Define fullscreen quad vertices with texcoords
                const GLfloat quad_vertices[] = {
                    // Position (x,y)  // TexCoord (u,v)
                    -1.0f, -1.0f,      0.0f, 0.0f,  // bottom-left
                     1.0f, -1.0f,      1.0f, 0.0f,  // bottom-right
                     1.0f,  1.0f,      1.0f, 1.0f,  // top-right
                    -1.0f,  1.0f,      0.0f, 1.0f   // top-left
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
                
                // Position attribute
                GLint pos_loc = glGetAttribLocation(g_gpu_opt.fragment_program, "position");
                glVertexAttribPointer((GLuint)pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 0);
                glEnableVertexAttribArray((GLuint)pos_loc);
                
                // TexCoord attribute
                GLint tex_loc = glGetAttribLocation(g_gpu_opt.fragment_program, "texCoord");
                glVertexAttribPointer((GLuint)tex_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 
                                    (void*)(2 * sizeof(GLfloat)));
                glEnableVertexAttribArray((GLuint)tex_loc);
                
                glBindVertexArray(0);
                
                // Create temporary FBO if needed
                if (g_gpu_opt.temp_fbo == 0) {
                    glGenFramebuffers(1, &g_gpu_opt.temp_fbo);
                    if (g_gpu_opt.temp_fbo == 0) {
                        LOG_WARN("Failed to create temporary framebuffer for GPU optimization");
                        return false;
                    }
                }
                
                // Clean up shader objects
                glDeleteShader(vertex_shader);
                glDeleteShader(fragment_shader);
                
                LOG_INFO("Fragment shader resources initialized successfully for fallback");
            } else {
                glGetProgramInfoLog(g_gpu_opt.fragment_program, 512, NULL, infoLog);
                LOG_WARN("Fragment shader program linking failed: %s", infoLog);
                g_gpu_opt.active_path = RENDER_PATH_NONE;
                
                // Clean up shader objects
                glDeleteShader(vertex_shader);
                glDeleteShader(fragment_shader);
                return false;
            }
        }
    }
    
    // Use fragment shader with FBO
    if (g_gpu_opt.fragment_program == 0 || g_gpu_opt.temp_fbo == 0) {
        LOG_WARN("Fragment shader resources not initialized");
        return false;
    }
    
    // Render input texture to output texture through fragment shader
    glBindFramebuffer(GL_FRAMEBUFFER, g_gpu_opt.temp_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output_texture, 0);
    
    // Check framebuffer status
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_WARN("Framebuffer not complete: 0x%x", status);
        glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
        glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        return false;
    }
    
    // Setup viewport for rendering
    glViewport(0, 0, width, height);
    
    // Use the fragment shader program
    glUseProgram(g_gpu_opt.fragment_program);
    error = glGetError();
    if (error != GL_NO_ERROR) {
        LOG_WARN("Error using fragment program: 0x%x", error);
        glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
        glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        return false;
    }
    
    // Set uniforms
    GLint matrix_loc = glGetUniformLocation(g_gpu_opt.fragment_program, "keystoneMatrix");
    if (matrix_loc != -1) {
        glUniformMatrix3fv(matrix_loc, 1, GL_FALSE, keystone_matrix);
    }
    
    // Set the input texture
    GLint tex_loc = glGetUniformLocation(g_gpu_opt.fragment_program, "inputTexture");
    if (tex_loc != -1) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, input_texture);
        glUniform1i(tex_loc, 0);
    }
    
    // Draw fullscreen quad using VAO/VBO
    glBindVertexArray(g_gpu_opt.vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
    
    // Check for errors
    error = glGetError();
    if (error != GL_NO_ERROR) {
        LOG_WARN("Error during fragment shader rendering: 0x%x", error);
    }
    
    // Restore OpenGL state
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
    
    // Update performance metrics
    g_gpu_opt.perf.frames_processed++;
    perf_frame_end(&g_gpu_opt.perf);
    
    return (error == GL_NO_ERROR);
}

void gpu_optimize_cleanup(void) {
    // Print final performance stats
    if (g_gpu_opt.perf.frames_processed > 0) {
        double avg_time = g_gpu_opt.perf.total_gpu_time_ms / (double)g_gpu_opt.perf.frames_processed;
        double avg_fps = 1000.0 / (avg_time > 0 ? avg_time : 16.67);
        
        LOG_INFO("=== GPU Keystone Performance Summary ===");
        LOG_INFO("Rendering path: %d", g_gpu_opt.active_path);
        LOG_INFO("Frames processed: %lu", g_gpu_opt.perf.frames_processed);
        LOG_INFO("Average time: %.2f ms", avg_time);
        LOG_INFO("Min time: %.2f ms", g_gpu_opt.perf.min_frame_time);
        LOG_INFO("Max time: %.2f ms", g_gpu_opt.perf.max_frame_time);
        LOG_INFO("Average FPS: %.1f", avg_fps);
        
        // Detailed hardware info
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const char* version = (const char*)glGetString(GL_VERSION);
        if (renderer && version) {
            LOG_INFO("GPU: %s, OpenGL ES: %s", renderer, version);
        }
        
        if (g_gpu_opt.has_atomic_modesetting) {
            LOG_INFO("Atomic modesetting: Enabled");
        } else {
            LOG_INFO("Atomic modesetting: Disabled");
        }
        
        if (g_gpu_opt.use_dmabuf) {
            LOG_INFO("DMA-BUF zero copy: Used");
        } else {
            LOG_INFO("DMA-BUF zero copy: Not used");
        }
    }
    
    // Clean up based on active path
    switch (g_gpu_opt.active_path) {
        case RENDER_PATH_COMPUTE:
            // Clean up compute shader resources
            if (g_gpu_opt.compute_program) {
                glDeleteProgram(g_gpu_opt.compute_program);
                g_gpu_opt.compute_program = 0;
            }
            if (g_gpu_opt.compute_output_texture) {
                glDeleteTextures(1, &g_gpu_opt.compute_output_texture);
                g_gpu_opt.compute_output_texture = 0;
            }
            if (g_gpu_opt.ssbo) {
                glDeleteBuffers(1, &g_gpu_opt.ssbo);
                g_gpu_opt.ssbo = 0;
            }
            break;
            
        case RENDER_PATH_FRAGMENT:
            // Clean up fragment shader resources
            if (g_gpu_opt.fragment_program) {
                glDeleteProgram(g_gpu_opt.fragment_program);
                g_gpu_opt.fragment_program = 0;
            }
            if (g_gpu_opt.vao) {
                glDeleteVertexArrays(1, &g_gpu_opt.vao);
                g_gpu_opt.vao = 0;
            }
            if (g_gpu_opt.vbo) {
                glDeleteBuffers(1, &g_gpu_opt.vbo);
                g_gpu_opt.vbo = 0;
            }
            break;
            
        case RENDER_PATH_VULKAN:
            // Vulkan resources are cleaned up elsewhere
            break;
            
        default:
            break;
    }
    
    // Clean up common resources
    if (g_gpu_opt.temp_fbo) {
        glDeleteFramebuffers(1, &g_gpu_opt.temp_fbo);
        g_gpu_opt.temp_fbo = 0;
    }
    
    if (g_gpu_opt.temp_texture) {
        glDeleteTextures(1, &g_gpu_opt.temp_texture);
        g_gpu_opt.temp_texture = 0;
    }
    
    if (g_gpu_opt.egl_image) {
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = 
            (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        if (eglDestroyImageKHR) {
            eglDestroyImageKHR(eglGetCurrentDisplay(), g_gpu_opt.egl_image);
        } else {
            LOG_INFO("Warning: eglDestroyImageKHR not available");
        }
        g_gpu_opt.egl_image = NULL;
    }
    
    // Close any open file descriptors
    if (g_gpu_opt.dmabuf_fd >= 0) {
        close(g_gpu_opt.dmabuf_fd);
        g_gpu_opt.dmabuf_fd = -1;
    }
    
    if (g_gpu_opt.drm_fd >= 0) {
        close(g_gpu_opt.drm_fd);
        g_gpu_opt.drm_fd = -1;
    }
    
    // Reset the context
    memset(&g_gpu_opt, 0, sizeof(g_gpu_opt));
    g_gpu_opt.dmabuf_fd = -1;
    g_gpu_opt.drm_fd = -1;
}

bool gpu_optimize_is_supported(void) {
    // Check for GPU hardware acceleration capabilities
    
    // First, check if Vulkan is available
#ifdef VULKAN_ENABLED
    const char* env_vulkan = getenv("PICKLE_USE_VULKAN_GPU");
    if ((!env_vulkan || atoi(env_vulkan) != 0) && check_vulkan_support()) {
        LOG_INFO("GPU optimization supported via Vulkan");
        return true;
    }
#endif
    
    // Then check for OpenGL ES 3.1+ with compute shader support
    const char* version = (const char*)glGetString(GL_VERSION);
    if (version && (strstr(version, "OpenGL ES 3.1") || strstr(version, "OpenGL ES 3.2"))) {
        // Test compute shader capability
        GLint max_compute_work_groups = 0;
        glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_COUNT, &max_compute_work_groups);
        GLenum error = glGetError();
        
        if (error == GL_NO_ERROR && max_compute_work_groups > 0) {
            LOG_INFO("GPU optimization supported via OpenGL ES compute shaders");
            return true;
        }
        
        // Even without compute shader, fragment shader approach is supported
        LOG_INFO("GPU optimization supported via OpenGL ES fragment shaders");
        return true;
    }
    
    // Fall back to software rendering
    return false;
}