#ifndef PICKLE_VULKAN_H
#define PICKLE_VULKAN_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <gbm.h>
// Use our local DRM header
#include "drm.h"
#include "error.h"
#include "keystone.h"

// Forward declaration for mpv types
typedef struct mpv_handle mpv_handle;
typedef struct mpv_render_context mpv_render_context;

// Compute shader uniform data structure
typedef struct {
    float corners[4][2];  // TL, TR, BL, BR corners in normalized coordinates
    float texture_size[2]; // Width and height of the texture
    float padding[2];     // Padding to maintain alignment
} vulkan_compute_ubo_t;

// Vulkan queue family indices structure
typedef struct {
    uint32_t graphics;
    uint32_t present;
    bool graphics_present_same;
} vulkan_queue_indices_t;

// Vulkan swapchain support details
typedef struct {
    VkSurfaceCapabilitiesKHR capabilities;
    VkSurfaceFormatKHR *formats;
    uint32_t format_count;
    VkPresentModeKHR *present_modes;
    uint32_t present_mode_count;
} vulkan_swapchain_support_t;

// Vulkan swapchain structure
typedef struct {
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    VkImage *images;
    VkImageView *image_views;
    uint32_t image_count;
    VkFramebuffer *framebuffers;
} vulkan_swapchain_t;

// Vulkan context structure
typedef struct {
    struct gbm_device *gbm_dev;
    struct gbm_surface *gbm_surf;
    
    // Vulkan instance
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    
    // Compute shader resources
    struct {
        bool supported;
        bool initialized;
        VkShaderModule compute_shader;
        VkPipelineLayout pipeline_layout;
        VkPipeline compute_pipeline;
        VkDescriptorSetLayout descriptor_set_layout;
        VkDescriptorPool descriptor_pool;
        VkDescriptorSet descriptor_set;
        VkBuffer uniform_buffer;
        VkDeviceMemory uniform_memory;
        VkImage input_image;
        VkDeviceMemory input_image_memory;
        VkImageView input_image_view;
        VkImage output_image;
        VkDeviceMemory output_image_memory;
        VkImageView output_image_view;
        VkSampler sampler;
        uint32_t width;
        uint32_t height;
    } compute;
    
    // Physical device
    VkPhysicalDevice physical_device;
    VkPhysicalDeviceProperties device_properties;
    VkPhysicalDeviceFeatures device_features;
    
    // Logical device & queues
    VkDevice device;
    vulkan_queue_indices_t queue_indices;
    VkQueue graphics_queue;
    VkQueue present_queue;
    
    // Surface & swapchain
    VkSurfaceKHR surface;
    vulkan_swapchain_t swapchain;
    
    // Command pool & buffers
    VkCommandPool command_pool;
    VkCommandBuffer *command_buffers;
    
    // Synchronization objects
    VkSemaphore *image_available_semaphores;
    VkSemaphore *render_finished_semaphores;
    VkFence *in_flight_fences;
    uint32_t current_frame;
    uint32_t max_frames_in_flight;
    
    // Rendering resources
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline graphics_pipeline;
    
    // DMA-BUF support
    bool dmabuf_supported;
    
    // mpv Vulkan state
    VkSemaphore mpv_render_semaphore;
    VkImageLayout mpv_image_layout;
    
    // Keystone correction resources
    VkBuffer keystone_vertex_buffer;
    VkDeviceMemory keystone_vertex_memory;
    
    // Initialized flag
    bool initialized;
} vulkan_ctx_t;

// DMA-BUF structure for Vulkan
typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint64_t modifier;
    VkDeviceMemory memory;
    VkImage image;
    VkImageView image_view;
} vulkan_dmabuf_info_t;

// Initialization and cleanup
pickle_result_t vulkan_init(vulkan_ctx_t *ctx, const kms_ctx_t *drm);
void vulkan_cleanup(vulkan_ctx_t *ctx);

// Swapchain management
pickle_result_t vulkan_create_swapchain(vulkan_ctx_t *ctx, uint32_t width, uint32_t height);
void vulkan_destroy_swapchain(vulkan_ctx_t *ctx);
pickle_result_t vulkan_recreate_swapchain(vulkan_ctx_t *ctx, uint32_t width, uint32_t height);

// Rendering functions
pickle_result_t vulkan_begin_frame(vulkan_ctx_t *ctx, uint32_t *image_index);
pickle_result_t vulkan_end_frame(vulkan_ctx_t *ctx, uint32_t image_index);
pickle_result_t vulkan_render_frame(vulkan_ctx_t *ctx, mpv_handle *mpv, mpv_render_context *mpv_ctx);

// DMA-BUF support
bool vulkan_is_dmabuf_supported(vulkan_ctx_t *ctx);
pickle_result_t vulkan_import_dmabuf(vulkan_ctx_t *ctx, int fd, uint32_t width, uint32_t height, 
                                     uint32_t format, uint32_t stride, uint64_t modifier,
                                     vulkan_dmabuf_info_t *dmabuf);
void vulkan_destroy_dmabuf(vulkan_ctx_t *ctx, vulkan_dmabuf_info_t *dmabuf);

// Helper functions
bool vulkan_is_available(void);
const char* vulkan_get_device_name(vulkan_ctx_t *ctx);
void vulkan_set_vsync(vulkan_ctx_t *ctx, bool enabled);
bool vulkan_get_vsync(vulkan_ctx_t *ctx);

// Compute shader functions
bool vulkan_compute_is_supported(vulkan_ctx_t *ctx);
pickle_result_t vulkan_compute_init(vulkan_ctx_t *ctx);
pickle_result_t vulkan_compute_create_resources(vulkan_ctx_t *ctx, uint32_t width, uint32_t height);
pickle_result_t vulkan_compute_update_uniform(vulkan_ctx_t *ctx, const keystone_t *keystone);
pickle_result_t vulkan_compute_keystone_apply(vulkan_ctx_t *ctx, VkImage source_image, 
                                              const keystone_t *keystone);
VkImage vulkan_compute_get_output_image(vulkan_ctx_t *ctx);
void vulkan_compute_cleanup(vulkan_ctx_t *ctx);

#endif // PICKLE_VULKAN_H