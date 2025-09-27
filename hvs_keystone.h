#ifndef PICKLE_HVS_KEYSTONE_H
#define PICKLE_HVS_KEYSTONE_H

#include <stdbool.h>
#include <stdint.h>
#include "keystone.h"

// DispmanX has been removed - no longer supported

/**
 * Check if HVS keystone transformation is supported on this platform
 * 
 * This function always returns false since DispmanX has been removed.
 * Hardware keystone correction is no longer available.
 * 
 * @return false (no longer supported)
 */
bool hvs_keystone_is_supported(void);

/**
 * Initialize the HVS keystone transformation
 * 
 * This function now always returns false since DispmanX has been removed.
 * Hardware keystone correction is no longer available.
 * 
 * @return false (no longer supported)
 */
bool hvs_keystone_init(void);

/**
 * Apply the keystone transformation to the current display using HVS
 * 
 * This function now always returns false since DispmanX has been removed.
 * Hardware keystone correction is no longer available.
 * 
 * @param keystone Unused parameter
 * @param display_width Unused parameter  
 * @param display_height Unused parameter
 * @return false (no longer supported)
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

// DispmanX-specific functions have been removed

#endif // PICKLE_HVS_KEYSTONE_H