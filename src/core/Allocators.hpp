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
    class IAllocator const* owningAllocator = nullptr;

    u64 offset;
    u64 size;

    MemoryHandle* pnext = nullptr;

    inline bool is_valid() const
    {
        return owningAllocator != nullptr && size > 0u;
    }
};

// Simple Allocator interface
class IAllocator
{
  public:
    static constexpr MemoryHandle InvalidHandle = {};

    inline bool is_valid_handle(const MemoryHandle& handle) const
    {
        return handle.owningAllocator == this;
    }

  public:
    struct AllocParams
    {
        u32 bEnsureContiguousAlloc : 1  = false;
        u32 Alignment              : 31 = MemoryUtils::DEFAULT_ALIGNMENT;
    };

  public:
    virtual bool is_linear() const = 0;

    virtual void                       Init(size_t Size)                = 0;
    [[nodiscard]] virtual MemoryHandle Allocate(size_t        Size,
                                                AllocParams&& params)   = 0;
    virtual void                       Free(const MemoryHandle& handle) = 0;
    virtual void                       FreeAll()                        = 0;
    virtual size_t                     GetSize() const                  = 0;
    virtual void  GetRawData(void*& out_data, u32* out_size)            = 0;
    virtual void* HandleToPtr(const MemoryHandle& handle)               = 0;

    template <typename T>
    [[nodiscard]] constexpr MemoryHandle
    Allocate(T*& out_obj, u32 Alignment = MemoryUtils::DEFAULT_ALIGNMENT)
    {
        MemoryHandle handle = Allocate(sizeof(T), {true, Alignment});

        if (handle.is_valid()) // valid handle to a contiguous block of memory
        {
            out_obj = static_cast<T*>(HandleToPtr(handle));
        }

        return handle;
    }

    template <typename T, class... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] constexpr MemoryHandle Create(T** out_obj, Args&&... args)
    {
        MemoryHandle handle = Allocate(sizeof(T), {true});

        if (handle.is_valid())
        {
            // Construct object in place
            void* address = HandleToPtr(handle);
            ::new (address) T(std::forward<Args>(args)...);
            *out_obj = static_cast<T*>(address);
        }

        return handle;
    }

    template <typename T, size_t _Alignment = alignof(T), class... Args>
        requires std::is_constructible_v<T, Args...>
    [[nodiscard]] constexpr MemoryHandle CreateArray(T*& out_obj, const u32 N,
                                                     Args&&... args)
    {
        out_obj            = nullptr;
        MemoryHandle handle = Allocate(sizeof(T) * N, {true, _Alignment});
        assert(handle.is_valid());

        // Individually construct each element in place. (Array placement-new
        // `::new (addr) T[N]` may prepend an implementation-defined cookie and
        // overrun the allocation, so it is avoided.)
        T* base_address = static_cast<T*>(HandleToPtr(handle));
        for (u32 i = 0; i < N; i++)
        {
            ::new (static_cast<void*>(base_address + i)) T(args...);
        }
        out_obj = base_address;

        return handle;
    }
};

template <bool Linear>
class IAllocatorTempl : public IAllocator
{
  public:
    constexpr bool is_linear() const override { return Linear; }
};

template<typename TAlloc>
concept contiguous_container = std::is_base_of_v<IAllocatorTempl<true>, TAlloc>;

template <size_t _Alignment = MemoryUtils::DEFAULT_ALIGNMENT>
class ArenaAllocator final : public IAllocatorTempl<true>
{
  public:
    u8*    buffer        = nullptr;
    size_t buffer_len    = 0;
    size_t buffer_offset = 0;

  public:
    [[nodiscard]] constexpr ArenaAllocator() = default;
    [[nodiscard]] explicit ArenaAllocator(size_t Size)
    {
        buffer_len    = 0;
        buffer_offset = 0;

        if (Size > 0)
        {
            Init(Size);
        }
    }
    ~ArenaAllocator() { free(buffer);  }

    ArenaAllocator(const ArenaAllocator&)            = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void GetRawData(void*& out_data, u32* out_size) override
    {
        out_data  = buffer;
        *out_size = static_cast<u32>(buffer_offset);
    }

    void* HandleToPtr(const MemoryHandle& handle) override
    {
        if (!handle.is_valid() || handle.owningAllocator != this)
        {
            return nullptr;
        }

        return &buffer[handle.offset];
    }

    void Init(size_t Size) override
    {
        buffer     = static_cast<u8*>(malloc(Size));
        buffer_len = Size;
    }

    [[nodiscard]] constexpr MemoryHandle Allocate(size_t        Size,
                                                  AllocParams&& params) override
    {
        const uintptr_t current_address =
            (uintptr_t)buffer + (uintptr_t)buffer_offset;
        const uintptr_t aligned_offset =
            MemoryUtils::align_forward(current_address, params.Alignment);
        const uintptr_t offset =
            aligned_offset - (uintptr_t)buffer; // Offset in local buffer

        if (offset + Size <= buffer_len)
        {
            // increment buffer offset counter
            buffer_offset = offset + Size;

            memset(&buffer[offset], 0, Size);

            return {.owningAllocator = this, .offset = offset, .size = Size};
        }

        // OUT OF MEMORY
        return IAllocator::InvalidHandle;
    }

    size_t GetSize() const override { return buffer_len; }

    void Free(const MemoryHandle&) override {}
    void FreeAll() override { buffer_offset = 0; };
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

    static constexpr size_t MIN_BLOCK_SIZE =
        MemoryUtils::align_forward(sizeof(FreeBlock), MemoryUtils::DEFAULT_ALIGNMENT);

  public:
    u8*        buffer     = nullptr;
    size_t     buffer_len = 0;
    FreeBlock* freelist   = nullptr;

  public:
    [[nodiscard]] constexpr LinearBlockAllocator() = default;
    [[nodiscard]] explicit LinearBlockAllocator(size_t Size)
    {
        if (Size > 0)
        {
            Init(Size);
        }
    }
    ~LinearBlockAllocator() { free(buffer); }

    LinearBlockAllocator(const LinearBlockAllocator&)            = delete;
    LinearBlockAllocator& operator=(const LinearBlockAllocator&) = delete;

    void Init(size_t Size) override
    {
        assert(buffer == nullptr);

        Size       = MemoryUtils::align_forward(Size, MemoryUtils::DEFAULT_ALIGNMENT);
        buffer     = static_cast<u8*>(malloc(Size));
        buffer_len = Size;

        FreeAll();
    }

    [[nodiscard]] MemoryHandle Allocate(size_t Size, AllocParams&& params) override
    {
        assert(params.Alignment <= MemoryUtils::DEFAULT_ALIGNMENT);

        // Round every allocation up so any split remainder stays aligned and
        // can hold a header when freed.
        const size_t block_size = MemoryUtils::align_forward(
            Size < MIN_BLOCK_SIZE ? MIN_BLOCK_SIZE : Size,
            MemoryUtils::DEFAULT_ALIGNMENT);

        FreeBlock** link = &freelist;
        for (FreeBlock* block = freelist; block; link = &block->next, block = block->next)
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

            const size_t handed_out = remainder >= MIN_BLOCK_SIZE ? block_size : block->size;
            const u64    offset     = (u64)(reinterpret_cast<u8*>(block) - buffer);

            memset(block, 0, handed_out);

            return {.owningAllocator = this, .offset = offset, .size = handed_out};
        }

        // OUT OF MEMORY (or too fragmented)
        return IAllocator::InvalidHandle;
    }

    void Free(const MemoryHandle& handle) override
    {
        if (!handle.is_valid() || handle.owningAllocator != this)
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

        if (freed->next &&
            reinterpret_cast<u8*>(freed) + freed->size == reinterpret_cast<u8*>(freed->next))
        {
            freed->size += freed->next->size;
            freed->next = freed->next->next;
        }

        if (link != &freelist)
        {
            FreeBlock* prev = reinterpret_cast<FreeBlock*>(
                reinterpret_cast<u8*>(link) - offsetof(FreeBlock, next));
            if (reinterpret_cast<u8*>(prev) + prev->size == reinterpret_cast<u8*>(freed))
            {
                prev->size += freed->size;
                prev->next = freed->next;
            }
        }
    }

    void FreeAll() override
    {
        freelist       = reinterpret_cast<FreeBlock*>(buffer);
        freelist->size = buffer_len;
        freelist->next = nullptr;
    }

    size_t GetSize() const override { return buffer_len; }

    void GetRawData(void*& out_data, u32* out_size) override
    {
        out_data  = buffer;
        *out_size = static_cast<u32>(buffer_len);
    }

    void* HandleToPtr(const MemoryHandle& handle) override
    {
        if (!handle.is_valid() || handle.owningAllocator != this)
        {
            return nullptr;
        }

        return &buffer[handle.offset];
    }
};

template <contiguous_container AllocatorA, contiguous_container AllocatorB>
inline void CopyFrom(const AllocatorA* src, AllocatorB* dst)
{
    assert(src->buffer_offset <= dst->buffer_len);
    std::memcpy(dst->buffer, src->buffer, src->buffer_offset);
}

template <contiguous_container AllocatorA, contiguous_container AllocatorB>
inline void MoveFrom(AllocatorA* src, AllocatorB* dst)
{
    assert(src->buffer_offset <= dst->buffer_len);
    std::memmove(dst->buffer, src->buffer, src->buffer_offset);
    src->FreeAll();
}
