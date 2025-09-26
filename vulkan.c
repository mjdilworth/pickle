#include "vulkan.h"
#include "utils.h"
#include "log.h"
#include "keystone.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>  // For dlopen and dlclose
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

// Define macros for MIN and MAX if not already defined
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Define logging macros if not already defined
#ifndef LOG_VULKAN
#define LOG_VULKAN(fmt, ...) fprintf(stderr, "[VULKAN] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef LOG_ERROR
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#endif

#ifndef PICKLE_RESULT_CHECK
#define PICKLE_RESULT_CHECK(res, msg) \
    do { \
        if ((res) != PICKLE_OK) { \
            LOG_ERROR("%s: %s", (msg), pickle_result_string(res)); \
            return res; \
        } \
    } while(0)
#endif

// Validation layer support - force disabled for now
const bool enableValidationLayers = false;

// Validation layers to request
const char* validationLayers[] = {
    "VK_LAYER_KHRONOS_validation"
};
const int validationLayerCount = sizeof(validationLayers) / sizeof(validationLayers[0]);

// Device extensions to request
const char* deviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME
};
const int deviceExtensionCount = sizeof(deviceExtensions) / sizeof(deviceExtensions[0]);

// Maximum number of frames in flight
const int MAX_FRAMES_IN_FLIGHT = 2;

// Forward declarations of static functions
static VkResult create_debug_utils_messenger_EXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pDebugMessenger);

static void destroy_debug_utils_messenger_EXT(
    VkInstance instance,
    VkDebugUtilsMessengerEXT debugMessenger,
    const VkAllocationCallbacks* pAllocator);

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData);

static bool check_validation_layer_support();
static bool check_device_extension_support(VkPhysicalDevice device);
static bool is_device_suitable(VkPhysicalDevice device, VkSurfaceKHR surface);
static vulkan_queue_indices_t find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface);
static vulkan_swapchain_support_t query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface);
static VkSurfaceFormatKHR choose_swap_surface_format(const VkSurfaceFormatKHR* available_formats, uint32_t format_count);
static VkPresentModeKHR choose_swap_present_mode(const VkPresentModeKHR* available_present_modes, uint32_t present_mode_count, bool vsync);
static VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR* capabilities, uint32_t width, uint32_t height);
static pickle_result_t create_shader_module(vulkan_ctx_t* ctx, const char* code, size_t code_size, VkShaderModule* shader_module) __attribute__((unused));
static void cleanup_swapchain(vulkan_ctx_t* ctx);

// Global state
static bool g_vulkan_available = false;
static bool g_vsync_enabled = true;

// Initialize Vulkan
pickle_result_t vulkan_init(vulkan_ctx_t *ctx, const kms_ctx_t *drm) {
    if (!ctx || !drm) {
        LOG_ERROR("Invalid context or DRM context provided");
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    LOG_VULKAN("Initializing Vulkan");
    
    // Clear the context
    memset(ctx, 0, sizeof(vulkan_ctx_t));
    
    // Set up GBM device and surface
    ctx->gbm_dev = gbm_create_device(drm->fd);
    if (!ctx->gbm_dev) {
        LOG_ERROR("Failed to create GBM device: %s", strerror(errno));
        return PICKLE_ERROR_GBM_INIT;
    }
    
    ctx->gbm_surf = gbm_surface_create(
        ctx->gbm_dev,
        drm->mode.hdisplay,
        drm->mode.vdisplay,
        GBM_FORMAT_XRGB8888,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING
    );
    
    if (!ctx->gbm_surf) {
        LOG_ERROR("Failed to create GBM surface: %s", strerror(errno));
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_GBM_SURFACE;
    }
    
    // Create Vulkan instance
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Pickle Video Player",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Pickle",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_2
    };
    
    // We need these extensions
    const char* required_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_DISPLAY_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
    };
    uint32_t required_extension_count = sizeof(required_extensions) / sizeof(required_extensions[0]);
    
    // Add validation layer extensions if enabled
    const char* validation_extensions[] = {
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME
    };
    uint32_t validation_extension_count = enableValidationLayers ? 
                                          sizeof(validation_extensions) / sizeof(validation_extensions[0]) : 0;
    
    // Combine all extensions
    uint32_t extension_count = required_extension_count + validation_extension_count;
    const char** extensions = malloc(extension_count * sizeof(const char*));
    if (!extensions) {
        LOG_ERROR("Failed to allocate memory for extension names");
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    // Copy required extensions
    for (uint32_t i = 0; i < required_extension_count; i++) {
        extensions[i] = required_extensions[i];
    }
    
    // Copy validation extensions if needed
    if (enableValidationLayers) {
        for (uint32_t i = 0; i < validation_extension_count; i++) {
            extensions[required_extension_count + i] = validation_extensions[i];
        }
    }
    
    // Check validation layer support
    if (enableValidationLayers && !check_validation_layer_support()) {
        LOG_ERROR("Validation layers requested, but not available");
        free(extensions);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_VALIDATION_LAYERS;
    }
    
    // Debug messenger create info
    VkDebugUtilsMessengerCreateInfoEXT debug_create_info = {0};
    if (enableValidationLayers) {
        debug_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_create_info.pfnUserCallback = debug_callback;
    }
    
    // Create Vulkan instance
    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = extensions
    };
    
    if (enableValidationLayers) {
        create_info.enabledLayerCount = validationLayerCount;
        create_info.ppEnabledLayerNames = validationLayers;
        create_info.pNext = &debug_create_info;
    } else {
        create_info.enabledLayerCount = 0;
        create_info.pNext = NULL;
    }
    
    VkResult result = vkCreateInstance(&create_info, NULL, &ctx->instance);
    free(extensions);
    
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to create Vulkan instance: %d", result);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_INSTANCE;
    }
    
    // Set up debug messenger
    if (enableValidationLayers) {
        result = create_debug_utils_messenger_EXT(ctx->instance, &debug_create_info, NULL, &ctx->debug_messenger);
        if (result != VK_SUCCESS) {
            LOG_ERROR("Failed to set up debug messenger: %d", result);
            vkDestroyInstance(ctx->instance, NULL);
            gbm_surface_destroy(ctx->gbm_surf);
            gbm_device_destroy(ctx->gbm_dev);
            return PICKLE_ERROR_VULKAN_DEBUG_MESSENGER;
        }
    }
    
    // Create surface
    // TODO: Implement platform-specific surface creation
    // For Linux/DRM, we would use VK_KHR_wayland_surface or VK_KHR_xcb_surface or VK_KHR_display
    
    // For now, let's assume we're using the display extension
    // Find the display and mode that matches our DRM configuration
    
    // Create a display surface using VK_KHR_display
    uint32_t display_count = 0;
    vkGetPhysicalDeviceDisplayPropertiesKHR(ctx->physical_device, &display_count, NULL);
    
    if (display_count == 0) {
        LOG_ERROR("No displays found for Vulkan");
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_NO_DISPLAY;
    }
    
    VkDisplayPropertiesKHR* display_properties = malloc(display_count * sizeof(VkDisplayPropertiesKHR));
    if (!display_properties) {
        LOG_ERROR("Failed to allocate memory for display properties");
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    vkGetPhysicalDeviceDisplayPropertiesKHR(ctx->physical_device, &display_count, display_properties);
    
    // Find a suitable display
    VkDisplayKHR display = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < display_count; i++) {
        display = display_properties[i].display;
        break; // Just take the first display for now
    }
    
    free(display_properties);
    
    if (display == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to find a suitable display");
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_NO_DISPLAY;
    }
    
    // Get display modes
    uint32_t mode_count = 0;
    vkGetDisplayModePropertiesKHR(ctx->physical_device, display, &mode_count, NULL);
    
    if (mode_count == 0) {
        LOG_ERROR("No display modes found");
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_NO_DISPLAY_MODE;
    }
    
    VkDisplayModePropertiesKHR* mode_properties = malloc(mode_count * sizeof(VkDisplayModePropertiesKHR));
    if (!mode_properties) {
        LOG_ERROR("Failed to allocate memory for display mode properties");
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    vkGetDisplayModePropertiesKHR(ctx->physical_device, display, &mode_count, mode_properties);
    
    // Find a suitable display mode
    VkDisplayModeKHR display_mode = VK_NULL_HANDLE;
    uint32_t target_width = drm->mode.hdisplay;
    uint32_t target_height = drm->mode.vdisplay;
    uint32_t refresh_rate = drm->mode.vrefresh * 1000; // Convert to mHz
    
    for (uint32_t i = 0; i < mode_count; i++) {
        VkDisplayModeParametersKHR parameters = mode_properties[i].parameters;
        
        if (parameters.visibleRegion.width == target_width && 
            parameters.visibleRegion.height == target_height &&
            parameters.refreshRate == refresh_rate) {
            display_mode = mode_properties[i].displayMode;
            break;
        }
    }
    
    // If we couldn't find an exact match, just take the first mode
    if (display_mode == VK_NULL_HANDLE && mode_count > 0) {
        display_mode = mode_properties[0].displayMode;
        LOG_VULKAN("Using display mode %ux%u @ %u mHz (not an exact match)",
                  mode_properties[0].parameters.visibleRegion.width,
                  mode_properties[0].parameters.visibleRegion.height,
                  mode_properties[0].parameters.refreshRate);
    }
    
    free(mode_properties);
    
    if (display_mode == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to find a suitable display mode");
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_NO_DISPLAY_MODE;
    }
    
    // Create display surface
    VkDisplaySurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = display_mode,
        .planeIndex = 0, // Primary plane
        .planeStackIndex = 0,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = {
            .width = target_width,
            .height = target_height
        }
    };
    
    result = vkCreateDisplayPlaneSurfaceKHR(ctx->instance, &surface_info, NULL, &ctx->surface);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to create display surface: %d", result);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_SURFACE;
    }
    
    // Enumerate physical devices
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &device_count, NULL);
    
    if (device_count == 0) {
        LOG_ERROR("Failed to find GPUs with Vulkan support");
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_NO_DEVICE;
    }
    
    VkPhysicalDevice* devices = malloc(device_count * sizeof(VkPhysicalDevice));
    if (!devices) {
        LOG_ERROR("Failed to allocate memory for physical devices");
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    vkEnumeratePhysicalDevices(ctx->instance, &device_count, devices);
    
    // TODO: Find the right physical device that matches our DRM device
    // For now, just pick the first one
    bool found_suitable_device = false;
    
    // Go through each device and check if it's suitable
    for (uint32_t i = 0; i < device_count; i++) {
        if (is_device_suitable(devices[i], ctx->surface)) {
            ctx->physical_device = devices[i];
            found_suitable_device = true;
            break;
        }
    }
    
    free(devices);
    
    if (!found_suitable_device || ctx->physical_device == VK_NULL_HANDLE) {
        LOG_ERROR("Failed to find a suitable GPU");
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_NO_SUITABLE_DEVICE;
    }
    
    // Get physical device properties
    vkGetPhysicalDeviceProperties(ctx->physical_device, &ctx->device_properties);
    vkGetPhysicalDeviceFeatures(ctx->physical_device, &ctx->device_features);
    
    LOG_VULKAN("Selected GPU: %s", ctx->device_properties.deviceName);
    
    // TODO: Create Vulkan surface from DRM/GBM
    // This is platform-specific and would require extensions
    
    // For now, let's assume we have a surface
    // In reality, we would need to use VK_KHR_display or other extensions
    
    // Find queue families
    ctx->queue_indices = find_queue_families(ctx->physical_device, ctx->surface);
    
    // Create logical device
    VkDeviceQueueCreateInfo queue_create_infos[2] = {0};
    uint32_t queue_create_info_count = 0;
    
    uint32_t queue_family_indices[2] = {
        ctx->queue_indices.graphics,
        ctx->queue_indices.present
    };
    
    float queue_priority = 1.0f;
    
    for (uint32_t i = 0; i < (ctx->queue_indices.graphics_present_same ? 1 : 2); i++) {
        VkDeviceQueueCreateInfo queue_create_info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = queue_family_indices[i],
            .queueCount = 1,
            .pQueuePriorities = &queue_priority
        };
        queue_create_infos[queue_create_info_count++] = queue_create_info;
    }
    
    // Device features
    VkPhysicalDeviceFeatures device_features = {0};
    
    // Create the logical device
    VkDeviceCreateInfo device_create_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = queue_create_info_count,
        .pQueueCreateInfos = queue_create_infos,
        .pEnabledFeatures = &device_features,
        .enabledExtensionCount = deviceExtensionCount,
        .ppEnabledExtensionNames = deviceExtensions
    };
    
    if (enableValidationLayers) {
        device_create_info.enabledLayerCount = validationLayerCount;
        device_create_info.ppEnabledLayerNames = validationLayers;
    } else {
        device_create_info.enabledLayerCount = 0;
    }
    
    result = vkCreateDevice(ctx->physical_device, &device_create_info, NULL, &ctx->device);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to create logical device: %d", result);
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_DEVICE;
    }
    
    // Get device queues
    vkGetDeviceQueue(ctx->device, ctx->queue_indices.graphics, 0, &ctx->graphics_queue);
    vkGetDeviceQueue(ctx->device, ctx->queue_indices.present, 0, &ctx->present_queue);
    
    // Create swapchain
    pickle_result_t res = vulkan_create_swapchain(ctx, drm->mode.hdisplay, drm->mode.vdisplay);
    if (res != PICKLE_OK) {
        vkDestroyDevice(ctx->device, NULL);
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return res;
    }
    
    // Create command pool
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = ctx->queue_indices.graphics,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
    };
    
    result = vkCreateCommandPool(ctx->device, &pool_info, NULL, &ctx->command_pool);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to create command pool: %d", result);
        cleanup_swapchain(ctx);
        vkDestroyDevice(ctx->device, NULL);
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_COMMAND_POOL;
    }
    
    // Create command buffers
    ctx->command_buffers = malloc(ctx->swapchain.image_count * sizeof(VkCommandBuffer));
    if (!ctx->command_buffers) {
        LOG_ERROR("Failed to allocate memory for command buffers");
        vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
        cleanup_swapchain(ctx);
        vkDestroyDevice(ctx->device, NULL);
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ctx->swapchain.image_count
    };
    
    result = vkAllocateCommandBuffers(ctx->device, &alloc_info, ctx->command_buffers);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to allocate command buffers: %d", result);
        free(ctx->command_buffers);
        vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
        cleanup_swapchain(ctx);
        vkDestroyDevice(ctx->device, NULL);
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_COMMAND_BUFFERS;
    }
    
    // Create synchronization objects
    ctx->max_frames_in_flight = MAX_FRAMES_IN_FLIGHT;
    ctx->current_frame = 0;
    
    ctx->image_available_semaphores = malloc(ctx->max_frames_in_flight * sizeof(VkSemaphore));
    ctx->render_finished_semaphores = malloc(ctx->max_frames_in_flight * sizeof(VkSemaphore));
    ctx->in_flight_fences = malloc(ctx->max_frames_in_flight * sizeof(VkFence));
    
    if (!ctx->image_available_semaphores || !ctx->render_finished_semaphores || !ctx->in_flight_fences) {
        LOG_ERROR("Failed to allocate memory for synchronization objects");
        if (ctx->image_available_semaphores) free(ctx->image_available_semaphores);
        if (ctx->render_finished_semaphores) free(ctx->render_finished_semaphores);
        if (ctx->in_flight_fences) free(ctx->in_flight_fences);
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, ctx->swapchain.image_count, ctx->command_buffers);
        free(ctx->command_buffers);
        vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
        cleanup_swapchain(ctx);
        vkDestroyDevice(ctx->device, NULL);
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    
    for (uint32_t i = 0; i < ctx->max_frames_in_flight; i++) {
        if (vkCreateSemaphore(ctx->device, &semaphore_info, NULL, &ctx->image_available_semaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(ctx->device, &semaphore_info, NULL, &ctx->render_finished_semaphores[i]) != VK_SUCCESS ||
            vkCreateFence(ctx->device, &fence_info, NULL, &ctx->in_flight_fences[i]) != VK_SUCCESS) {
            
            LOG_ERROR("Failed to create synchronization objects");
            
            // Clean up already created objects
            for (uint32_t j = 0; j < i; j++) {
                vkDestroySemaphore(ctx->device, ctx->image_available_semaphores[j], NULL);
                vkDestroySemaphore(ctx->device, ctx->render_finished_semaphores[j], NULL);
                vkDestroyFence(ctx->device, ctx->in_flight_fences[j], NULL);
            }
            
            free(ctx->image_available_semaphores);
            free(ctx->render_finished_semaphores);
            free(ctx->in_flight_fences);
            vkFreeCommandBuffers(ctx->device, ctx->command_pool, ctx->swapchain.image_count, ctx->command_buffers);
            free(ctx->command_buffers);
            vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
            cleanup_swapchain(ctx);
            vkDestroyDevice(ctx->device, NULL);
            vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
            if (enableValidationLayers) {
                destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
            }
            vkDestroyInstance(ctx->instance, NULL);
            gbm_surface_destroy(ctx->gbm_surf);
            gbm_device_destroy(ctx->gbm_dev);
            return PICKLE_ERROR_VULKAN_SYNC_OBJECTS;
        }
    }
    
    // Create a semaphore for mpv rendering
    if (vkCreateSemaphore(ctx->device, &semaphore_info, NULL, &ctx->mpv_render_semaphore) != VK_SUCCESS) {
        LOG_ERROR("Failed to create MPV render semaphore");
        for (uint32_t i = 0; i < ctx->max_frames_in_flight; i++) {
            vkDestroySemaphore(ctx->device, ctx->image_available_semaphores[i], NULL);
            vkDestroySemaphore(ctx->device, ctx->render_finished_semaphores[i], NULL);
            vkDestroyFence(ctx->device, ctx->in_flight_fences[i], NULL);
        }
        free(ctx->image_available_semaphores);
        free(ctx->render_finished_semaphores);
        free(ctx->in_flight_fences);
        vkFreeCommandBuffers(ctx->device, ctx->command_pool, ctx->swapchain.image_count, ctx->command_buffers);
        free(ctx->command_buffers);
        vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
        cleanup_swapchain(ctx);
        vkDestroyDevice(ctx->device, NULL);
        vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (enableValidationLayers) {
            destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
        gbm_surface_destroy(ctx->gbm_surf);
        gbm_device_destroy(ctx->gbm_dev);
        return PICKLE_ERROR_VULKAN_SYNC_OBJECTS;
    }
    
    // Initialize keystone correction resources
    if (vulkan_compute_is_supported(ctx)) {
        LOG_VULKAN("Initializing compute shader for keystone correction");
        pickle_result_t compute_result = vulkan_compute_init(ctx);
        if (compute_result != PICKLE_OK) {
            LOG_WARN("Failed to initialize compute shader for keystone correction: %d", compute_result);
        } else {
            LOG_INFO("Compute shader for keystone correction initialized successfully");
        }
    } else {
        LOG_WARN("Compute shaders not supported on this device, keystone correction will not be available");
    }
    
    // Set initialization flag
    ctx->initialized = true;
    g_vulkan_available = true;
    
    LOG_VULKAN("Vulkan initialization complete");
    return PICKLE_OK;
}

// Clean up Vulkan resources
void vulkan_cleanup(vulkan_ctx_t *ctx) {
    if (!ctx || !ctx->initialized) {
        return;
    }
    
    LOG_VULKAN("Cleaning up Vulkan resources");
    
    // Wait for device to finish operations
    vkDeviceWaitIdle(ctx->device);
    
    // Clean up synchronization objects
    for (uint32_t i = 0; i < ctx->max_frames_in_flight; i++) {
        vkDestroySemaphore(ctx->device, ctx->image_available_semaphores[i], NULL);
        vkDestroySemaphore(ctx->device, ctx->render_finished_semaphores[i], NULL);
        vkDestroyFence(ctx->device, ctx->in_flight_fences[i], NULL);
    }
    free(ctx->image_available_semaphores);
    free(ctx->render_finished_semaphores);
    free(ctx->in_flight_fences);
    
    vkDestroySemaphore(ctx->device, ctx->mpv_render_semaphore, NULL);
    
    // Clean up command buffers and pool
    vkFreeCommandBuffers(ctx->device, ctx->command_pool, ctx->swapchain.image_count, ctx->command_buffers);
    free(ctx->command_buffers);
    vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
    
    // Clean up swapchain
    cleanup_swapchain(ctx);
    
    // Clean up compute shader resources
    if (ctx->compute.initialized) {
        LOG_VULKAN("Cleaning up compute shader resources");
        vulkan_compute_cleanup(ctx);
    }
    
    // Clean up device
    vkDestroyDevice(ctx->device, NULL);
    
    // Clean up surface
    vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
    
    // Clean up debug messenger
    if (enableValidationLayers) {
        destroy_debug_utils_messenger_EXT(ctx->instance, ctx->debug_messenger, NULL);
    }
    
    // Clean up instance
    vkDestroyInstance(ctx->instance, NULL);
    
    // Clean up GBM
    gbm_surface_destroy(ctx->gbm_surf);
    gbm_device_destroy(ctx->gbm_dev);
    
    // Reset initialization flag
    ctx->initialized = false;
}

// Create swapchain
pickle_result_t vulkan_create_swapchain(vulkan_ctx_t *ctx, uint32_t width, uint32_t height) {
    if (!ctx || !ctx->device) {
        LOG_ERROR("Invalid Vulkan context");
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    LOG_VULKAN("Creating swapchain (%ux%u)", width, height);
    
    // Query swapchain support
    vulkan_swapchain_support_t swapchain_support = query_swapchain_support(ctx->physical_device, ctx->surface);
    
    // Choose the best surface format, present mode, and extent
    VkSurfaceFormatKHR surface_format = choose_swap_surface_format(swapchain_support.formats, swapchain_support.format_count);
    VkPresentModeKHR present_mode = choose_swap_present_mode(swapchain_support.present_modes, swapchain_support.present_mode_count, g_vsync_enabled);
    VkExtent2D extent = choose_swap_extent(&swapchain_support.capabilities, width, height);
    
    // Choose the number of images in the swapchain
    uint32_t image_count = swapchain_support.capabilities.minImageCount + 1;
    if (swapchain_support.capabilities.maxImageCount > 0 && image_count > swapchain_support.capabilities.maxImageCount) {
        image_count = swapchain_support.capabilities.maxImageCount;
    }
    
    // Create swapchain info
    VkSwapchainCreateInfoKHR create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = ctx->surface,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .preTransform = swapchain_support.capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE
    };
    
    // Set queue family indices
    uint32_t queue_family_indices[] = {
        ctx->queue_indices.graphics,
        ctx->queue_indices.present
    };
    
    if (ctx->queue_indices.graphics_present_same) {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.queueFamilyIndexCount = 0;
        create_info.pQueueFamilyIndices = NULL;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = queue_family_indices;
    }
    
    // Create the swapchain
    VkResult result = vkCreateSwapchainKHR(ctx->device, &create_info, NULL, &ctx->swapchain.handle);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to create swapchain: %d", result);
        return PICKLE_ERROR_VULKAN_SWAPCHAIN;
    }
    
    // Get swapchain images
    vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain.handle, &image_count, NULL);
    ctx->swapchain.images = malloc(image_count * sizeof(VkImage));
    if (!ctx->swapchain.images) {
        LOG_ERROR("Failed to allocate memory for swapchain images");
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain.handle, NULL);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain.handle, &image_count, ctx->swapchain.images);
    
    // Store swapchain info
    ctx->swapchain.format = surface_format.format;
    ctx->swapchain.extent = extent;
    ctx->swapchain.image_count = image_count;
    
    // Create image views
    ctx->swapchain.image_views = malloc(image_count * sizeof(VkImageView));
    if (!ctx->swapchain.image_views) {
        LOG_ERROR("Failed to allocate memory for swapchain image views");
        free(ctx->swapchain.images);
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain.handle, NULL);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = ctx->swapchain.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = ctx->swapchain.format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        
        result = vkCreateImageView(ctx->device, &view_info, NULL, &ctx->swapchain.image_views[i]);
        if (result != VK_SUCCESS) {
            LOG_ERROR("Failed to create image view %u: %d", i, result);
            
            // Clean up already created image views
            for (uint32_t j = 0; j < i; j++) {
                vkDestroyImageView(ctx->device, ctx->swapchain.image_views[j], NULL);
            }
            
            free(ctx->swapchain.image_views);
            free(ctx->swapchain.images);
            vkDestroySwapchainKHR(ctx->device, ctx->swapchain.handle, NULL);
            return PICKLE_ERROR_VULKAN_IMAGE_VIEW;
        }
    }
    
    // Create render pass
    VkAttachmentDescription color_attachment = {
        .format = ctx->swapchain.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };
    
    VkAttachmentReference color_attachment_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref
    };
    
    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
    };
    
    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency
    };
    
    result = vkCreateRenderPass(ctx->device, &render_pass_info, NULL, &ctx->render_pass);
    if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to create render pass: %d", result);
        
        for (uint32_t i = 0; i < ctx->swapchain.image_count; i++) {
            vkDestroyImageView(ctx->device, ctx->swapchain.image_views[i], NULL);
        }
        
        free(ctx->swapchain.image_views);
        free(ctx->swapchain.images);
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain.handle, NULL);
        return PICKLE_ERROR_VULKAN_RENDER_PASS;
    }
    
    // Create framebuffers
    ctx->swapchain.framebuffers = malloc(ctx->swapchain.image_count * sizeof(VkFramebuffer));
    if (!ctx->swapchain.framebuffers) {
        LOG_ERROR("Failed to allocate memory for framebuffers");
        vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);
        
        for (uint32_t i = 0; i < ctx->swapchain.image_count; i++) {
            vkDestroyImageView(ctx->device, ctx->swapchain.image_views[i], NULL);
        }
        
        free(ctx->swapchain.image_views);
        free(ctx->swapchain.images);
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain.handle, NULL);
        return PICKLE_ERROR_OUT_OF_MEMORY;
    }
    
    for (uint32_t i = 0; i < ctx->swapchain.image_count; i++) {
        VkImageView attachments[] = {
            ctx->swapchain.image_views[i]
        };
        
        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = ctx->render_pass,
            .attachmentCount = 1,
            .pAttachments = attachments,
            .width = ctx->swapchain.extent.width,
            .height = ctx->swapchain.extent.height,
            .layers = 1
        };
        
        result = vkCreateFramebuffer(ctx->device, &framebuffer_info, NULL, &ctx->swapchain.framebuffers[i]);
        if (result != VK_SUCCESS) {
            LOG_ERROR("Failed to create framebuffer %u: %d", i, result);
            
            // Clean up already created framebuffers
            for (uint32_t j = 0; j < i; j++) {
                vkDestroyFramebuffer(ctx->device, ctx->swapchain.framebuffers[j], NULL);
            }
            
            free(ctx->swapchain.framebuffers);
            vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);
            
            for (uint32_t j = 0; j < ctx->swapchain.image_count; j++) {
                vkDestroyImageView(ctx->device, ctx->swapchain.image_views[j], NULL);
            }
            
            free(ctx->swapchain.image_views);
            free(ctx->swapchain.images);
            vkDestroySwapchainKHR(ctx->device, ctx->swapchain.handle, NULL);
            return PICKLE_ERROR_VULKAN_FRAMEBUFFER;
        }
    }
    
    LOG_VULKAN("Swapchain created with %u images (%ux%u)", ctx->swapchain.image_count, 
              ctx->swapchain.extent.width, ctx->swapchain.extent.height);
    
    // Create compute shader resources for keystone correction if compute is supported
    if (ctx->compute.supported) {
        LOG_VULKAN("Creating compute shader resources for keystone correction");
        pickle_result_t compute_result = vulkan_compute_create_resources(ctx, width, height);
        if (compute_result != PICKLE_OK) {
            LOG_WARN("Failed to create compute shader resources: %d", compute_result);
        } else {
            LOG_INFO("Compute shader resources created successfully");
        }
    }
    
    return PICKLE_OK;
}

// Destroy swapchain
void vulkan_destroy_swapchain(vulkan_ctx_t *ctx) {
    cleanup_swapchain(ctx);
}

// Recreate swapchain
pickle_result_t vulkan_recreate_swapchain(vulkan_ctx_t *ctx, uint32_t width, uint32_t height) {
    if (!ctx || !ctx->initialized) {
        return PICKLE_ERROR_INIT;
    }
    
    LOG_VULKAN("Recreating swapchain (%ux%u)", width, height);
    
    // Wait for device to finish operations
    vkDeviceWaitIdle(ctx->device);
    
    // Clean up old swapchain
    cleanup_swapchain(ctx);
    
    // Create new swapchain
    pickle_result_t result = vulkan_create_swapchain(ctx, width, height);
    if (result != PICKLE_OK) {
        LOG_ERROR("Failed to recreate swapchain: %d", result);
        return result;
    }
    
    LOG_VULKAN("Swapchain recreated successfully");
    return PICKLE_OK;
}

// Begin frame rendering
pickle_result_t vulkan_begin_frame(vulkan_ctx_t *ctx, uint32_t *image_index) {
    if (!ctx || !ctx->initialized || !image_index) {
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    // Wait for previous frame to complete
    vkWaitForFences(ctx->device, 1, &ctx->in_flight_fences[ctx->current_frame], VK_TRUE, UINT64_MAX);
    
    // Acquire next swapchain image
    VkResult result = vkAcquireNextImageKHR(ctx->device, ctx->swapchain.handle, UINT64_MAX,
                                          ctx->image_available_semaphores[ctx->current_frame], 
                                          VK_NULL_HANDLE, image_index);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // Swapchain is outdated or suboptimal, recreate it
        uint32_t width = ctx->swapchain.extent.width;
        uint32_t height = ctx->swapchain.extent.height;
        return vulkan_recreate_swapchain(ctx, width, height);
    } else if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to acquire swapchain image: %d", result);
        return PICKLE_ERROR_VULKAN_SWAPCHAIN;
    }
    
    // Reset fence for current frame
    vkResetFences(ctx->device, 1, &ctx->in_flight_fences[ctx->current_frame]);
    
    return PICKLE_OK;
}

// End frame rendering
pickle_result_t vulkan_end_frame(vulkan_ctx_t *ctx, uint32_t image_index) {
    if (!ctx || !ctx->initialized) {
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    // Set up submit info for rendering commands
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ctx->image_available_semaphores[ctx->current_frame],
        .pWaitDstStageMask = (VkPipelineStageFlags[]) { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT },
        .commandBufferCount = 1,
        .pCommandBuffers = &ctx->command_buffers[image_index],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &ctx->render_finished_semaphores[ctx->current_frame]
    };
    
    // Submit rendering commands
    if (vkQueueSubmit(ctx->graphics_queue, 1, &submit_info, ctx->in_flight_fences[ctx->current_frame]) != VK_SUCCESS) {
        LOG_ERROR("Failed to submit draw command buffer");
        return PICKLE_ERROR_VULKAN_COMMAND_BUFFERS;
    }
    
    // Set up present info for presenting the frame
    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ctx->render_finished_semaphores[ctx->current_frame],
        .swapchainCount = 1,
        .pSwapchains = &ctx->swapchain.handle,
        .pImageIndices = &image_index
    };
    
    // Present the frame
    VkResult result = vkQueuePresentKHR(ctx->present_queue, &present_info);
    
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // Swapchain is outdated or suboptimal, recreate it
        uint32_t width = ctx->swapchain.extent.width;
        uint32_t height = ctx->swapchain.extent.height;
        return vulkan_recreate_swapchain(ctx, width, height);
    } else if (result != VK_SUCCESS) {
        LOG_ERROR("Failed to present swapchain image: %d", result);
        return PICKLE_ERROR_VULKAN_SWAPCHAIN;
    }
    
    // Move to next frame
    ctx->current_frame = (ctx->current_frame + 1) % ctx->max_frames_in_flight;
    
    return PICKLE_OK;
}

// Render a frame
pickle_result_t vulkan_render_frame(vulkan_ctx_t *ctx, mpv_handle *mpv, mpv_render_context *mpv_ctx) {
    if (!ctx || !ctx->initialized) {
        return PICKLE_ERROR_INIT;
    }
    
    if (!mpv || !mpv_ctx) {
        return PICKLE_ERROR_INVALID_PARAMETER;
    }
    
    uint32_t image_index;
    pickle_result_t result = vulkan_begin_frame(ctx, &image_index);
    if (result != PICKLE_OK) {
        return result;
    }
    
    // Determine whether to render with MPV
    int mpv_has_frame = 0;
    if (mpv_render_context_update(mpv_ctx) & MPV_RENDER_UPDATE_FRAME) {
        mpv_has_frame = 1;
    }
    
    // Begin command buffer recording
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(ctx->command_buffers[image_index], &begin_info);
    
    // Set up render pass
    VkClearValue clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    VkRenderPassBeginInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = ctx->render_pass,
        .framebuffer = ctx->swapchain.framebuffers[image_index],
        .renderArea.offset = {0, 0},
        .renderArea.extent = ctx->swapchain.extent,
        .clearValueCount = 1,
        .pClearValues = &clear_color
    };
    
    vkCmdBeginRenderPass(ctx->command_buffers[image_index], &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
    
    // Render video content with MPV if available
    if (mpv_has_frame) {
        // Since we don't have Vulkan support in MPV headers, 
        // we're using a placeholder for now
        // A real implementation would integrate properly with MPV's Vulkan rendering
        
        // Just log that we would render the frame
        LOG_DEBUG("MPV has frame available for rendering");
    }
    
    // End render pass
    vkCmdEndRenderPass(ctx->command_buffers[image_index]);
    
    // Apply keystone correction if enabled and compute shader is available
    keystone_t *keystone = keystone_get_config();
    if (keystone && keystone->enabled && ctx->compute.initialized) {
        LOG_DEBUG("Applying keystone correction with Vulkan compute shader");
        
        // Apply keystone correction to the rendered frame
        vulkan_compute_keystone_apply(ctx, ctx->swapchain.images[image_index], keystone);
    }
    
    // End command buffer recording
    if (vkEndCommandBuffer(ctx->command_buffers[image_index]) != VK_SUCCESS) {
        LOG_ERROR("Failed to record command buffer");
        return PICKLE_ERROR_VULKAN_COMMAND_BUFFERS;
    }
    
    // Submit the rendered frame
    return vulkan_end_frame(ctx, image_index);
}

// Check if DMA-BUF is supported
bool vulkan_is_dmabuf_supported(vulkan_ctx_t *ctx) {
    return ctx->dmabuf_supported;
}

// Import DMA-BUF
pickle_result_t vulkan_import_dmabuf(vulkan_ctx_t *ctx, int fd, uint32_t width, uint32_t height, 
                               uint32_t format, uint32_t stride, uint64_t modifier,
                               vulkan_dmabuf_info_t *dmabuf) {
    // TODO: Implement DMA-BUF import
    
    return PICKLE_OK;
}

// Destroy DMA-BUF
void vulkan_destroy_dmabuf(vulkan_ctx_t *ctx, vulkan_dmabuf_info_t *dmabuf) {
    // TODO: Implement DMA-BUF destruction
}

// Check if Vulkan is available
bool vulkan_is_available(void) {
#ifdef VULKAN_ENABLED
    // If we've already initialized a Vulkan context, use that status
    if (g_vulkan_available) {
        return true;
    }
    
    // Simple test - try to dynamically load the Vulkan library
    void* vulkan_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (vulkan_lib) {
        // Successfully loaded the Vulkan library
        dlclose(vulkan_lib);
        LOG_VULKAN("Vulkan library is available");
        return true;
    } else {
        LOG_VULKAN("Vulkan library not found: %s", dlerror());
    }
    
    return false;
#else
    LOG_VULKAN("Vulkan support not compiled in");
    return false;
#endif
}

// Get physical device name
const char* vulkan_get_device_name(vulkan_ctx_t *ctx) {
    if (!ctx || !ctx->initialized) {
        return "Unknown";
    }
    
    return ctx->device_properties.deviceName;
}

// Set vsync
void vulkan_set_vsync(vulkan_ctx_t *ctx, bool enabled) {
    g_vsync_enabled = enabled;
    
    // If we have an active swapchain, recreate it with the new vsync setting
    if (ctx && ctx->initialized && ctx->swapchain.handle != VK_NULL_HANDLE) {
        uint32_t width = ctx->swapchain.extent.width;
        uint32_t height = ctx->swapchain.extent.height;
        vulkan_recreate_swapchain(ctx, width, height);
    }
}

// Get vsync state
bool vulkan_get_vsync(vulkan_ctx_t *ctx) {
    return g_vsync_enabled;
}

// Static helper functions

// Check validation layer support
static bool check_validation_layer_support() {
    uint32_t layer_count;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);
    
    VkLayerProperties* available_layers = malloc(layer_count * sizeof(VkLayerProperties));
    vkEnumerateInstanceLayerProperties(&layer_count, available_layers);
    
    for (int i = 0; i < validationLayerCount; i++) {
        bool layer_found = false;
        
        for (uint32_t j = 0; j < layer_count; j++) {
            if (strcmp(validationLayers[i], available_layers[j].layerName) == 0) {
                layer_found = true;
                break;
            }
        }
        
        if (!layer_found) {
            free(available_layers);
            return false;
        }
    }
    
    free(available_layers);
    return true;
}

// Check device extension support
static bool check_device_extension_support(VkPhysicalDevice device) {
    uint32_t extension_count;
    vkEnumerateDeviceExtensionProperties(device, NULL, &extension_count, NULL);
    
    VkExtensionProperties* available_extensions = malloc(extension_count * sizeof(VkExtensionProperties));
    vkEnumerateDeviceExtensionProperties(device, NULL, &extension_count, available_extensions);
    
    for (int i = 0; i < deviceExtensionCount; i++) {
        bool extension_found = false;
        
        for (uint32_t j = 0; j < extension_count; j++) {
            if (strcmp(deviceExtensions[i], available_extensions[j].extensionName) == 0) {
                extension_found = true;
                break;
            }
        }
        
        if (!extension_found) {
            LOG_VULKAN("Required device extension not supported: %s", deviceExtensions[i]);
            free(available_extensions);
            return false;
        }
    }
    
    free(available_extensions);
    return true;
}

// Check if device is suitable
static bool is_device_suitable(VkPhysicalDevice device, VkSurfaceKHR surface) {
    vulkan_queue_indices_t indices = find_queue_families(device, surface);
    bool extensions_supported = check_device_extension_support(device);
    
    bool swapchain_adequate = false;
    if (extensions_supported) {
        vulkan_swapchain_support_t swapchain_support = query_swapchain_support(device, surface);
        swapchain_adequate = swapchain_support.format_count > 0 && swapchain_support.present_mode_count > 0;
        
        // Free allocated memory
        free(swapchain_support.formats);
        free(swapchain_support.present_modes);
    }
    
    VkPhysicalDeviceFeatures supported_features;
    vkGetPhysicalDeviceFeatures(device, &supported_features);
    
    return indices.graphics != UINT32_MAX && indices.present != UINT32_MAX && 
           extensions_supported && swapchain_adequate;
}

// Find queue families
static vulkan_queue_indices_t find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
    vulkan_queue_indices_t indices = {
        .graphics = UINT32_MAX,
        .present = UINT32_MAX,
        .graphics_present_same = false
    };
    
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, NULL);
    
    VkQueueFamilyProperties* queue_families = malloc(queue_family_count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families);
    
    for (uint32_t i = 0; i < queue_family_count; i++) {
        // Check for graphics support
        if (queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphics = i;
        }
        
        // Check for presentation support
        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_support);
        
        if (present_support) {
            indices.present = i;
        }
        
        // If we found both, check if they're the same
        if (indices.graphics != UINT32_MAX && indices.present != UINT32_MAX) {
            if (indices.graphics == indices.present) {
                indices.graphics_present_same = true;
            }
            break;
        }
    }
    
    free(queue_families);
    return indices;
}

// Query swapchain support
static vulkan_swapchain_support_t query_swapchain_support(VkPhysicalDevice device, VkSurfaceKHR surface) {
    vulkan_swapchain_support_t details = {0};
    
    // Get surface capabilities
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
    
    // Get surface formats
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &details.format_count, NULL);
    
    if (details.format_count > 0) {
        details.formats = malloc(details.format_count * sizeof(VkSurfaceFormatKHR));
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &details.format_count, details.formats);
    }
    
    // Get present modes
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &details.present_mode_count, NULL);
    
    if (details.present_mode_count > 0) {
        details.present_modes = malloc(details.present_mode_count * sizeof(VkPresentModeKHR));
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &details.present_mode_count, details.present_modes);
    }
    
    return details;
}

// Choose swap surface format
static VkSurfaceFormatKHR choose_swap_surface_format(const VkSurfaceFormatKHR* available_formats, uint32_t format_count) {
    for (uint32_t i = 0; i < format_count; i++) {
        if (available_formats[i].format == VK_FORMAT_B8G8R8A8_SRGB && 
            available_formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return available_formats[i];
        }
    }
    
    // If preferred format is not available, just return the first one
    return available_formats[0];
}

// Choose swap present mode
static VkPresentModeKHR choose_swap_present_mode(const VkPresentModeKHR* available_present_modes, uint32_t present_mode_count, bool vsync) {
    // If vsync is enabled, use FIFO (guaranteed to be available)
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    
    // If vsync is disabled, try to find IMMEDIATE or MAILBOX
    for (uint32_t i = 0; i < present_mode_count; i++) {
        if (available_present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
    }
    
    for (uint32_t i = 0; i < present_mode_count; i++) {
        if (available_present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    
    // Fallback to FIFO
    return VK_PRESENT_MODE_FIFO_KHR;
}

// Choose swap extent
static VkExtent2D choose_swap_extent(const VkSurfaceCapabilitiesKHR* capabilities, uint32_t width, uint32_t height) {
    if (capabilities->currentExtent.width != UINT32_MAX) {
        return capabilities->currentExtent;
    } else {
        VkExtent2D actual_extent = {width, height};
        
        actual_extent.width = MAX(capabilities->minImageExtent.width, 
                               MIN(capabilities->maxImageExtent.width, actual_extent.width));
                               
        actual_extent.height = MAX(capabilities->minImageExtent.height, 
                                MIN(capabilities->maxImageExtent.height, actual_extent.height));
                                
        return actual_extent;
    }
}

// Create shader module
static pickle_result_t create_shader_module(vulkan_ctx_t* ctx, const char* code, size_t code_size, VkShaderModule* shader_module) {
    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_size,
        .pCode = (const uint32_t*)code
    };
    
    if (vkCreateShaderModule(ctx->device, &create_info, NULL, shader_module) != VK_SUCCESS) {
        LOG_ERROR("Failed to create shader module");
        return PICKLE_ERROR_VULKAN_SHADER;
    }
    
    return PICKLE_OK;
}

// Clean up swapchain resources
static void cleanup_swapchain(vulkan_ctx_t* ctx) {
    if (!ctx || !ctx->device) {
        return;
    }
    
    // Clean up framebuffers
    if (ctx->swapchain.framebuffers) {
        for (uint32_t i = 0; i < ctx->swapchain.image_count; i++) {
            if (ctx->swapchain.framebuffers[i] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(ctx->device, ctx->swapchain.framebuffers[i], NULL);
            }
        }
        free(ctx->swapchain.framebuffers);
        ctx->swapchain.framebuffers = NULL;
    }
    
    // Clean up render pass
    if (ctx->render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);
        ctx->render_pass = VK_NULL_HANDLE;
    }
    
    // Clean up pipeline
    if (ctx->graphics_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(ctx->device, ctx->graphics_pipeline, NULL);
        ctx->graphics_pipeline = VK_NULL_HANDLE;
    }
    
    // Clean up pipeline layout
    if (ctx->pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, NULL);
        ctx->pipeline_layout = VK_NULL_HANDLE;
    }
    
    // Clean up image views
    if (ctx->swapchain.image_views) {
        for (uint32_t i = 0; i < ctx->swapchain.image_count; i++) {
            if (ctx->swapchain.image_views[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(ctx->device, ctx->swapchain.image_views[i], NULL);
            }
        }
        free(ctx->swapchain.image_views);
        ctx->swapchain.image_views = NULL;
    }
    
    // Clean up swapchain
    if (ctx->swapchain.handle != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(ctx->device, ctx->swapchain.handle, NULL);
        ctx->swapchain.handle = VK_NULL_HANDLE;
    }
    
    // Clean up images array (note: the images themselves are owned by the swapchain)
    if (ctx->swapchain.images) {
        free(ctx->swapchain.images);
        ctx->swapchain.images = NULL;
    }
    
    ctx->swapchain.image_count = 0;
}

// Debug callback
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData) {
    
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG_VULKAN("Validation layer: %s", pCallbackData->pMessage);
    }
    
    return VK_FALSE;
}

// Create debug messenger
static VkResult create_debug_utils_messenger_EXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pDebugMessenger) {
    
    PFN_vkCreateDebugUtilsMessengerEXT func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != NULL) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

// Destroy debug messenger
static void destroy_debug_utils_messenger_EXT(
    VkInstance instance,
    VkDebugUtilsMessengerEXT debugMessenger,
    const VkAllocationCallbacks* pAllocator) {
    
    PFN_vkDestroyDebugUtilsMessengerEXT func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != NULL) {
        func(instance, debugMessenger, pAllocator);
    }
}