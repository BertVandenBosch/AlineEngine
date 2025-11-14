#pragma once

#include <cstring>
#include <string.h>
#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "../core/Algorithms/Search.hpp"
#include "../core/Allocators.hpp"
#include "../core/Containers.hpp"

struct AE_Window
{
    i32 width  = -1;
    i32 height = -1;
};

class AE_Renderer final
{
    struct _RenderObjects
    {
        VkInstance   instance = VK_NULL_HANDLE;
        VkDevice     device   = VK_NULL_HANDLE;
        VkSurfaceKHR surface  = VK_NULL_HANDLE;
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

        u32 num_glfw_ext = 0u;

        const char** glfw_ext =
            glfwGetRequiredInstanceExtensions(&num_glfw_ext);

        if (num_glfw_ext == 0)
        {
            // TODO: error handling
            return;
        }

        // Get available extensions
        u32 available_ext_count = 0u;
        vkEnumerateInstanceExtensionProperties(nullptr, &available_ext_count,
                                               nullptr);

        // create array in scratch alloc with reserved size of available
        // extensions
        Array<VkExtensionProperties> available_ext(scratch,
                                                   available_ext_count);

        vkEnumerateInstanceExtensionProperties(nullptr, &available_ext_count,
                                               available_ext.Data);
        available_ext.NumElements = available_ext_count;

        constexpr char extensions_wanted[][VK_MAX_EXTENSION_NAME_SIZE] = {
            "VK_LAYER_KHRONOS_validation",
        };
        constexpr u32 num_extensions_wanted =
            sizeof(extensions_wanted) / VK_MAX_EXTENSION_NAME_SIZE;

        using ExtT = char[VK_MAX_EXTENSION_NAME_SIZE];

        Array<ExtT> req_extensions(scratch,
                                   num_extensions_wanted + num_glfw_ext);
        req_extensions.Append({.Data        = &extensions_wanted[0],
                               .NumElements = sizeof(extensions_wanted) /
                                              VK_MAX_EXTENSION_NAME_SIZE});
        for (u32 i = 0; i < num_glfw_ext; i++) {
      		strcpy_s(req_extensions.Data[req_extensions.NumElements + i], glfw_ext[i]);
        }
        req_extensions.NumElements += num_glfw_ext;

        // check if required extensions are available
        for (const ExtT& ext : req_extensions)
        {

            if (!AE::find_linear(
                    CreateConstView(available_ext), ext,
                    [](const VkExtensionProperties& ext, const ExtT& str)
                    {
                        // return memcmp(ext.extensionName, str,
                        //               VK_MAX_EXTENSION_NAME_SIZE);
                        return strcmp(ext.extensionName, str) == 0;
                    }))
            {
                return;
            }
        }

        VkApplicationInfo app_info = {
            .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pNext              = nullptr,
            .pApplicationName   = "Test app",
            .applicationVersion = VK_MAKE_API_VERSION(1, 0, 0, 0),
            .pEngineName        = "Aline Engine",
            .engineVersion      = VK_MAKE_API_VERSION(1, 0, 0, 0),
            .apiVersion         = VK_API_VERSION_1_3,
        };

        VkInstanceCreateInfo instance_ci = {
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext                   = nullptr,
            .flags                   = 0u,
            .pApplicationInfo        = &app_info,
            .enabledLayerCount       = 0,
            .ppEnabledLayerNames     = nullptr,
            .enabledExtensionCount   = num_glfw_ext,
            .ppEnabledExtensionNames = glfw_ext,
        };

        vkCreateInstance(&instance_ci, nullptr, &renderObjects.instance);

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window =
            glfwCreateWindow(512, 512, "Hello world", NULL, NULL);

        VkResult SurfaceResult = glfwCreateWindowSurface(
            renderObjects.instance, window, NULL, &renderObjects.surface);
        if (SurfaceResult != VK_SUCCESS)
        {
            return;
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
        vkDestroyInstance(renderObjects.instance, nullptr);
    }
};
