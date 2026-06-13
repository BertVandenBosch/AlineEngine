#pragma once

#include "../core/core.hpp"
#include "../core/Containers.hpp"
#include "core/Allocators.hpp"
#include "vulkan/vulkan_core.h"

#include <vulkan/vulkan.h>

struct TSwapChain
{
	// TODO(Bert): suballocate from general renderer setup allocator?
	// TODO(Bert): this is probably way too much, trim after runtime inspection
	ArenaAllocator<> _inline_allocator = ArenaAllocator<>(KB(512u));

	u8 num_swapchains = 0u;
	VkSurfaceCapabilitiesKHR capabilities;

	u32 num_formats;
	Array<VkSurfaceFormatKHR> formats = Array<VkSurfaceFormatKHR>(_inline_allocator);

	u32 num_modes;
	Array<VkPresentModeKHR> present_modes = Array<VkPresentModeKHR>(_inline_allocator);
};
