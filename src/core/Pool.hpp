#pragma once

#include "../core/core.hpp"
#include "../core/Intrinsics.hpp"
#include "../core/Containers.hpp"
#include "../core/BitList.hpp"

template <typename HandleT, u32 INDEX_BITS,
          u32 GEN_BITS = sizeof(HandleT) * 8u - INDEX_BITS>
struct PoolHandle
{
    static constexpr u32 HANDLE_SIZE_BITS = sizeof(HandleT) * 8u;

    static constexpr u32 PAD_BITS = HANDLE_SIZE_BITS - INDEX_BITS - GEN_BITS;

    static constexpr u32 MAX_INDEX = max_uint_value<INDEX_BITS>();
    static constexpr u32 MAX_GEN   = max_uint_value<GEN_BITS>();

    static constexpr u32 MAX_GEN_WITH_PAD =
        max_uint_value<GEN_BITS + PAD_BITS>();

    static_assert(Intrinsics::find_msb(MAX_INDEX) == GEN_BITS,
                  "should add up \n");
    static_assert(Intrinsics::find_msb(MAX_GEN) == INDEX_BITS,
                  "should add up \n");

    HandleT index : INDEX_BITS;
    HandleT gen   : GEN_BITS + PAD_BITS;
};

/*
 * Pool
 *
 * PoolHandleT should be a child or instance of PoolHandle<...>
 */
template <typename T, typename PoolHandleT>
class Pool
{
  public:
    [[nodiscard]] explicit Pool(IAllocator& allocator, u32 start_size)
        : generations(allocator, start_size), freelist(allocator, start_size),
          objects(allocator, start_size)
    {
        generations.NumElements = generations._NumAllocated;
        objects.NumElements     = objects._NumAllocated;
    }

    [[nodiscard]] PoolHandleT add_element(T&& elem)
    {
        i32 free_index = freelist.find_first(false); // find open spot
        if (free_index < 0)
        {
            const u32 old_size = generations.NumElements;
            const u32 new_size = old_size * 2u;
            freelist.resize(new_size);
            generations.Resize(new_size);
            objects.Resize(new_size);

            free_index = old_size;
        }

        freelist.set_bit(free_index);
        objects[free_index] = std::move(elem);

        const u32 index = (u32)free_index & 0xFFFFFF;

        return {index, generations[free_index]};
    }

    void remove_element(const PoolHandleT& handle)
    {
        if (!is_handle_valid(handle))
        {
            return;
        }

        const u32 index = handle.index;

        freelist.unset_bit(index);
        generations[index] = (generations[index] + 1) % PoolHandleT::MAX_GEN;
    }

    const T& get_elemement(const PoolHandleT& handle) const;
    T&       update_element(const PoolHandleT& handle, T&& new_elem);

    inline bool is_handle_valid(const PoolHandleT& handle) const
    {
        const u32 index = handle.index;
        return index > 0 && index < generations.NumElements &&
               handle.gen == generations[index];
    }

    inline bool is_dirty() const { return dirty; }

  public:
    Array<u32>          generations;
    DynamicBitlist<64u> freelist;
    Array<T>            objects;

  private:
    bool dirty = false;
};
