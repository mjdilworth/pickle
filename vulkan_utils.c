#include "vulkan.h"
#include "utils.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>  // For dlopen and dlclose

// Read a SPIR-V shader file
static unsigned char* read_spv_file(const char* filename, size_t* size) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        LOG_ERROR("Failed to open SPIR-V file: %s", filename);
        return NULL;
    }
    
    fseek(file, 0, SEEK_END);
    size_t length = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    unsigned char* buffer = malloc(length);
    if (!buffer) {
        LOG_ERROR("Failed to allocate memory for SPIR-V file");
        fclose(file);
        return NULL;
    }
    
    size_t read_length = fread(buffer, 1, length, file);
    fclose(file);
    
    if (read_length != length) {
        LOG_ERROR("Failed to read SPIR-V file");
        free(buffer);
        return NULL;
    }
    
    *size = length;
    return buffer;
}

// Create a shader module from a SPIR-V file
VkResult create_shader_module(VkDevice device, const char* filename, VkShaderModule* shader_module) {
    size_t size;
    unsigned char* code = read_spv_file(filename, &size);
    if (!code) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = (const uint32_t*)code
    };
    
    VkResult result = vkCreateShaderModule(device, &create_info, NULL, shader_module);
    
    free(code);
    
    return result;
}