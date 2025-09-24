#ifndef GPU_OPTIMIZE_KEYSTONE_H
#define GPU_OPTIMIZE_KEYSTONE_H

#include <stdbool.h>
#include <stdint.h>
#include <GLES3/gl31.h>
#include <mpv/render.h>

/**
 * Initialize the GPU-optimized keystone processing
 * 
 * @return true if initialization was successful, false otherwise
 */
bool gpu_optimize_init(void);

/**
 * Import a texture from MPV using zero-copy when possible
 * 
 * @param ctx The MPV render context
 * @return A GL texture ID or 0 on failure
 */
GLuint gpu_optimize_import_mpv_texture(mpv_render_context *ctx);

/**
 * Process a frame using GPU with minimal CPU involvement
 * 
 * @param input_texture The input GL texture ID
 * @param output_texture The output GL texture ID
 * @param keystone_matrix The 3x3 keystone transformation matrix (column-major)
 * @return true if processing was successful, false otherwise
 */
bool gpu_optimize_process_frame(GLuint input_texture, GLuint output_texture, 
                               const float* keystone_matrix);

/**
 * Clean up GPU optimization resources
 */
void gpu_optimize_cleanup(void);

/**
 * Check if GPU optimization is supported on the current system
 * 
 * @return true if supported, false otherwise
 */
bool gpu_optimize_is_supported(void);

#endif /* GPU_OPTIMIZE_KEYSTONE_H */