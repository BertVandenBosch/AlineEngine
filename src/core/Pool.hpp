#pragma once

#include "../core/core.hpp"
#include "../core/Intrinsics.hpp"
#include "../core/Containers.hpp"
#include "../core/BitList.hpp"

/*
 * Generational handle into a Pool.
 *
 * gen == 0 is reserved as "empty / never created": a default-constructed
 * handle is never valid, and stored generations start at 1 and skip 0 when
 * they wrap.
 *
 * The Tag parameter makes otherwise identical handle layouts distinct types,
 * so APIs handing out handles to different resources get type safety and
 * overload resolution for free (e.g. one handle type per GPU resource kind).
 */
template <typename HandleT, u32 INDEX_BITS,
          u32 GEN_BITS = sizeof(HandleT) * 8u - INDEX_BITS, typename Tag = void>
struct PoolHandle
{
    static constexpr u32 HANDLE_SIZE_BITS = sizeof(HandleT) * 8u;

    static constexpr u32 PAD_BITS = HANDLE_SIZE_BITS - INDEX_BITS - GEN_BITS;

    static constexpr HandleT MAX_INDEX = max_uint_value<INDEX_BITS, HandleT>();
    static constexpr HandleT MAX_GEN   = max_uint_value<GEN_BITS, HandleT>();

    static_assert(INDEX_BITS > 0u, "handle needs index bits");
    static_assert(GEN_BITS > 0u, "handle needs generation bits");
    static_assert(INDEX_BITS + GEN_BITS <= HANDLE_SIZE_BITS,
                  "index + generation bits exceed the handle type");

    HandleT index : INDEX_BITS          = 0;
    HandleT gen   : GEN_BITS + PAD_BITS = 0;

    constexpr bool empty() const { return gen == 0u; }
    constexpr bool valid() const { return gen != 0u; }

    constexpr explicit operator bool() const { return gen != 0u; }

    // The raw index/whole handle as an opaque pointer-sized value, for APIs
    // that traffic in void* ids (bindless indices, UI texture ids).
    void* index_as_void() const
    {
        return reinterpret_cast<void*>((uintptr_t)index);
    }
    void* handle_as_void() const
    {
        static_assert(sizeof(PoolHandle) <= sizeof(void*),
                      "handle does not fit a pointer");
        void* result = nullptr;
        memcpy(&result, this, sizeof(PoolHandle));
        return result;
    }

    constexpr bool operator==(const PoolHandle& other) const
    {
        return index == other.index && gen == other.gen;
    }
    constexpr bool operator!=(const PoolHandle& other) const
    {
        return !(*this == other);
    }
};

/*
 * Pool
 *
 * Generational object pool over a dense array: `objects[handle.index]`.
 * PoolHandleT should be a PoolHandle<...> instantiation (or
 * layout-compatible with one). Index 0 is a normal, valid slot.
 *
 * The dense array grows lazily: `objects.num_elements` is the high-water mark
 * of slots ever created, not the reserved capacity. Consumers may iterate the
 * dense array; freed slots keep their zeroed (T{}) payload.
 */
template <typename T, typename PoolHandleT>
class Pool
{
  public:
    [[nodiscard]] explicit Pool(IAllocator& allocator, u32 start_size)
        : generations(allocator, start_size),
          freelist(allocator, num_chunks_for(start_size)),
          objects(allocator, start_size), capacity_(start_size)
    {
        assert(start_size > 0u);
    }

    [[nodiscard]] PoolHandleT add_element(T&& elem)
    {
        // lowest free bit: a previously freed slot, or the first
        // never-created one (freed and never-created slots are both 0)
        const i32 found = freelist.find_first(false);

        u32 index = found < 0 ? objects.num_elements : (u32)found;
        if (index > objects.num_elements)
        {
            // chunk-padding bits past the high-water mark also read as free
            index = objects.num_elements;
        }

        if (index >= capacity_)
        {
            const u32 new_capacity = capacity_ * 2u;
            assert(new_capacity <= PoolHandleT::MAX_INDEX);

            generations.reserve(new_capacity);
            objects.reserve(new_capacity);
            freelist.resize(new_capacity);
            capacity_ = new_capacity;
        }

        if (index == objects.num_elements)
        {
            // never-created slot: extend the dense array (payload memory is
            // zero-initialized by the allocator) and seed its generation
            objects.resize(index + 1u);
            generations.resize(index + 1u);
            generations[index] = 1u;
        }

        freelist.set_bit(index);
        objects[index] = std::move(elem);
        num_alive_++;

        PoolHandleT handle{};
        handle.index = index;
        handle.gen   = generations[index];
        return handle;
    }

    void remove_element(const PoolHandleT& handle)
    {
        if (!is_handle_valid(handle))
        {
            return;
        }

        const u32 index = (u32)handle.index;

        freelist.unset_bit(index);
        objects[index] = T{}; // consumers rely on dead slots being zeroed

        // bump the generation, skipping the reserved 0
        u32 next_gen = generations[index] + 1u;
        if (next_gen > PoolHandleT::MAX_GEN || next_gen == 0u)
        {
            next_gen = 1u;
        }
        generations[index] = next_gen;

        num_alive_--;
    }

    // ---------------- create/get/destroy surface ----------------

    [[nodiscard]] PoolHandleT create(T&& elem)
    {
        return add_element(std::move(elem));
    }

    void destroy(const PoolHandleT& handle) { remove_element(handle); }

    T* get(const PoolHandleT& handle)
    {
        return is_handle_valid(handle) ? &objects[(u32)handle.index] : nullptr;
    }

    const T* get(const PoolHandleT& handle) const
    {
        return is_handle_valid(handle) ? &objects[(u32)handle.index] : nullptr;
    }

    u32 num_objects() const { return num_alive_; }

    void clear()
    {
        for (u32 i = 0; i < generations.num_elements; i++)
        {
            if (!freelist[i])
            {
                continue;
            }

            freelist.unset_bit(i);
            objects[i] = T{};

            u32 next_gen = generations[i] + 1u;
            if (next_gen > PoolHandleT::MAX_GEN || next_gen == 0u)
            {
                next_gen = 1u;
            }
            generations[i] = next_gen;
        }
        num_alive_ = 0u;
    }

    // ----------------------------------------------------

    const T& get_element(const PoolHandleT& handle) const
    {
        assert(is_handle_valid(handle));
        return objects[(u32)handle.index];
    }

    T& update_element(const PoolHandleT& handle, T&& new_elem)
    {
        assert(is_handle_valid(handle));

        T& slot = objects[(u32)handle.index];
        slot    = std::move(new_elem);
        return slot;
    }

    inline bool is_handle_valid(const PoolHandleT& handle) const
    {
        const u32 index = (u32)handle.index;
        // stored generations are >= 1, so an empty handle (gen 0) or a stale
        // one can never match
        return index < generations.num_elements &&
               (u32)handle.gen == generations[index];
    }

    inline bool is_dirty() const { return dirty; }

  public:
    Array<u32>          generations;
    DynamicBitlist<64u> freelist;
    Array<T>            objects;

  private:
    static constexpr u32 num_chunks_for(u32 num_elements)
    {
        constexpr u32 bits_per_chunk = DynamicBitlist<64u>::bits_per_chunk;
        return (num_elements + bits_per_chunk - 1u) / bits_per_chunk;
    }

    void seed_generations(u32 from, u32 to)
    {
        for (u32 i = from; i < to; i++)
        {
            generations[i] = 1u;
        }
    }

    u32 capacity_ =
        0u; // reserved slots; objects.num_elements is the high-water mark
    u32  num_alive_ = 0u;
    bool dirty      = false;
};
