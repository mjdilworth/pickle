#include "hvs_keystone.h"
#include "log.h"

/**
 * HVS Keystone Implementation - Stub version
 * 
 * This is now a stub implementation since DispmanX has been removed.
 * All functions return false or do nothing, indicating that hardware
 * keystone correction is not available.
 */

bool hvs_keystone_is_supported(void) {
    // DispmanX has been removed, so HVS keystone is no longer supported
    return false;
}

bool hvs_keystone_init(void) {
    LOG_WARN("HVS keystone not supported - DispmanX has been removed");
    return false;
}

bool hvs_keystone_apply(keystone_t *keystone, int display_width, int display_height) {
    (void)keystone;      // unused parameter
    (void)display_width; // unused parameter
    (void)display_height;// unused parameter
    
    LOG_WARN("HVS keystone apply not supported - DispmanX has been removed");
    return false;
}

bool hvs_keystone_update(keystone_t *keystone) {
    (void)keystone; // unused parameter
    
    LOG_WARN("HVS keystone update not supported - DispmanX has been removed");
    return false;
}

void hvs_keystone_cleanup(void) {
    // Nothing to clean up in stub implementation
}
