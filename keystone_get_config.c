#include "keystone.h"

// Reference to the global keystone configuration defined in keystone.c
extern keystone_t g_keystone;

// Get the current keystone configuration
keystone_t *keystone_get_config(void) {
    return &g_keystone;
}