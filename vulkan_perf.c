#include "vulkan.h"
#include "utils.h"
#include "log.h"
#include "keystone.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>  // For performance measurements

// Function to log GPU hardware details
void log_vulkan_hardware_info(vulkan_ctx_t *ctx) {
    if (!ctx || !ctx->initialized) {
        LOG_ERROR("Cannot log hardware info - Vulkan context not initialized");
        return;
    }

    LOG_INFO("=== VULKAN HARDWARE INFORMATION ===");
    LOG_INFO("GPU: %s", ctx->device_properties.deviceName);
    LOG_INFO("Vendor ID: 0x%x", ctx->device_properties.vendorID);
    LOG_INFO("Device ID: 0x%x", ctx->device_properties.deviceID);
    LOG_INFO("Driver version: %u.%u.%u", 
             VK_VERSION_MAJOR(ctx->device_properties.driverVersion),
             VK_VERSION_MINOR(ctx->device_properties.driverVersion),
             VK_VERSION_PATCH(ctx->device_properties.driverVersion));
    LOG_INFO("API version: %u.%u.%u", 
             VK_VERSION_MAJOR(ctx->device_properties.apiVersion),
             VK_VERSION_MINOR(ctx->device_properties.apiVersion),
             VK_VERSION_PATCH(ctx->device_properties.apiVersion));
    
    if (ctx->compute.supported) {
        LOG_INFO("Compute shader support: YES");
    } else {
        LOG_INFO("Compute shader support: NO");
    }
    
    LOG_INFO("===================================");
}

// Function to measure keystone correction performance
double measure_keystone_performance(vulkan_ctx_t *ctx, VkImage source_image, 
                                   const keystone_t *keystone) {
    if (!ctx || !ctx->initialized || !ctx->compute.initialized) {
        LOG_ERROR("Cannot measure performance - Vulkan compute not initialized");
        return -1.0;
    }
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    // Apply keystone correction
    vulkan_compute_keystone_apply(ctx, source_image, keystone);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    // Calculate time in milliseconds
    double elapsed = (end.tv_sec - start.tv_sec) * 1000.0;
    elapsed += (end.tv_nsec - start.tv_nsec) / 1000000.0;
    
    return elapsed;
}

// Function to log compute shader performance
void log_keystone_performance(vulkan_ctx_t *ctx, VkImage source_image, 
                             const keystone_t *keystone) {
    static int frame_count = 0;
    static double total_time = 0.0;
    static double min_time = 999999.0;
    static double max_time = 0.0;
    
    // Only measure every 60 frames to avoid overhead
    if (++frame_count % 60 != 0) {
        return;
    }
    
    double elapsed = measure_keystone_performance(ctx, source_image, keystone);
    if (elapsed < 0) {
        return;
    }
    
    total_time += elapsed;
    if (elapsed < min_time) min_time = elapsed;
    if (elapsed > max_time) max_time = elapsed;
    
    double avg_time = total_time / (frame_count / 60);
    
    LOG_INFO("Vulkan compute keystone performance: %.2f ms (avg: %.2f, min: %.2f, max: %.2f)",
             elapsed, avg_time, min_time, max_time);
}