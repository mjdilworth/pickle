#include "compute_keystone.h"
#include "keystone.h"
#include "utils.h"
#include "shader.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl32.h>  // Using GLES 3.2 for glCopyImageSubData

// Define the compute shader source
const char *g_compute_shader_src = 
"#version 310 es\n"
"layout(local_size_x = 16, local_size_y = 16) in;\n"
"layout(binding = 0, rgba8) readonly uniform highp image2D inputImage;\n"
"layout(binding = 1, rgba8) writeonly uniform highp image2D outputImage;\n"
"\n"
"// Keystone correction parameters\n"
"uniform vec2 corners[4]; // TL, TR, BL, BR corners in normalized coordinates\n"
"uniform vec2 texSize;    // Size of the texture for normalization\n"
"\n"
"// Helper function to perform bilinear interpolation\n"
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
"// Helper function to determine if a point is inside a quadrilateral using cross products\n"
"bool isInsideQuad(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d) {\n"
"    // Check if the point is on the same side of all four edges\n"
"    vec2 ab = b - a;\n"
"    vec2 ap = p - a;\n"
"    float cross_ab_ap = ab.x * ap.y - ab.y * ap.x;\n"
"    \n"
"    vec2 bc = c - b;\n"
"    vec2 bp = p - b;\n"
"    float cross_bc_bp = bc.x * bp.y - bc.y * bp.x;\n"
"    \n"
"    vec2 cd = d - c;\n"
"    vec2 cp = p - c;\n"
"    float cross_cd_cp = cd.x * cp.y - cd.y * cp.x;\n"
"    \n"
"    vec2 da = a - d;\n"
"    vec2 dp = p - d;\n"
"    float cross_da_dp = da.x * dp.y - da.y * dp.x;\n"
"    \n"
"    // All cross products must have the same sign\n"
"    return (cross_ab_ap > 0.0 && cross_bc_bp > 0.0 && cross_cd_cp > 0.0 && cross_da_dp > 0.0) ||\n"
"           (cross_ab_ap < 0.0 && cross_bc_bp < 0.0 && cross_cd_cp < 0.0 && cross_da_dp < 0.0);\n"
"}\n"
"\n"
"// Compute the barycentric coordinates for a point in a quadrilateral\n"
"vec2 computeQuadTexCoord(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d) {\n"
"    // Compute the texture coordinates using bilinear interpolation\n"
"    // Convert point p to parametric coordinates (s,t)\n"
"    // where p = (1-s)(1-t)*a + s*(1-t)*b + s*t*c + (1-s)*t*d\n"
"    \n"
"    // This is a quadratic equation that we need to solve\n"
"    // We'll use an iterative approach for simplicity\n"
"    \n"
"    // Initialize s and t to a reasonable guess (center of quad)\n"
"    vec2 st = vec2(0.5, 0.5);\n"
"    \n"
"    // Perform a few iterations to refine the coordinates\n"
"    for (int i = 0; i < 4; i++) {\n"
"        float s = st.x;\n"
"        float t = st.y;\n"
"        \n"
"        // Compute the position using the current s,t\n"
"        vec2 pos = (1.0-s)*(1.0-t)*a + s*(1.0-t)*b + s*t*c + (1.0-s)*t*d;\n"
"        \n"
"        // Compute the error\n"
"        vec2 error = p - pos;\n"
"        \n"
"        // Compute the Jacobian (partial derivatives)\n"
"        vec2 ds = (1.0-t)*(b-a) + t*(c-d);\n"
"        vec2 dt = (1.0-s)*(d-a) + s*(c-b);\n"
"        \n"
"        // Compute determinant of Jacobian\n"
"        float det = ds.x*dt.y - ds.y*dt.x;\n"
"        \n"
"        // Update s and t (inverse Jacobian multiplied by error)\n"
"        if (abs(det) > 0.0001) {\n"
"            st += vec2(dt.y*error.x - dt.x*error.y, -ds.y*error.x + ds.x*error.y) / det;\n"
"            \n"
"            // Clamp s and t to [0,1]\n"
"            st = clamp(st, vec2(0.0), vec2(1.0));\n"
"        }\n"
"    }\n"
"    \n"
"    return st;\n"
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
"    // Check if this pixel is inside the keystone quad\n"
"    if (isInsideQuad(normalizedCoord, corners[0], corners[1], corners[3], corners[2])) {\n"
"        // Compute texture coordinates using projective mapping\n"
"        vec2 st = computeQuadTexCoord(normalizedCoord, corners[0], corners[1], corners[3], corners[2]);\n"
"        \n"
"        // Sample from the input image using bilinear interpolation\n"
"        vec4 color = bilinearSample(st);\n"
"        \n"
"        // Write to the output image\n"
"        imageStore(outputImage, outputCoord, color);\n"
"    } else {\n"
"        // Outside the keystone quad, write transparent black\n"
"        imageStore(outputImage, outputCoord, vec4(0.0, 0.0, 0.0, 0.0));\n"
"    }\n"
"}";

// Internal state for compute shader keystone
typedef struct {
    bool initialized;
    bool supported;
    GLuint compute_shader;
    GLuint compute_program;
    GLuint input_texture;   // Input texture
    GLuint output_texture;  // Output texture
    GLuint input_image;     // Input image binding
    GLuint output_image;    // Output image binding
    int width;              // Current width
    int height;             // Current height
    GLint corners_loc;      // Uniform location for corners
    GLint texSize_loc;      // Uniform location for texture size
} compute_keystone_state_t;

static compute_keystone_state_t g_compute_state = {0};

// Check if compute shaders are supported
bool compute_keystone_is_supported(void) {
    // Check if GLES 3.1 with compute shaders is supported
    const char *version = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    
    if (!version || !extensions || !gl_version) {
        LOG_ERROR("Failed to get GL version or extensions");
        return false;
    }
    
    // Check GL version first (need at least OpenGL ES 3.1)
    bool version_ok = false;
    if (strstr(gl_version, "OpenGL ES 3.1") || 
        strstr(gl_version, "OpenGL ES 3.2")) {
        version_ok = true;
    } else if (strstr(gl_version, "OpenGL ES")) {
        // Extract the version number
        float gl_ver = 0.0f;
        if (sscanf(gl_version, "OpenGL ES %f", &gl_ver) == 1) {
            version_ok = (gl_ver >= 3.1f);
        }
    }
    
    // Check for compute shader capabilities
    bool has_compute_shader = false;
    
    // Method 1: Check specific extensions
    if (strstr(extensions, "GL_ARB_compute_shader") != NULL ||
        strstr(extensions, "GL_EXT_compute_shader") != NULL ||
        strstr(extensions, "GL_ANDROID_extension_pack_es31a") != NULL) {
        has_compute_shader = true;
    }
    
    // Method 2: Try to use a compute shader capability directly
    GLint max_compute_work_group_count[3] = {0};
    GLint max_compute_work_group_size[3] = {0};
    GLint max_compute_work_group_invocations = 0;
    
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &max_compute_work_group_count[0]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &max_compute_work_group_size[0]);
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &max_compute_work_group_invocations);
    
    // If we got valid values, compute shaders are supported
    if (glGetError() == GL_NO_ERROR && 
        max_compute_work_group_count[0] > 0 &&
        max_compute_work_group_size[0] > 0 &&
        max_compute_work_group_invocations > 0) {
        has_compute_shader = true;
    }
    
    LOG_INFO("Compute shader support: GL version %s, GLSL version %s, Compute shaders: %s", 
             gl_version, version, has_compute_shader ? "yes" : "no");
    
    if (has_compute_shader) {
        LOG_INFO("Max compute work group count: [%d, %d, %d]", 
                max_compute_work_group_count[0], 
                max_compute_work_group_count[1], 
                max_compute_work_group_count[2]);
        LOG_INFO("Max compute work group size: [%d, %d, %d]", 
                max_compute_work_group_size[0], 
                max_compute_work_group_size[1], 
                max_compute_work_group_size[2]);
        LOG_INFO("Max compute work group invocations: %d", 
                max_compute_work_group_invocations);
    }
    
    return version_ok && has_compute_shader;
}

// Create textures for compute shader
static bool create_compute_textures(int width, int height) {
    // Clean up existing textures
    if (g_compute_state.input_texture) {
        glDeleteTextures(1, &g_compute_state.input_texture);
        g_compute_state.input_texture = 0;
    }
    
    if (g_compute_state.output_texture) {
        glDeleteTextures(1, &g_compute_state.output_texture);
        g_compute_state.output_texture = 0;
    }
    
    // Create input texture
    glGenTextures(1, &g_compute_state.input_texture);
    glBindTexture(GL_TEXTURE_2D, g_compute_state.input_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    
    // Create output texture
    glGenTextures(1, &g_compute_state.output_texture);
    glBindTexture(GL_TEXTURE_2D, g_compute_state.output_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    
    g_compute_state.width = width;
    g_compute_state.height = height;
    
    return true;
}

bool compute_keystone_init(void) {
    // Check for compute shader support
    if (!compute_keystone_is_supported()) {
        LOG_WARN("Compute shaders not supported on this platform");
        g_compute_state.supported = false;
        return false;
    }
    
    if (g_compute_state.initialized) {
        // Already initialized
        return true;
    }
    
    // Compile compute shader
    g_compute_state.compute_shader = glCreateShader(GL_COMPUTE_SHADER);
    if (!g_compute_state.compute_shader) {
        LOG_ERROR("Failed to create compute shader");
        return false;
    }
    
    const GLchar *sources[1] = { g_compute_shader_src };
    GLint lengths[1] = { (GLint)strlen(g_compute_shader_src) };
    glShaderSource(g_compute_state.compute_shader, 1, sources, lengths);
    glCompileShader(g_compute_state.compute_shader);
    
    // Check for compilation errors
    GLint success;
    glGetShaderiv(g_compute_state.compute_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetShaderInfoLog(g_compute_state.compute_shader, sizeof(infoLog), NULL, infoLog);
        LOG_ERROR("Compute shader compilation failed: %s", infoLog);
        glDeleteShader(g_compute_state.compute_shader);
        g_compute_state.compute_shader = 0;
        return false;
    }
    
    // Create compute program
    g_compute_state.compute_program = glCreateProgram();
    if (!g_compute_state.compute_program) {
        LOG_ERROR("Failed to create compute program");
        glDeleteShader(g_compute_state.compute_shader);
        g_compute_state.compute_shader = 0;
        return false;
    }
    
    glAttachShader(g_compute_state.compute_program, g_compute_state.compute_shader);
    glLinkProgram(g_compute_state.compute_program);
    
    // Check for linking errors
    glGetProgramiv(g_compute_state.compute_program, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[512];
        glGetProgramInfoLog(g_compute_state.compute_program, sizeof(infoLog), NULL, infoLog);
        LOG_ERROR("Compute program linking failed: %s", infoLog);
        glDeleteProgram(g_compute_state.compute_program);
        glDeleteShader(g_compute_state.compute_shader);
        g_compute_state.compute_program = 0;
        g_compute_state.compute_shader = 0;
        return false;
    }
    
    // Get uniform locations
    g_compute_state.corners_loc = glGetUniformLocation(g_compute_state.compute_program, "corners");
    g_compute_state.texSize_loc = glGetUniformLocation(g_compute_state.compute_program, "texSize");
    
    g_compute_state.initialized = true;
    g_compute_state.supported = true;
    
    LOG_INFO("Compute shader keystone initialized successfully");
    
    return true;
}

bool compute_keystone_apply(keystone_t *keystone, GLuint source_texture, int width, int height) {
    if (!g_compute_state.initialized && !compute_keystone_init()) {
        return false;
    }
    
    if (!g_compute_state.supported) {
        return false;
    }
    
    if (!keystone->enabled) {
        return false;
    }
    
    // Check if we need to resize the textures
    if (g_compute_state.width != width || g_compute_state.height != height) {
        if (!create_compute_textures(width, height)) {
            LOG_ERROR("Failed to create compute textures");
            return false;
        }
    }
    
    // Copy source texture to input texture
    glCopyImageSubData(
        source_texture, GL_TEXTURE_2D, 0, 0, 0, 0,
        g_compute_state.input_texture, GL_TEXTURE_2D, 0, 0, 0, 0,
        width, height, 1
    );
    
    // Use compute shader program
    glUseProgram(g_compute_state.compute_program);
    
    // Bind input and output textures as image units
    glBindImageTexture(0, g_compute_state.input_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    glBindImageTexture(1, g_compute_state.output_texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    
    // Set uniform values
    if (g_compute_state.corners_loc >= 0) {
        // Pass keystone corners to compute shader
        // The corners array in the shader is in the order: TL, TR, BL, BR
        glUniform2f(g_compute_state.corners_loc + 0, keystone->points[0][0], keystone->points[0][1]); // TL
        glUniform2f(g_compute_state.corners_loc + 1, keystone->points[1][0], keystone->points[1][1]); // TR
        glUniform2f(g_compute_state.corners_loc + 2, keystone->points[2][0], keystone->points[2][1]); // BL
        glUniform2f(g_compute_state.corners_loc + 3, keystone->points[3][0], keystone->points[3][1]); // BR
    }
    
    // Set texture size uniform
    if (g_compute_state.texSize_loc >= 0) {
        glUniform2f(g_compute_state.texSize_loc, (float)width, (float)height);
    }
    
    // Dispatch compute shader
    unsigned int group_x = ((unsigned int)width + 15U) / 16U;
    unsigned int group_y = ((unsigned int)height + 15U) / 16U;
    glDispatchCompute(group_x, group_y, 1);
    
    // Wait for compute shader to finish
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    
    // Render the output texture to the screen
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Use a basic shader to render the output texture
    GLuint prog = get_basic_shader_program();
    glUseProgram(prog);
    
    // Draw a full-screen quad
    GLfloat vertices[] = {
        -1.0f, -1.0f,  // Bottom left
         1.0f, -1.0f,  // Bottom right
         1.0f,  1.0f,  // Top right
        -1.0f,  1.0f   // Top left
    };
    
    GLfloat texcoords[] = {
        0.0f, 0.0f,  // Bottom left
        1.0f, 0.0f,  // Bottom right
        1.0f, 1.0f,  // Top right
        0.0f, 1.0f   // Top left
    };
    
    GLuint indices[] = {
        0, 1, 2,  // First triangle
        0, 2, 3   // Second triangle
    };
    
    GLint pos_attrib = glGetAttribLocation(prog, "position");
    GLint tex_attrib = glGetAttribLocation(prog, "texcoord");
    GLint tex_uniform = glGetUniformLocation(prog, "texture");
    
    // Bind the output texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_compute_state.output_texture);
    glUniform1i(tex_uniform, 0);  // Texture unit 0
    
    // Set up vertex attributes
    if (pos_attrib >= 0) {
        glEnableVertexAttribArray((GLuint)pos_attrib);
        glVertexAttribPointer((GLuint)pos_attrib, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    }
    
    if (tex_attrib >= 0) {
        glEnableVertexAttribArray((GLuint)tex_attrib);
        glVertexAttribPointer((GLuint)tex_attrib, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    }
    
    // Draw the quad
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, indices);
    
    // Clean up
    if (pos_attrib >= 0) {
        glDisableVertexAttribArray((GLuint)pos_attrib);
    }
    if (tex_attrib >= 0) {
        glDisableVertexAttribArray((GLuint)tex_attrib);
    }
    
    // Convert keystone points to corners array
    float corners[8];
    for (int i = 0; i < 4; i++) {
        corners[i*2] = keystone->points[i][0];
        corners[i*2+1] = keystone->points[i][1];
    }
    
    // Draw border and corner markers if enabled
    if (keystone->border_visible) {
        draw_keystone_border(corners);
    }
    
    if (keystone->corner_markers) {
        draw_keystone_corner_markers(corners, keystone->selected_corner);
    }
    
    return true;
}

bool compute_keystone_update(keystone_t *keystone) {
    (void)keystone; // Unused parameter
    
    // Nothing to update, since we set the parameters in compute_keystone_apply
    return g_compute_state.initialized && g_compute_state.supported;
}

void compute_keystone_cleanup(void) {
    if (g_compute_state.initialized) {
        // Delete compute shader resources
        if (g_compute_state.compute_program) {
            glDeleteProgram(g_compute_state.compute_program);
            g_compute_state.compute_program = 0;
        }
        
        if (g_compute_state.compute_shader) {
            glDeleteShader(g_compute_state.compute_shader);
            g_compute_state.compute_shader = 0;
        }
        
        // Delete textures
        if (g_compute_state.input_texture) {
            glDeleteTextures(1, &g_compute_state.input_texture);
            g_compute_state.input_texture = 0;
        }
        
        if (g_compute_state.output_texture) {
            glDeleteTextures(1, &g_compute_state.output_texture);
            g_compute_state.output_texture = 0;
        }
        
        g_compute_state.initialized = false;
    }
}

GLuint compute_keystone_get_output_texture(void) {
    return g_compute_state.output_texture;
}
