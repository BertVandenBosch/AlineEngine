#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdlib.h>
#include <sys/stat.h>
#include <type_traits>
#include <utility>

#include "core.hpp"

namespace MemoryUtils
{

static constexpr u32 DEFAULT_ALIGNMENT = 2 * sizeof(void*);

constexpr uintptr_t align_forward(uintptr_t ptr, size_t align)
{
    uintptr_t p, a, modulo;

    assert(is_power_of_two(align));

    p      = ptr;
    a      = (uintptr_t)align;
    // Same as (p % a) but faster as 'a' is a power of two
    modulo = p & (a - 1);

    if (modulo != 0)
    {
        // If 'p' address is not aligned, push the address to the
        // next value which is aligned
        p += a - modulo;
    }
    return p;
}
} // namespace MemoryUtils

struct MemoryHandle
{
    class IAllocator const* owning_allocator = nullptr;

    u64 offset;
    u64 size;

    MemoryHandle* pnext = nullptr;

    inline bool is_valid() const
    {
        return owning_allocator != nullptr && size > 0u;
    }
};

// Simple Allocator interface
class IAllocator
{
  public:
    static constexpr MemoryHandle invalid_handle = {};

    inline bool is_valid_handle(const MemoryHandle& handle) const
    {
        return handle.owning_allocator == this;
    }

  public:
    struct AllocParams
    {
        u32 ensure_contiguous_alloc : 1  = false;
        u32 alignment               : 31 = MemoryUtils::DEFAULT_ALIGNMENT;
    };

  public:
    virtual bool is_linear() const = 0;

    virtual void                       init(size_t size)                = 0;
    [[nodiscard]] virtual MemoryHandle allocate(size_t        size,
                                                AllocParams&& params)   = 0;
    virtual void                       free(const MemoryHandle& handle) = 0;
    virtual void                       free_all()                       = 0;
    virtual size_t                     get_size() const                 = 0;
    virtual void  get_raw_data(void*& out_data, u32* out_size)          = 0;
    virtual void* handle_to_ptr(const MemoryHandle& handle)             = 0;

    template <typename T>
    [[nodiscard]] constexpr MemoryHandle
    allocate(T*& out_obj, u32 alignment = MemoryUtils::DEFAULT_ALIGNMENT)
    {
        MemoryHandle handle = allocate(sizeof(T), {true, alignment});

        if (handle.is_valid()) // valid handle to a contiguous block of memory
        {
            out_obj = static_cast<T*>(handle_to_ptr(handle));
        }

        return handle;
    }

    template <typename T, class... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] constexpr MemoryHandle create(T** out_obj, Args&&... args)
    {
        MemoryHandle handle = allocate(sizeof(T), {true});

        if (handle.is_valid())
        {
            // Construct object in place
            void* address = handle_to_ptr(handle);
            ::new (address) T(std::forward<Args>(args)...);
            *out_obj = static_cast<T*>(address);
        }

        return handle;
    }

    template <typename T, size_t default_alignment = alignof(T), class... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] constexpr MemoryHandle create_array(T*& out_obj, const u32 N,
                                                      Args&&... args)
    {
        out_obj = nullptr;
        MemoryHandle handle =
            allocate(sizeof(T) * N, {true, default_alignment});
        assert(handle.is_valid());

        // Individually construct each element in place. (Array placement-new
        // `::new (addr) T[N]` may prepend an implementation-defined cookie and
        // overrun the allocation, so it is avoided.)
        T* base_address = static_cast<T*>(handle_to_ptr(handle));
        for (u32 i = 0; i < N; i++)
        {
            ::new (static_cast<void*>(base_address + i)) T(args...);
        }
        out_obj = base_address;

        return handle;
    }
};

template <bool linear>
class IAllocatorTempl : public IAllocator
{
  public:
    constexpr bool is_linear() const override { return linear; }
};

template <typename TAlloc>
concept contiguous_container = std::is_base_of_v<IAllocatorTempl<true>, TAlloc>;

template <size_t default_alignment = MemoryUtils::DEFAULT_ALIGNMENT>
class ArenaAllocator final : public IAllocatorTempl<true>
{
  public:
    u8*    buffer        = nullptr;
    size_t buffer_len    = 0;
    size_t buffer_offset = 0;

  public:
    [[nodiscard]] constexpr ArenaAllocator() = default;
    [[nodiscard]] explicit ArenaAllocator(size_t size)
    {
        buffer_len    = 0;
        buffer_offset = 0;

        if (size > 0)
        {
            init(size);
        }
    }
    ~ArenaAllocator() { ::free(buffer); }

    ArenaAllocator(const ArenaAllocator&)            = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void get_raw_data(void*& out_data, u32* out_size) override
    {
        out_data  = buffer;
        *out_size = static_cast<u32>(buffer_offset);
    }

    void* handle_to_ptr(const MemoryHandle& handle) override
    {
        if (!handle.is_valid() || handle.owning_allocator != this)
        {
            return nullptr;
        }

        return &buffer[handle.offset];
    }

    void init(size_t size) override
    {
        buffer     = static_cast<u8*>(malloc(size));
        buffer_len = size;
    }

    [[nodiscard]] constexpr MemoryHandle allocate(size_t        size,
                                                  AllocParams&& params) override
    {
        const uintptr_t current_address =
            (uintptr_t)buffer + (uintptr_t)buffer_offset;
        const uintptr_t aligned_offset =
            MemoryUtils::align_forward(current_address, params.alignment);
        const uintptr_t offset =
            aligned_offset - (uintptr_t)buffer; // Offset in local buffer

        if (offset + size <= buffer_len)
        {
            // increment buffer offset counter
            buffer_offset = offset + size;

            memset(&buffer[offset], 0, size);

            return {.owning_allocator = this, .offset = offset, .size = size};
        }

        // OUT OF MEMORY
        return IAllocator::invalid_handle;
    }

    size_t get_size() const override { return buffer_len; }

    void free(const MemoryHandle&) override {}
    void free_all() override { buffer_offset = 0; };
};

/*
 * First-fit freelist allocator with real Free() and neighbor coalescing.
 * For allocations with unbounded/mixed lifetimes where an arena's
 * leak-on-free is not acceptable (e.g. resource blobs freed on destroy).
 *
 * The freelist is intrusive: block headers live inside the free memory
 * itself, so the allocator needs no side storage. Every offset it hands out
 * is aligned to DEFAULT_ALIGNMENT; requested alignments above that are not
 * supported (asserted).
 */
class LinearBlockAllocator final : public IAllocatorTempl<false>
{
    struct FreeBlock
    {
        size_t     size; // includes the header itself
        FreeBlock* next; // next free block, sorted by address
    };

    static constexpr size_t MIN_BLOCK_SIZE = MemoryUtils::align_forward(
        sizeof(FreeBlock), MemoryUtils::DEFAULT_ALIGNMENT);

  public:
    u8*        buffer     = nullptr;
    size_t     buffer_len = 0;
    FreeBlock* freelist   = nullptr;

  public:
    [[nodiscard]] constexpr LinearBlockAllocator() = default;
    [[nodiscard]] explicit LinearBlockAllocator(size_t size)
    {
        if (size > 0)
        {
            init(size);
        }
    }
    ~LinearBlockAllocator() { ::free(buffer); }

    LinearBlockAllocator(const LinearBlockAllocator&)            = delete;
    LinearBlockAllocator& operator=(const LinearBlockAllocator&) = delete;

    void init(size_t size) override
    {
        assert(buffer == nullptr);

        size = MemoryUtils::align_forward(size, MemoryUtils::DEFAULT_ALIGNMENT);
        buffer     = static_cast<u8*>(malloc(size));
        buffer_len = size;

        free_all();
    }

    [[nodiscard]] MemoryHandle allocate(size_t        size,
                                        AllocParams&& params) override
    {
        assert(params.alignment <= MemoryUtils::DEFAULT_ALIGNMENT);

        // Round every allocation up so any split remainder stays aligned and
        // can hold a header when freed.
        const size_t block_size = MemoryUtils::align_forward(
            size < MIN_BLOCK_SIZE ? MIN_BLOCK_SIZE : size,
            MemoryUtils::DEFAULT_ALIGNMENT);

        FreeBlock** link = &freelist;
        for (FreeBlock* block = freelist; block;
             link = &block->next, block = block->next)
        {
            if (block->size < block_size)
            {
                continue;
            }

            const size_t remainder = block->size - block_size;
            if (remainder >= MIN_BLOCK_SIZE)
            {
                // Split: the tail of this block stays free.
                FreeBlock* tail = reinterpret_cast<FreeBlock*>(
                    reinterpret_cast<u8*>(block) + block_size);
                tail->size = remainder;
                tail->next = block->next;
                *link      = tail;
            }
            else
            {
                *link = block->next;
            }

            const size_t handed_out =
                remainder >= MIN_BLOCK_SIZE ? block_size : block->size;
            const u64 offset = (u64)(reinterpret_cast<u8*>(block) - buffer);

            memset(block, 0, handed_out);

            return {
                .owning_allocator = this, .offset = offset, .size = handed_out};
        }

        // OUT OF MEMORY (or too fragmented)
        return IAllocator::invalid_handle;
    }

    void free(const MemoryHandle& handle) override
    {
        if (!handle.is_valid() || handle.owning_allocator != this)
        {
            return;
        }

        FreeBlock* freed = reinterpret_cast<FreeBlock*>(&buffer[handle.offset]);
        freed->size      = handle.size;

        // Insert sorted by address, then coalesce with both neighbors.
        FreeBlock** link = &freelist;
        while (*link && *link < freed)
        {
            link = &(*link)->next;
        }
        freed->next = *link;
        *link       = freed;

        if (freed->next && reinterpret_cast<u8*>(freed) + freed->size ==
                               reinterpret_cast<u8*>(freed->next))
        {
            freed->size += freed->next->size;
            freed->next = freed->next->next;
        }

        if (link != &freelist)
        {
            FreeBlock* prev = reinterpret_cast<FreeBlock*>(
                reinterpret_cast<u8*>(link) - offsetof(FreeBlock, next));
            if (reinterpret_cast<u8*>(prev) + prev->size ==
                reinterpret_cast<u8*>(freed))
            {
                prev->size += freed->size;
                prev->next = freed->next;
            }
        }
    }

    void free_all() override
    {
        freelist       = reinterpret_cast<FreeBlock*>(buffer);
        freelist->size = buffer_len;
        freelist->next = nullptr;
    }

    size_t get_size() const override { return buffer_len; }

    void get_raw_data(void*& out_data, u32* out_size) override
    {
        out_data  = buffer;
        *out_size = static_cast<u32>(buffer_len);
    }

    void* handle_to_ptr(const MemoryHandle& handle) override
    {
        if (!handle.is_valid() || handle.owning_allocator != this)
        {
            return nullptr;
        }

        return &buffer[handle.offset];
    }
};

template <contiguous_container AllocatorA, contiguous_container AllocatorB>
inline void copy_from(const AllocatorA* src, AllocatorB* dst)
{
    assert(src->buffer_offset <= dst->buffer_len);
    std::memcpy(dst->buffer, src->buffer, src->buffer_offset);
}

template <contiguous_container AllocatorA, contiguous_container AllocatorB>
inline void move_from(AllocatorA* src, AllocatorB* dst)
{
    assert(src->buffer_offset <= dst->buffer_len);
    std::memmove(dst->buffer, src->buffer, src->buffer_offset);
    src->free_all();
}
