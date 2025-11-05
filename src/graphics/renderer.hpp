#pragma once

#include "../core/Allocators.hpp"
#include "vulkan/vulkan_core.h"

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

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

        vkDestroySurfaceKHR(renderObjects.instance, renderObjects.surface,
                            NULL);
        vkDestroyInstance(renderObjects.instance, nullptr);
    }
};
