#pragma once

#include "../core/core.hpp"
#include "../core/Containers.hpp"
#include "vulkan/vulkan_core.h"

#include <vulkan/vulkan.h>

struct SwapChain
{
	u8 num_swapchains = 0u;
	VkSurfaceCapabilitiesKHR capabilities;

	u32 num_formats;
	StaticArray<VkSurfaceFormatKHR, 16> formats;

	u32 num_modes;
	StaticArray<VkPresentModeKHR, 8> present_modes;
};
