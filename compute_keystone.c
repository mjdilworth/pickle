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
"\n"
"// Helper function to determine if a point is inside a quadrilateral\n"
"bool isInsideQuad(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d) {\n"
"    vec2 ab = b - a;\n"
"    vec2 bc = c - b;\n"
"    vec2 cd = d - c;\n"
"    vec2 da = a - d;\n"
"    \n"
"    vec2 ap = p - a;\n"
"    vec2 bp = p - b;\n"
"    vec2 cp = p - c;\n"
"    vec2 dp = p - d;\n"
"    \n"
"    float abCross = ab.x * ap.y - ab.y * ap.x;\n"
"    float bcCross = bc.x * bp.y - bc.y * bp.x;\n"
"    float cdCross = cd.x * cp.y - cd.y * cp.x;\n"
"    float daCross = da.x * dp.y - da.y * dp.x;\n"
"    \n"
"    return (abCross >= 0.0 && bcCross >= 0.0 && cdCross >= 0.0 && daCross >= 0.0) || \n"
"           (abCross <= 0.0 && bcCross <= 0.0 && cdCross <= 0.0 && daCross <= 0.0);\n"
"}\n"
"\n"
"// Bilinear interpolation\n"
"vec4 bilinearInterpolation(vec2 uv, ivec2 textureSize) {\n"
"    // Convert to pixel coordinates\n"
"    vec2 pixel = uv * vec2(textureSize);\n"
"    \n"
"    // Get the four surrounding pixels\n"
"    ivec2 pixel00 = ivec2(floor(pixel));\n"
"    ivec2 pixel10 = ivec2(pixel00.x + 1, pixel00.y);\n"
"    ivec2 pixel01 = ivec2(pixel00.x, pixel00.y + 1);\n"
"    ivec2 pixel11 = ivec2(pixel00.x + 1, pixel00.y + 1);\n"
"    \n"
"    // Clamp to valid texture coordinates\n"
"    pixel00 = clamp(pixel00, ivec2(0), textureSize - ivec2(1));\n"
"    pixel10 = clamp(pixel10, ivec2(0), textureSize - ivec2(1));\n"
"    pixel01 = clamp(pixel01, ivec2(0), textureSize - ivec2(1));\n"
"    pixel11 = clamp(pixel11, ivec2(0), textureSize - ivec2(1));\n"
"    \n"
"    // Get the interpolation factors\n"
"    vec2 f = fract(pixel);\n"
"    \n"
"    // Sample the four pixels\n"
"    vec4 s00 = imageLoad(inputImage, pixel00);\n"
"    vec4 s10 = imageLoad(inputImage, pixel10);\n"
"    vec4 s01 = imageLoad(inputImage, pixel01);\n"
"    vec4 s11 = imageLoad(inputImage, pixel11);\n"
"    \n"
"    // Bilinear interpolation\n"
"    vec4 sx0 = mix(s00, s10, f.x);\n"
"    vec4 sx1 = mix(s01, s11, f.x);\n"
"    return mix(sx0, sx1, f.y);\n"
"}\n"
"\n"
"// Cross product of 2D vectors\n"
"float cross2d(vec2 a, vec2 b) {\n"
"    return a.x * b.y - a.y * b.x;\n"
"}\n"
"\n"
"// Find barycentric coordinates for a point inside a quadrilateral\n"
"vec2 barycentricQuad(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d) {\n"
"    // Divide the quad into two triangles: abc and acd\n"
"    // Check which triangle the point is in\n"
"    vec2 ab = b - a;\n"
"    vec2 ac = c - a;\n"
"    vec2 ap = p - a;\n"
"    \n"
"    // Cross product to determine which side of diagonal p is on\n"
"    float crossABC = cross2d(ab, ac);\n"
"    float crossABP = cross2d(ab, ap);\n"
"    float crossACP = cross2d(ac, ap);\n"
"    \n"
"    // Barycentric coordinates for triangle abc\n"
"    float u = crossACP / crossABC;\n"
"    float v = crossABP / crossABC;\n"
"    \n"
"    // Normalize the coordinates\n"
"    float s = u + v;\n"
"    if (s > 1.0) {\n"
"        u /= s;\n"
"        v /= s;\n"
"    }\n"
"    \n"
"    return vec2(u, v);\n"
"}\n"
"\n"
"// Inverse mapping for keystone correction\n"
"vec2 inverseKeystone(vec2 pos, vec2 tl, vec2 tr, vec2 bl, vec2 br) {\n"
"    // Check if point is inside the keystone quad\n"
"    if (!isInsideQuad(pos, tl, tr, br, bl)) {\n"
"        return vec2(-1.0, -1.0); // Outside the quad\n"
"    }\n"
"    \n"
"    // Get barycentric coordinates\n"
"    vec2 uv = barycentricQuad(pos, tl, tr, br, bl);\n"
"    \n"
"    // Map to original texture space (0-1)\n"
"    return vec2(uv.x, uv.y);\n"
"}\n"
"\n"
"void main() {\n"
"    // Get the current pixel coordinates\n"
"    ivec2 texelCoord = ivec2(gl_GlobalInvocationID.xy);\n"
"    ivec2 outputSize = imageSize(outputImage);\n"
"    ivec2 inputSize = imageSize(inputImage);\n"
"    \n"
"    // Skip if outside the output image\n"
"    if (texelCoord.x >= outputSize.x || texelCoord.y >= outputSize.y) {\n"
"        return;\n"
"    }\n"
"    \n"
"    // Normalize coordinates to 0-1 range\n"
"    vec2 normalizedCoord = vec2(texelCoord) / vec2(outputSize);\n"
"    \n"
"    // Get the corners\n"
"    vec2 tl = corners[0];\n"
"    vec2 tr = corners[1];\n"
"    vec2 bl = corners[2];\n"
"    vec2 br = corners[3];\n"
"    \n"
"    // Compute the inverse mapping\n"
"    vec2 sourceCoord = inverseKeystone(normalizedCoord, tl, tr, bl, br);\n"
"    \n"
"    // If outside the quad, set to transparent black\n"
"    if (sourceCoord.x < 0.0 || sourceCoord.y < 0.0) {\n"
"        imageStore(outputImage, texelCoord, vec4(0.0, 0.0, 0.0, 0.0));\n"
"        return;\n"
"    }\n"
"    \n"
"    // Sample the input image with bilinear interpolation\n"
"    vec4 color = bilinearInterpolation(sourceCoord, inputSize);\n"
"    \n"
"    // Store the result in the output image\n"
"    imageStore(outputImage, texelCoord, color);\n"
"}\n";

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
} compute_keystone_state_t;

static compute_keystone_state_t g_compute_state = {0};

// Check if compute shaders are supported
bool compute_keystone_is_supported(void) {
    // Check if GLES 3.1 with compute shaders is supported
    const char *version = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
    
    if (!version || !extensions) {
        LOG_ERROR("Failed to get GL version or extensions");
        return false;
    }
    
    // Check for GLSL ES 3.10 or higher
    bool version_ok = strstr(version, "3.10") != NULL || 
                      strstr(version, "3.20") != NULL ||
                      strstr(version, "3.") != NULL;
    
    // Check for compute shader extension
    bool has_compute_shader = strstr(extensions, "GL_ARB_compute_shader") != NULL ||
                             strstr(extensions, "GL_EXT_compute_shader") != NULL;
    
    LOG_INFO("Compute shader support: GLSL version %s, Compute shaders: %s", 
             version, has_compute_shader ? "yes" : "no");
    
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
    // No persistent state to update, just return success
    return g_compute_state.initialized && g_compute_state.supported;
}

void compute_keystone_cleanup(void) {
    if (g_compute_state.initialized) {
        // Clean up OpenGL resources
        if (g_compute_state.compute_program) {
            glDeleteProgram(g_compute_state.compute_program);
            g_compute_state.compute_program = 0;
        }
        
        if (g_compute_state.compute_shader) {
            glDeleteShader(g_compute_state.compute_shader);
            g_compute_state.compute_shader = 0;
        }
        
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