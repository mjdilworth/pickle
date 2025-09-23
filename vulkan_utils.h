#ifndef PICKLE_VULKAN_UTILS_H
#define PICKLE_VULKAN_UTILS_H

#include <vulkan/vulkan.h>

// Utility functions for Vulkan
VkResult create_shader_module(VkDevice device, const char* filename, VkShaderModule* shader_module);

#endif // PICKLE_VULKAN_UTILS_H