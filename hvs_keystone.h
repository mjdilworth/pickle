#ifndef PICKLE_HVS_KEYSTONE_H
#define PICKLE_HVS_KEYSTONE_H

#include <stdbool.h>
#include <stdint.h>
#include "keystone.h"

/**
 * Check if HVS keystone transformation is supported on this platform
 * 
 * @return true if supported, false otherwise
 */
bool hvs_keystone_is_supported(void);

/**
 * Initialize the HVS keystone transformation
 * 
 * @return true if initialization succeeded, false otherwise
 */
bool hvs_keystone_init(void);

/**
 * Apply the keystone transformation to the current display using HVS
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
 * @param keystone Pointer to the updated keystone configuration
 * @return true if update succeeded, false otherwise
 */
bool hvs_keystone_update(keystone_t *keystone);

/**
 * Clean up HVS keystone transformation resources
 */
void hvs_keystone_cleanup(void);

#endif // PICKLE_HVS_KEYSTONE_H