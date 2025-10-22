#pragma once

#include "../core/Containers.hpp"
#include "../core/Allocators.hpp"

#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>

struct AE_Window
{
    i32 width     = -1;
    i32 height    = -1;
};


class AE_Renderer final
{
    struct _RenderObjects
    {
        vk::Instance instance   = VK_NULL_HANDLE;
        vk::Device device       = VK_NULL_HANDLE;
    };

public:
    _RenderObjects renderObjects;

public:
    void init_renderer()
    {
        if (!glfwInit()) {
            //TODO(bert): handle glfw errors;
            return;
        }

        // scratch arena allocator for throw away data
        ArenaAllocator<> scratch = ArenaAllocator<>(KB(512u));

        u32 num_glfw_ext = 0u;
        const char** glfw_ext = glfwGetRequiredInstanceExtensions(&num_glfw_ext);

        if (num_glfw_ext == 0) {
            //TODO: error handling
            return;
        }

        vk::ApplicationInfo app_info =
        {
            .pApplicationName = "Test app",
            .applicationVersion = vk::makeApiVersion(1, 0, 0, 0),
            .pEngineName        = "Aline Engine",
            .engineVersion      = vk::makeApiVersion(1, 0, 0, 0),
            .apiVersion         = vk::ApiVersion13
        };

        vk::InstanceCreateInfo instance_ci =
        {
            .pApplicationInfo = &app_info,
            .enabledExtensionCount = num_glfw_ext,
            .ppEnabledExtensionNames = glfw_ext
        };

        renderObjects.instance = vk::createInstance(instance_ci);

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window = glfwCreateWindow(512, 512, "Hello world", NULL, NULL);

        renderObjects.instance.destroy();
    }
};
