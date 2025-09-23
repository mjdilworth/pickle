#include "vulkan.h"
#include "log.h"
#include "vulkan_utils.h"
#include <stdlib.h>
#include <string.h>

// Forward declarations for functions defined in vulkan_utils.c

// Check if compute shaders are supported
bool vulkan_compute_is_supported(vulkan_ctx_t *ctx) {
    if (!ctx || !ctx->device) {
        LOG_ERROR("Invalid Vulkan context");
        return false;
    }
    
    // Check if compute queue family is available
    bool compute_queue_found = false;
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &queue_family_count, NULL);
    
    VkQueueFamilyProperties* queue_families = malloc(queue_family_count * sizeof(VkQueueFamilyProperties));
    if (!queue_families) {
        LOG_ERROR("Failed to allocate memory for queue families");
        return false;
    }
    
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &queue_family_count, queue_families);
    
    for (uint32_t i = 0; i < queue_family_count; i++) {
        if (queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            compute_queue_found = true;
            break;
        }
    }
    
    free(queue_families);
    
    if (!compute_queue_found) {
        LOG_WARN("Compute queue not found on this device");
        return false;
    }
    
    LOG_INFO("Vulkan compute shaders are supported on this device");
    return true;
}

// Initialize compute shader resources
pickle_result_t vulkan_compute_init(vulkan_ctx_t *ctx) {
    if (!ctx || !ctx->device) {
        LOG_ERROR("Invalid Vulkan context");
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    // Check if compute shaders are supported
    if (!vulkan_compute_is_supported(ctx)) {
        ctx->compute.supported = false;
        return PICKLE_ERROR_UNSUPPORTED;
    }
    
    // Create descriptor set layout
    VkDescriptorSetLayoutBinding bindings[3] = {
        // Binding 0: Uniform buffer (keystone parameters)
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = NULL
        },
        // Binding 1: Input image
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = NULL
        },
        // Binding 2: Output image
        {
            .binding = 2,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = NULL
        }
    };
    
    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings
    };
    
    if (vkCreateDescriptorSetLayout(ctx->device, &layout_info, NULL, &ctx->compute.descriptor_set_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create compute descriptor set layout");
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &ctx->compute.descriptor_set_layout,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = NULL
    };
    
    if (vkCreatePipelineLayout(ctx->device, &pipeline_layout_info, NULL, &ctx->compute.pipeline_layout) != VK_SUCCESS) {
        LOG_ERROR("Failed to create compute pipeline layout");
        vkDestroyDescriptorSetLayout(ctx->device, ctx->compute.descriptor_set_layout, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create compute shader module
    if (create_shader_module(ctx->device, "shaders/spirv/keystone_compute.spv", &ctx->compute.compute_shader) != VK_SUCCESS) {
        LOG_ERROR("Failed to create compute shader module");
        vkDestroyPipelineLayout(ctx->device, ctx->compute.pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, ctx->compute.descriptor_set_layout, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create compute pipeline
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = ctx->compute.compute_shader,
            .pName = "main"
        },
        .layout = ctx->compute.pipeline_layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1
    };
    
    if (vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &ctx->compute.compute_pipeline) != VK_SUCCESS) {
        LOG_ERROR("Failed to create compute pipeline");
        vkDestroyShaderModule(ctx->device, ctx->compute.compute_shader, NULL);
        vkDestroyPipelineLayout(ctx->device, ctx->compute.pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, ctx->compute.descriptor_set_layout, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create descriptor pool
    VkDescriptorPoolSize pool_sizes[2] = {
        {
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1
        },
        {
            .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 2
        }
    };
    
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
        .maxSets = 1
    };
    
    if (vkCreateDescriptorPool(ctx->device, &pool_info, NULL, &ctx->compute.descriptor_pool) != VK_SUCCESS) {
        LOG_ERROR("Failed to create descriptor pool");
        vkDestroyPipeline(ctx->device, ctx->compute.compute_pipeline, NULL);
        vkDestroyShaderModule(ctx->device, ctx->compute.compute_shader, NULL);
        vkDestroyPipelineLayout(ctx->device, ctx->compute.pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, ctx->compute.descriptor_set_layout, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create sampler
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .mipLodBias = 0.0f,
        .minLod = 0.0f,
        .maxLod = 0.0f
    };
    
    if (vkCreateSampler(ctx->device, &sampler_info, NULL, &ctx->compute.sampler) != VK_SUCCESS) {
        LOG_ERROR("Failed to create sampler");
        vkDestroyDescriptorPool(ctx->device, ctx->compute.descriptor_pool, NULL);
        vkDestroyPipeline(ctx->device, ctx->compute.compute_pipeline, NULL);
        vkDestroyShaderModule(ctx->device, ctx->compute.compute_shader, NULL);
        vkDestroyPipelineLayout(ctx->device, ctx->compute.pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(ctx->device, ctx->compute.descriptor_set_layout, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    ctx->compute.initialized = true;
    ctx->compute.supported = true;
    
    LOG_INFO("Vulkan compute shader initialized successfully");
    
    return PICKLE_OK;
}

// Helper function to create a buffer
static VkResult create_buffer(vulkan_ctx_t *ctx, VkDeviceSize size, VkBufferUsageFlags usage, 
                              VkMemoryPropertyFlags properties, VkBuffer *buffer, VkDeviceMemory *buffer_memory) {
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    if (vkCreateBuffer(ctx->device, &buffer_info, NULL, buffer) != VK_SUCCESS) {
        LOG_ERROR("Failed to create buffer");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(ctx->device, *buffer, &mem_requirements);
    
    // Find memory type index
    uint32_t memory_type_index = 0;
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &mem_properties);
    
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((mem_requirements.memoryTypeBits & (1 << i)) && 
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            memory_type_index = i;
            break;
        }
    }
    
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_requirements.size,
        .memoryTypeIndex = memory_type_index
    };
    
    if (vkAllocateMemory(ctx->device, &alloc_info, NULL, buffer_memory) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate buffer memory");
        vkDestroyBuffer(ctx->device, *buffer, NULL);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    if (vkBindBufferMemory(ctx->device, *buffer, *buffer_memory, 0) != VK_SUCCESS) {
        LOG_ERROR("Failed to bind buffer memory");
        vkFreeMemory(ctx->device, *buffer_memory, NULL);
        vkDestroyBuffer(ctx->device, *buffer, NULL);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    return VK_SUCCESS;
}

// Helper function to create an image
static VkResult create_image(vulkan_ctx_t *ctx, uint32_t width, uint32_t height, VkFormat format,
                             VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                             VkImage *image, VkDeviceMemory *image_memory) {
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = format,
        .tiling = tiling,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = usage,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    if (vkCreateImage(ctx->device, &image_info, NULL, image) != VK_SUCCESS) {
        LOG_ERROR("Failed to create image");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    VkMemoryRequirements mem_requirements;
    vkGetImageMemoryRequirements(ctx->device, *image, &mem_requirements);
    
    // Find memory type index
    uint32_t memory_type_index = 0;
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &mem_properties);
    
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++) {
        if ((mem_requirements.memoryTypeBits & (1 << i)) && 
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            memory_type_index = i;
            break;
        }
    }
    
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_requirements.size,
        .memoryTypeIndex = memory_type_index
    };
    
    if (vkAllocateMemory(ctx->device, &alloc_info, NULL, image_memory) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate image memory");
        vkDestroyImage(ctx->device, *image, NULL);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    if (vkBindImageMemory(ctx->device, *image, *image_memory, 0) != VK_SUCCESS) {
        LOG_ERROR("Failed to bind image memory");
        vkFreeMemory(ctx->device, *image_memory, NULL);
        vkDestroyImage(ctx->device, *image, NULL);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    return VK_SUCCESS;
}

// Helper function to create an image view
static VkResult create_image_view(vulkan_ctx_t *ctx, VkImage image, VkFormat format, 
                                 VkImageViewType view_type, VkImageView *image_view) {
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = view_type,
        .format = format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = 1
    };
    
    return vkCreateImageView(ctx->device, &view_info, NULL, image_view);
}

// Create compute shader resources
pickle_result_t vulkan_compute_create_resources(vulkan_ctx_t *ctx, uint32_t width, uint32_t height) {
    if (!ctx || !ctx->device) {
        LOG_ERROR("Invalid Vulkan context");
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    if (!ctx->compute.initialized) {
        LOG_ERROR("Compute shader not initialized");
        return PICKLE_ERROR_INIT;
    }
    
    // Clean up existing resources
    if (ctx->compute.uniform_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(ctx->device, ctx->compute.uniform_buffer, NULL);
        ctx->compute.uniform_buffer = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.uniform_memory != VK_NULL_HANDLE) {
        vkFreeMemory(ctx->device, ctx->compute.uniform_memory, NULL);
        ctx->compute.uniform_memory = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.input_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx->device, ctx->compute.input_image_view, NULL);
        ctx->compute.input_image_view = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.input_image != VK_NULL_HANDLE) {
        vkDestroyImage(ctx->device, ctx->compute.input_image, NULL);
        ctx->compute.input_image = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.input_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(ctx->device, ctx->compute.input_image_memory, NULL);
        ctx->compute.input_image_memory = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.output_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx->device, ctx->compute.output_image_view, NULL);
        ctx->compute.output_image_view = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.output_image != VK_NULL_HANDLE) {
        vkDestroyImage(ctx->device, ctx->compute.output_image, NULL);
        ctx->compute.output_image = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.output_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(ctx->device, ctx->compute.output_image_memory, NULL);
        ctx->compute.output_image_memory = VK_NULL_HANDLE;
    }
    
    // Create uniform buffer
    VkDeviceSize buffer_size = sizeof(vulkan_compute_ubo_t);
    if (create_buffer(ctx, buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &ctx->compute.uniform_buffer, &ctx->compute.uniform_memory) != VK_SUCCESS) {
        LOG_ERROR("Failed to create uniform buffer");
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create input image
    if (create_image(ctx, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    &ctx->compute.input_image, &ctx->compute.input_image_memory) != VK_SUCCESS) {
        LOG_ERROR("Failed to create input image");
        vkDestroyBuffer(ctx->device, ctx->compute.uniform_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->compute.uniform_memory, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create input image view
    if (create_image_view(ctx, ctx->compute.input_image, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_VIEW_TYPE_2D, &ctx->compute.input_image_view) != VK_SUCCESS) {
        LOG_ERROR("Failed to create input image view");
        vkDestroyImage(ctx->device, ctx->compute.input_image, NULL);
        vkFreeMemory(ctx->device, ctx->compute.input_image_memory, NULL);
        vkDestroyBuffer(ctx->device, ctx->compute.uniform_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->compute.uniform_memory, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create output image
    if (create_image(ctx, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    &ctx->compute.output_image, &ctx->compute.output_image_memory) != VK_SUCCESS) {
        LOG_ERROR("Failed to create output image");
        vkDestroyImageView(ctx->device, ctx->compute.input_image_view, NULL);
        vkDestroyImage(ctx->device, ctx->compute.input_image, NULL);
        vkFreeMemory(ctx->device, ctx->compute.input_image_memory, NULL);
        vkDestroyBuffer(ctx->device, ctx->compute.uniform_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->compute.uniform_memory, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Create output image view
    if (create_image_view(ctx, ctx->compute.output_image, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_VIEW_TYPE_2D, &ctx->compute.output_image_view) != VK_SUCCESS) {
        LOG_ERROR("Failed to create output image view");
        vkDestroyImage(ctx->device, ctx->compute.output_image, NULL);
        vkFreeMemory(ctx->device, ctx->compute.output_image_memory, NULL);
        vkDestroyImageView(ctx->device, ctx->compute.input_image_view, NULL);
        vkDestroyImage(ctx->device, ctx->compute.input_image, NULL);
        vkFreeMemory(ctx->device, ctx->compute.input_image_memory, NULL);
        vkDestroyBuffer(ctx->device, ctx->compute.uniform_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->compute.uniform_memory, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Allocate descriptor sets
    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = ctx->compute.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &ctx->compute.descriptor_set_layout
    };
    
    if (vkAllocateDescriptorSets(ctx->device, &alloc_info, &ctx->compute.descriptor_set) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate descriptor sets");
        vkDestroyImageView(ctx->device, ctx->compute.output_image_view, NULL);
        vkDestroyImage(ctx->device, ctx->compute.output_image, NULL);
        vkFreeMemory(ctx->device, ctx->compute.output_image_memory, NULL);
        vkDestroyImageView(ctx->device, ctx->compute.input_image_view, NULL);
        vkDestroyImage(ctx->device, ctx->compute.input_image, NULL);
        vkFreeMemory(ctx->device, ctx->compute.input_image_memory, NULL);
        vkDestroyBuffer(ctx->device, ctx->compute.uniform_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->compute.uniform_memory, NULL);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Update descriptor sets
    VkDescriptorBufferInfo buffer_info = {
        .buffer = ctx->compute.uniform_buffer,
        .offset = 0,
        .range = sizeof(vulkan_compute_ubo_t)
    };
    
    VkDescriptorImageInfo input_image_info = {
        .imageView = ctx->compute.input_image_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL
    };
    
    VkDescriptorImageInfo output_image_info = {
        .imageView = ctx->compute.output_image_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL
    };
    
    VkWriteDescriptorSet descriptor_writes[3] = {
        // Binding 0: Uniform buffer
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ctx->compute.descriptor_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .pBufferInfo = &buffer_info
        },
        // Binding 1: Input image
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ctx->compute.descriptor_set,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .pImageInfo = &input_image_info
        },
        // Binding 2: Output image
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ctx->compute.descriptor_set,
            .dstBinding = 2,
            .dstArrayElement = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .pImageInfo = &output_image_info
        }
    };
    
    vkUpdateDescriptorSets(ctx->device, 3, descriptor_writes, 0, NULL);
    
    ctx->compute.width = width;
    ctx->compute.height = height;
    
    LOG_INFO("Vulkan compute resources created successfully for %ux%u", width, height);
    
    return PICKLE_OK;
}

// Update uniform buffer with keystone parameters
pickle_result_t vulkan_compute_update_uniform(vulkan_ctx_t *ctx, const keystone_t *keystone) {
    if (!ctx || !ctx->device || !keystone) {
        LOG_ERROR("Invalid parameters");
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    if (!ctx->compute.initialized) {
        LOG_ERROR("Compute shader not initialized");
        return PICKLE_ERROR_INIT;
    }
    
    // Prepare uniform data
    vulkan_compute_ubo_t ubo;
    for (int i = 0; i < 4; i++) {
        ubo.corners[i][0] = keystone->points[i][0];
        ubo.corners[i][1] = keystone->points[i][1];
    }
    ubo.texture_size[0] = (float)ctx->compute.width;
    ubo.texture_size[1] = (float)ctx->compute.height;
    
    // Map uniform buffer memory and copy data
    void *data;
    if (vkMapMemory(ctx->device, ctx->compute.uniform_memory, 0, sizeof(vulkan_compute_ubo_t), 0, &data) != VK_SUCCESS) {
        LOG_ERROR("Failed to map uniform buffer memory");
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    memcpy(data, &ubo, sizeof(vulkan_compute_ubo_t));
    vkUnmapMemory(ctx->device, ctx->compute.uniform_memory);
    
    return PICKLE_OK;
}

// Helper function to transition image layout
static void transition_image_layout(VkCommandBuffer cmd_buffer, VkImage image, 
                                   VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = 1
    };
    
    VkPipelineStageFlags source_stage;
    VkPipelineStageFlags destination_stage;
    
    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        
        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && new_layout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        
        source_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED && new_layout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        
        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else {
        LOG_ERROR("Unsupported layout transition");
        return;
    }
    
    vkCmdPipelineBarrier(
        cmd_buffer,
        source_stage, destination_stage,
        0,
        0, NULL,
        0, NULL,
        1, &barrier
    );
}

// Helper function to copy an image
static void copy_image(VkCommandBuffer cmd_buffer, VkImage src_image, VkImage dst_image, 
                      uint32_t width, uint32_t height) {
    VkImageCopy region = {
        .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .srcSubresource.mipLevel = 0,
        .srcSubresource.baseArrayLayer = 0,
        .srcSubresource.layerCount = 1,
        .srcOffset = {0, 0, 0},
        .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .dstSubresource.mipLevel = 0,
        .dstSubresource.baseArrayLayer = 0,
        .dstSubresource.layerCount = 1,
        .dstOffset = {0, 0, 0},
        .extent = {
            .width = width,
            .height = height,
            .depth = 1
        }
    };
    
    vkCmdCopyImage(
        cmd_buffer,
        src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &region
    );
}

// Apply keystone correction using compute shader
pickle_result_t vulkan_compute_keystone_apply(vulkan_ctx_t *ctx, VkImage source_image, 
                                             const keystone_t *keystone) {
    if (!ctx || !ctx->device || !keystone) {
        LOG_ERROR("Invalid parameters");
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    if (!ctx->compute.initialized) {
        LOG_ERROR("Compute shader not initialized");
        return PICKLE_ERROR_INIT;
    }
    
    if (!keystone->enabled) {
        LOG_WARN("Keystone correction is disabled");
        return PICKLE_OK;
    }
    
    // Update uniform buffer with keystone parameters
    pickle_result_t result = vulkan_compute_update_uniform(ctx, keystone);
    if (result != PICKLE_OK) {
        return result;
    }
    
    // Create command buffer
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    
    VkCommandBuffer command_buffer;
    if (vkAllocateCommandBuffers(ctx->device, &alloc_info, &command_buffer) != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate command buffer");
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Begin command buffer
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    
    if (vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) {
        LOG_ERROR("Failed to begin command buffer");
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &command_buffer);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Transition layouts for the input and output images
    transition_image_layout(command_buffer, ctx->compute.input_image, 
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    
    transition_image_layout(command_buffer, source_image, 
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    
    // Copy source image to input image
    copy_image(command_buffer, source_image, ctx->compute.input_image, 
              ctx->compute.width, ctx->compute.height);
    
    // Transition input image to general layout for compute shader
    transition_image_layout(command_buffer, ctx->compute.input_image, 
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    
    // Transition source image back to present layout
    transition_image_layout(command_buffer, source_image, 
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    
    // Transition output image to general layout for compute shader
    transition_image_layout(command_buffer, ctx->compute.output_image, 
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    
    // Bind compute pipeline
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->compute.compute_pipeline);
    
    // Bind descriptor set
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, 
                           ctx->compute.pipeline_layout, 0, 1, &ctx->compute.descriptor_set, 0, NULL);
    
    // Dispatch compute shader
    uint32_t group_x = (ctx->compute.width + 15) / 16;
    uint32_t group_y = (ctx->compute.height + 15) / 16;
    vkCmdDispatch(command_buffer, group_x, group_y, 1);
    
    // End command buffer
    if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS) {
        LOG_ERROR("Failed to end command buffer");
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &command_buffer);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Submit command buffer
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    
    if (vkQueueSubmit(ctx->graphics_queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        LOG_ERROR("Failed to submit command buffer");
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &command_buffer);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Wait for the queue to finish
    if (vkQueueWaitIdle(ctx->graphics_queue) != VK_SUCCESS) {
        LOG_ERROR("Failed to wait for queue idle");
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &command_buffer);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Free command buffer
    vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &command_buffer);
    
    return PICKLE_OK;
}

// Get the output image
VkImage vulkan_compute_get_output_image(vulkan_ctx_t *ctx) {
    if (!ctx || !ctx->compute.initialized) {
        return VK_NULL_HANDLE;
    }
    
    return ctx->compute.output_image;
}

// Clean up compute shader resources
void vulkan_compute_cleanup(vulkan_ctx_t *ctx) {
    if (!ctx || !ctx->device || !ctx->compute.initialized) {
        return;
    }
    
    if (ctx->compute.uniform_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(ctx->device, ctx->compute.uniform_buffer, NULL);
        ctx->compute.uniform_buffer = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.uniform_memory != VK_NULL_HANDLE) {
        vkFreeMemory(ctx->device, ctx->compute.uniform_memory, NULL);
        ctx->compute.uniform_memory = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.input_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx->device, ctx->compute.input_image_view, NULL);
        ctx->compute.input_image_view = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.input_image != VK_NULL_HANDLE) {
        vkDestroyImage(ctx->device, ctx->compute.input_image, NULL);
        ctx->compute.input_image = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.input_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(ctx->device, ctx->compute.input_image_memory, NULL);
        ctx->compute.input_image_memory = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.output_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(ctx->device, ctx->compute.output_image_view, NULL);
        ctx->compute.output_image_view = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.output_image != VK_NULL_HANDLE) {
        vkDestroyImage(ctx->device, ctx->compute.output_image, NULL);
        ctx->compute.output_image = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.output_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(ctx->device, ctx->compute.output_image_memory, NULL);
        ctx->compute.output_image_memory = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(ctx->device, ctx->compute.sampler, NULL);
        ctx->compute.sampler = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(ctx->device, ctx->compute.descriptor_pool, NULL);
        ctx->compute.descriptor_pool = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.compute_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(ctx->device, ctx->compute.compute_pipeline, NULL);
        ctx->compute.compute_pipeline = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.compute_shader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(ctx->device, ctx->compute.compute_shader, NULL);
        ctx->compute.compute_shader = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(ctx->device, ctx->compute.pipeline_layout, NULL);
        ctx->compute.pipeline_layout = VK_NULL_HANDLE;
    }
    
    if (ctx->compute.descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(ctx->device, ctx->compute.descriptor_set_layout, NULL);
        ctx->compute.descriptor_set_layout = VK_NULL_HANDLE;
    }
    
    ctx->compute.initialized = false;
}