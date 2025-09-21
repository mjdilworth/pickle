#ifndef PICKLE_COMPUTE_KEYSTONE_H
#define PICKLE_COMPUTE_KEYSTONE_H

#include "keystone.h"
#include <stdbool.h>
#include <GLES3/gl31.h>  // GLES 3.1 is required for compute shaders

/**
 * Check if compute shader-based keystone is supported on this platform
 * 
 * @return true if supported, false otherwise
 */
bool compute_keystone_is_supported(void);

/**
 * Initialize compute shader-based keystone
 * 
 * @return true if initialization was successful, false otherwise
 */
bool compute_keystone_init(void);

/**
 * Apply compute shader-based keystone transformation
 * 
 * @param keystone The keystone parameters to apply
 * @param source_texture Source texture to apply keystone to
 * @param width The width of the display
 * @param height The height of the display
 * @return true if successful, false otherwise
 */
bool compute_keystone_apply(keystone_t *keystone, GLuint source_texture, int width, int height);

/**
 * Update compute shader-based keystone parameters
 * 
 * @param keystone The keystone parameters to apply
 * @return true if successful, false otherwise
 */
bool compute_keystone_update(keystone_t *keystone);

/**
 * Clean up compute shader-based keystone resources
 */
void compute_keystone_cleanup(void);

/**
 * Get the output texture after compute shader processing
 * 
 * @return The output texture handle
 */
GLuint compute_keystone_get_output_texture(void);

#endif // PICKLE_COMPUTE_KEYSTONE_H