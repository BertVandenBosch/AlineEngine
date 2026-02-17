#pragma once

#include "../core/core.hpp"
#include "../core/Pool.hpp"

#include <vulkan/vulkan_core.h>

enum BindlessType : u8
{
    TEXTURE,
    SAMPLER,

    NUM_TYPES,
};

typedef PoolHandle<u32, 24, 8> BindlessHandle;

class BindlessHeapManager final
{
  public:
    [[nodiscard]] BindlessHeapManager(VkDevice& vulkan_device)
        : texture_heap(allocator, 64), device(vulkan_device)
    {
    }

    void update_descriptorsets();

    // TODO(bert): figure out how to do this best: as in do we bind this using a
    // unique layout or do we want to append to other temporal cohese layout
    // when binding?
    void bind_descriptorsets(VkCommandBuffer     cmdBuffer,
                             VkPipelineBindPoint bindpoint)
    {
    }
    // {
    // vkCmdBindDescriptorSets(cmdBuffer, bindpoint, VkPipelineLayout layout,
    // uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet
    // *pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t
    // *pDynamicOffsets)
    // }

  public:
    Pool<struct Texture, BindlessHandle> texture_heap;

  private:
    ArenaAllocator allocator = ArenaAllocator(MB(256));

    // vulkan objects
    VkDevice& device;

    // double-buffered round robin descriptor sets
    VkDescriptorSet descriptor_set[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
};
