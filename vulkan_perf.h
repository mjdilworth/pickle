#ifndef PICKLE_VULKAN_PERF_H
#define PICKLE_VULKAN_PERF_H

#include "vulkan.h"
#include "keystone.h"

// Function to log GPU hardware details
void log_vulkan_hardware_info(vulkan_ctx_t *ctx);

// Function to measure keystone correction performance
double measure_keystone_performance(vulkan_ctx_t *ctx, VkImage source_image, 
                                   const keystone_t *keystone);

// Function to log compute shader performance
void log_keystone_performance(vulkan_ctx_t *ctx, VkImage source_image, 
                             const keystone_t *keystone);

#endif // PICKLE_VULKAN_PERF_H