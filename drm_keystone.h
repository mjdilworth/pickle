#ifndef PICKLE_DRM_KEYSTONE_H
#define PICKLE_DRM_KEYSTONE_H

#include <stdbool.h>
#include <stdint.h>
#include "keystone.h"
#include "drm.h"

/**
 * Check if DRM/KMS keystone transformation is supported on this platform
 * 
 * This function checks if the hardware supports keystone transformation
 * by verifying that we're running on hardware with DRM/KMS and appropriate planes.
 * 
 * @return true if supported, false otherwise
 */
bool drm_keystone_is_supported(void);

/**
 * Initialize the DRM/KMS keystone transformation
 * 
 * This function initializes the DRM/KMS API and prepares it for use with
 * keystone correction. It must be called before any other DRM keystone functions.
 * 
 * @return true if initialization succeeded, false otherwise
 */
bool drm_keystone_init(void);

/**
 * Apply the keystone transformation to the current display using DRM/KMS
 * 
 * This function applies a keystone transformation to the display using the
 * DRM/KMS API. It maps a rectangular source to a quadrilateral destination
 * defined by the keystone parameters.
 * 
 * @param keystone Pointer to the keystone configuration to apply
 * @param display_width The width of the display
 * @param display_height The height of the display
 * @return true if transformation succeeded, false otherwise
 */
bool drm_keystone_apply(keystone_t *keystone, int display_width, int display_height);

/**
 * Update the keystone transformation parameters
 * 
 * This function updates an existing keystone transformation with new parameters.
 * It should be called whenever the keystone parameters change.
 * 
 * @param keystone Pointer to the keystone configuration to apply
 * @return true if update succeeded, false otherwise
 */
bool drm_keystone_update(keystone_t *keystone);

/**
 * Set the source content for the keystone transformation
 * 
 * This function sets the source content (e.g., video frame) for the keystone
 * transformation. It should be called whenever the source content changes.
 * 
 * @param buffer Pointer to the source buffer
 * @param width Width of the source buffer
 * @param height Height of the source buffer
 * @param stride Stride of the source buffer in bytes
 * @param format Pixel format of the source buffer (DRM format)
 * @return true if successful, false otherwise
 */
bool drm_keystone_set_source(void *buffer, int width, int height, int stride, uint32_t format);

/**
 * Display a frame using the keystone transformation
 * 
 * This function displays a frame with the current keystone transformation applied.
 * It should be called for each frame to be displayed.
 * 
 * @param buffer Pointer to the frame buffer
 * @param width Width of the frame
 * @param height Height of the frame
 * @param stride Stride of the frame in bytes
 * @param format Pixel format of the frame (DRM format)
 * @return true if successful, false otherwise
 */
bool drm_keystone_display_frame(void *buffer, int width, int height, int stride, uint32_t format);

/**
 * Clean up the DRM/KMS keystone transformation
 * 
 * This function cleans up resources used by the DRM/KMS keystone transformation.
 * It should be called when the keystone correction is no longer needed.
 */
void drm_keystone_cleanup(void);

/**
 * Checks if DRM/KMS keystone transformation is active
 * 
 * @return true if active, false otherwise
 */
bool drm_keystone_is_active(void);

#endif /* PICKLE_DRM_KEYSTONE_H */