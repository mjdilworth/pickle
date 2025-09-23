#ifndef PICKLE_HVS_KEYSTONE_H
#define PICKLE_HVS_KEYSTONE_H

#include <stdbool.h>
#include <stdint.h>
#include "keystone.h"

// Include DispmanX definitions when enabled
#if defined(DISPMANX_ENABLED)
#include <bcm_host.h>
#endif

/**
 * Check if HVS keystone transformation is supported on this platform
 * 
 * This function checks if the hardware supports HVS keystone transformation
 * by verifying that we're running on Raspberry Pi hardware with DispmanX.
 * 
 * @return true if supported, false otherwise
 */
bool hvs_keystone_is_supported(void);

/**
 * Initialize the HVS keystone transformation
 * 
 * This function initializes the DispmanX API and prepares it for use with
 * keystone correction. It must be called before any other HVS keystone functions.
 * 
 * @return true if initialization succeeded, false otherwise
 */
bool hvs_keystone_init(void);

/**
 * Apply the keystone transformation to the current display using HVS
 * 
 * This function applies a keystone transformation to the display using the
 * Hardware Video Scaler (HVS) through the DispmanX API. It maps a rectangular
 * source to a quadrilateral destination defined by the keystone parameters.
 * 
 * @param keystone Pointer to the keystone configuration to apply
 * @param display_width The width of the display
 * @param display_height The height of the display
 * @return true if transformation succeeded, false otherwise
 */
bool hvs_keystone_apply(keystone_t *keystone, int display_width, int display_height);

/**
 * Update the keystone transformation parameters
 * 
 * This function updates an existing keystone transformation with new parameters.
 * It's more efficient than applying a new transformation as it only updates
 * the destination quadrilateral.
 * 
 * @param keystone Pointer to the updated keystone configuration
 * @return true if update succeeded, false otherwise
 */
bool hvs_keystone_update(keystone_t *keystone);

/**
 * Clean up HVS keystone transformation resources
 * 
 * This function releases all resources associated with the HVS keystone
 * transformation. It should be called when the keystone correction is no
 * longer needed.
 */
void hvs_keystone_cleanup(void);

#if defined(DISPMANX_ENABLED)
/**
 * Create a DispmanX resource from a buffer
 * 
 * Helper function to create a DispmanX resource from an RGBA buffer,
 * useful for applying keystone to a specific framebuffer.
 * 
 * @param buffer The source buffer (RGBA)
 * @param width Width of the buffer
 * @param height Height of the buffer
 * @param stride Stride of the buffer in bytes
 * @return The resource handle or DISPMANX_NO_HANDLE on failure
 */
DISPMANX_RESOURCE_HANDLE_T hvs_keystone_create_resource(void *buffer, int width, int height, int stride);
#endif

#endif // PICKLE_HVS_KEYSTONE_H