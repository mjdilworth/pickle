#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

// Function to check Vulkan hardware capabilities
int main(int argc, char *argv[]) {
    VkResult result;
    VkInstance instance;
    uint32_t deviceCount = 0;
    VkPhysicalDevice *physicalDevices = NULL;
    
    // Create Vulkan instance
    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Pickle Vulkan Hardware Check",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "No Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0
    };
    
    VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo
    };
    
    result = vkCreateInstance(&createInfo, NULL, &instance);
    if (result != VK_SUCCESS) {
        printf("ERROR: Failed to create Vulkan instance! (Result: %d)\n", result);
        return 1;
    }
    
    // Enumerate physical devices
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
    if (result != VK_SUCCESS || deviceCount == 0) {
        printf("ERROR: No Vulkan-compatible devices found!\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    physicalDevices = (VkPhysicalDevice*)malloc(sizeof(VkPhysicalDevice) * deviceCount);
    if (!physicalDevices) {
        printf("ERROR: Failed to allocate memory for physical devices!\n");
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices);
    if (result != VK_SUCCESS) {
        printf("ERROR: Failed to enumerate physical devices!\n");
        free(physicalDevices);
        vkDestroyInstance(instance, NULL);
        return 1;
    }
    
    // Print information about each device
    printf("=== VULKAN HARDWARE INFORMATION ===\n");
    printf("Found %d Vulkan-compatible device(s):\n", deviceCount);
    
    for (uint32_t i = 0; i < deviceCount; i++) {
        VkPhysicalDeviceProperties deviceProperties;
        VkPhysicalDeviceFeatures deviceFeatures;
        
        vkGetPhysicalDeviceProperties(physicalDevices[i], &deviceProperties);
        vkGetPhysicalDeviceFeatures(physicalDevices[i], &deviceFeatures);
        
        printf("\nDevice %d:\n", i);
        printf("  Name: %s\n", deviceProperties.deviceName);
        printf("  Type: %s\n", 
               deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "Integrated GPU" :
               deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "Discrete GPU" :
               deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? "Virtual GPU" :
               deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "CPU" : "Unknown");
        printf("  Vendor ID: 0x%x\n", deviceProperties.vendorID);
        printf("  Device ID: 0x%x\n", deviceProperties.deviceID);
        printf("  API Version: %d.%d.%d\n",
               VK_VERSION_MAJOR(deviceProperties.apiVersion),
               VK_VERSION_MINOR(deviceProperties.apiVersion),
               VK_VERSION_PATCH(deviceProperties.apiVersion));
        printf("  Driver Version: %d.%d.%d\n",
               VK_VERSION_MAJOR(deviceProperties.driverVersion),
               VK_VERSION_MINOR(deviceProperties.driverVersion),
               VK_VERSION_PATCH(deviceProperties.driverVersion));
        
        // Check compute shader support
        VkPhysicalDeviceProperties2 deviceProps2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
        };
        vkGetPhysicalDeviceProperties2(physicalDevices[i], &deviceProps2);
        
        // Enumerate queue families
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueFamilyCount, NULL);
        
        VkQueueFamilyProperties *queueFamilies = 
            (VkQueueFamilyProperties*)malloc(sizeof(VkQueueFamilyProperties) * queueFamilyCount);
        
        if (!queueFamilies) {
            printf("  ERROR: Failed to allocate memory for queue families!\n");
            continue;
        }
        
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueFamilyCount, queueFamilies);
        
        // Check for compute support
        int computeSupport = 0;
        for (uint32_t j = 0; j < queueFamilyCount; j++) {
            if (queueFamilies[j].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                computeSupport = 1;
                break;
            }
        }
        
        printf("  Compute Shader Support: %s\n", computeSupport ? "YES" : "NO");
        printf("  Features:\n");
        printf("    Geometry Shader: %s\n", deviceFeatures.geometryShader ? "YES" : "NO");
        printf("    Tessellation Shader: %s\n", deviceFeatures.tessellationShader ? "YES" : "NO");
        printf("    Multi Viewport: %s\n", deviceFeatures.multiViewport ? "YES" : "NO");
        
        free(queueFamilies);
    }
    
    printf("\n=== CONCLUSION ===\n");
    if (deviceCount > 0) {
        printf("Vulkan hardware acceleration is AVAILABLE.\n");
        printf("Pickle should be able to use Vulkan for keystone correction.\n");
    } else {
        printf("No Vulkan-compatible devices found!\n");
        printf("Pickle will fall back to CPU-based keystone correction.\n");
    }
    printf("================\n");
    
    // Clean up
    free(physicalDevices);
    vkDestroyInstance(instance, NULL);
    
    return 0;
}