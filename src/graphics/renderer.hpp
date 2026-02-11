#pragma once

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string.h>
#include <utility>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "../core/Algorithms/Search.hpp"
#include "../core/Allocators.hpp"
#include "../core/Containers.hpp"
#include "vulkan/vulkan_core.h"

struct AE_Window
{
    i32 width  = -1;
    i32 height = -1;
};

class AE_Renderer final
{
    struct _RenderObjects
    {
        VkInstance       instance        = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkSurfaceKHR     surface         = VK_NULL_HANDLE;
        VkDevice         device          = VK_NULL_HANDLE;

        VkPhysicalDeviceLimits limits = {};

        VkQueue graphics_queue = VK_NULL_HANDLE;
        VkQueue compute_queue  = VK_NULL_HANDLE;
        VkQueue transfer_queue = VK_NULL_HANDLE;
    };

  public:
    _RenderObjects renderObjects;

  public:
    void init_renderer()
    {
        if (!glfwInit())
        {
            // TODO(bert): handle glfw errors;
            return;
        }

        // scratch arena allocator for throw away data
        ArenaAllocator<> scratch = ArenaAllocator<>(KB(512u));

        u32 num_glfw_instance_ext = 0u;

        const char** glfw_instance_ext =
            glfwGetRequiredInstanceExtensions(&num_glfw_instance_ext);

        if (num_glfw_instance_ext == 0)
        {
            // TODO: error handling
            return;
        }

        // Get available extensions
        u32 available_instance_ext_count = 0u;
        vkEnumerateInstanceExtensionProperties(nullptr, &available_instance_ext_count,
                                               nullptr);

        // create array in scratch alloc with reserved size of available
        // extensions
        Array<VkExtensionProperties> available_instance_ext(scratch,
                                                   available_instance_ext_count);

        vkEnumerateInstanceExtensionProperties(nullptr, &available_instance_ext_count,
                                               available_instance_ext.Data);
        available_instance_ext.NumElements = available_instance_ext_count;


        // constexpr u32 num_instance_extensions_wanted =
        //     sizeof(extensions_wanted) / VK_MAX_EXTENSION_NAME_SIZE;

        // using ExtT = char[VK_MAX_EXTENSION_NAME_SIZE];

        // Array<ExtT> req_instance_extensions(scratch,
        //                            num_instance_extensions_wanted + num_glfw_instance_ext);
        // req_instance_extensions.Append({.Data        = &extensions_wanted[0],
        //                        .NumElements = sizeof(extensions_wanted) /
        //                                       VK_MAX_EXTENSION_NAME_SIZE});
        // for (u32 i = 0; i < num_glfw_instance_ext; i++)
        // {
        //     strcpy_s(req_instance_extensions.Data[req_instance_extensions.NumElements + i],
        //              glfw_instance_ext[i]);
        // }
        // req_instance_extensions.NumElements += num_glfw_instance_ext;

        // // check if required extensions are available
        // for (const ExtT& ext : req_instance_extensions)
        // {
        //     if (!AE::find_linear(
        //             CreateConstView(available_instance_ext), ext,
        //             [](const VkExtensionProperties& ext, const ExtT& str)
        //             { return strcmp(ext.extensionName, str) == 0; }))
        //     {
        //         return;
        //     }
        // }

        VkApplicationInfo app_info = {
            .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext              = nullptr,
            .pApplicationName   = "Test app",
            .applicationVersion = VK_MAKE_API_VERSION(1, 0, 0, 0),
            .pEngineName        = "Aline Engine",
            .engineVersion      = VK_MAKE_API_VERSION(0, 0, 0, 1),
            .apiVersion         = VK_API_VERSION_1_3,
        };

        VkInstanceCreateInfo instance_ci = {
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0u,
            .pApplicationInfo        = &app_info,
            .enabledLayerCount       = 0,
            .ppEnabledLayerNames     = nullptr,
            .enabledExtensionCount   = num_glfw_instance_ext,
            .ppEnabledExtensionNames = glfw_instance_ext,
        };

        VkResult InstanceResult = vkCreateInstance(&instance_ci, nullptr, &renderObjects.instance);
        assert(InstanceResult == VK_SUCCESS);

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window =
            glfwCreateWindow(512, 512, "Hello world", NULL, NULL);

        VkResult SurfaceResult = glfwCreateWindowSurface(
            renderObjects.instance, window, NULL, &renderObjects.surface);
        if (SurfaceResult != VK_SUCCESS)
        {
            return;
        }

        // Vulkan logical device features
        VkPhysicalDeviceVulkan13Features features_13 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = nullptr,
            // .subgroupSizeControl  = true,
            // .computeFullSubgroups = true,
            // .synchronization2     = true,
            // .dynamicRendering     = true,
        };

        VkPhysicalDeviceVulkan12Features features_12 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &features_13,
            // .shaderFloat16                                      = true,
            // .descriptorIndexing                                 = true,
            // .descriptorBindingUniformBufferUpdateAfterBind      = true,
            // .descriptorBindingSampledImageUpdateAfterBind       = true,
            // .descriptorBindingStorageImageUpdateAfterBind       = true,
            // .descriptorBindingStorageBufferUpdateAfterBind      = true,
            // .descriptorBindingUniformTexelBufferUpdateAfterBind = true,
            // .descriptorBindingStorageTexelBufferUpdateAfterBind = true,
            // .descriptorBindingUpdateUnusedWhilePending          = true,
            // .descriptorBindingPartiallyBound                    = true,
            // .descriptorBindingVariableDescriptorCount           = true,
            // .runtimeDescriptorArray                             = true,
            // .bufferDeviceAddress                                = true,
            // .bufferDeviceAddressCaptureReplay                   = true,
        };

        VkPhysicalDeviceVulkan11Features features_11 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .pNext = &features_12,
        };

        VkPhysicalDeviceFeatures features_10 = {
            .robustBufferAccess         = true,
            .fullDrawIndexUint32        = true,
            .imageCubeArray             = true,
            .independentBlend           = true,
            .logicOp                    = true,
            .multiDrawIndirect          = true,
            .drawIndirectFirstInstance  = true,
            .multiViewport              = true,
            .textureCompressionETC2     = true,
            .textureCompressionASTC_LDR = true,
            .textureCompressionBC       = true,
            .shaderImageGatherExtended  = true,
            .shaderInt64                = true,
            .shaderInt16                = true,
        };

        // VkPhysicalDeviceFeatures2 device_features = {
        //     .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        //     .pNext    = &features_11,
        //     .features = features_10,
        // };

        VkPhysicalDeviceFeatures device_features =
        {
        	.multiDrawIndirect = true,
        };

        // query available devices
        u32 num_devices = 0u;
        vkEnumeratePhysicalDevices(renderObjects.instance, &num_devices,
                                   nullptr);

        if (num_devices == 0u)
        {
            return;
        }

        constexpr u32 MAX_DEVICES = 4u;
        assert(num_devices <= MAX_DEVICES);
        StaticArray<VkPhysicalDevice, MAX_DEVICES> available_devices;
        // available_devices.NumElements = num_devices;
        vkEnumeratePhysicalDevices(renderObjects.instance, &num_devices,
                                   available_devices.Data);

        i8 device_idx = get_valid_physical_device(
            CreateView(available_devices, num_devices));
        if (device_idx == -1)
        {
            return;
        }

        renderObjects.physical_device = available_devices[device_idx];

        // QUEUES
        u32 num_queues = 0u;
        vkGetPhysicalDeviceQueueFamilyProperties(renderObjects.physical_device,
                                                 &num_queues, nullptr);

        Array<VkQueueFamilyProperties> queue_properties(scratch, num_queues);
        queue_properties.NumElements = num_queues;

        vkGetPhysicalDeviceQueueFamilyProperties(
            renderObjects.physical_device, &num_queues, queue_properties.Data);

        i32 graphics_q_idx = -1;
        i32 compute_q_idx  = -1;
        i32 transfer_q_idx = -1;

        for (u32 i = 0u; i < num_queues; i++)
        {
            if (queue_properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT &&
                graphics_q_idx < 0)
            {
                graphics_q_idx = i;
                continue;
            }
            if (queue_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            {
                compute_q_idx = i;
                continue;
            }
            if (queue_properties[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
            {
                transfer_q_idx = i;
            }
        }

        // dedicated compute queue not required as my gpu does not support it :(
        assert(graphics_q_idx >= 0 && transfer_q_idx >= 0);

        float queue_priority = 0.0f;
        Array<VkDeviceQueueCreateInfo> queues_ci(scratch, 2);
        {
            {
                VkDeviceQueueCreateInfo queue_ci = {
                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue_ci.queueCount       = 1;
                queue_ci.queueFamilyIndex = graphics_q_idx;
                queue_ci.pQueuePriorities = &queue_priority;
                queues_ci.Add(std::move(queue_ci));
            }
            if (compute_q_idx >= 0)
            {
                VkDeviceQueueCreateInfo queue_ci = {
                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue_ci.queueCount       = 1;
                queue_ci.queueFamilyIndex = compute_q_idx;
                queue_ci.pQueuePriorities = &queue_priority;
                queues_ci.Add(std::move(queue_ci));
            }
            {
                VkDeviceQueueCreateInfo queue_ci = {
                    VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue_ci.queueCount       = 1;
                queue_ci.queueFamilyIndex = transfer_q_idx;
                queue_ci.pQueuePriorities = &queue_priority;
                queues_ci.Add(std::move(queue_ci));
            }
        }

        //test manual extension array smh
        // const char* ext_manual[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};

        // const char* extensions_wanted[] = {
        //     "VK_LAYER_KHRONOS_validation",
        // };

        // Creating logical device
        VkDeviceCreateInfo device_ci = {
            .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            // .pNext                   = &device_features,
            .queueCreateInfoCount    = queues_ci.NumElements,
            .pQueueCreateInfos       = queues_ci.Data,
            // .enabledExtensionCount   = req_extensions.NumElements,
            // .ppEnabledExtensionNames = req_ext_char_arr.Data,
            // .enabledExtensionCount = 0,
            // .ppEnabledExtensionNames = nullptr,
            .pEnabledFeatures = &device_features
        };


        VkResult create_device_result = vkCreateDevice(renderObjects.physical_device, &device_ci, nullptr,
                       &renderObjects.device);

        assert(create_device_result == VK_SUCCESS);

        // Getting the queue object handles
        {
            vkGetDeviceQueue(renderObjects.device, graphics_q_idx, 0,
                             &renderObjects.graphics_queue);

            if (compute_q_idx >= 0)
            {
                vkGetDeviceQueue(renderObjects.device, compute_q_idx, 0,
                                 &renderObjects.compute_queue);
            }

            vkGetDeviceQueue(renderObjects.device, transfer_q_idx, 0,
                             &renderObjects.transfer_queue);
        }

        while (!glfwWindowShouldClose(window))
        {
            glfwSwapBuffers(window);
            glfwPollEvents();
        }

        // CLEANUP
        glfwDestroyWindow(window);

        vkDestroySurfaceKHR(renderObjects.instance, renderObjects.surface,
                            NULL);

        vkDestroyDevice(renderObjects.device, nullptr);
        vkDestroyInstance(renderObjects.instance, nullptr);
    }

  private:
    i8 get_valid_physical_device(const View<VkPhysicalDevice>&& devices);
};
