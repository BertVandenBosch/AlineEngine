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
#include "utils/types.hpp"

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
    virtual void  GetRawData(void* out_data, u32* out_size)             = 0;
    virtual void* HandleToPtr(const MemoryHandle& handle)               = 0;

    template <typename T>
    [[nodiscard]] constexpr MemoryHandle
    Allocate(T* out_obj, u32 Alignment = MemoryUtils::DEFAULT_ALIGNMENT)
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
    [[nodiscard]] constexpr MemoryHandle CreateArray(T* out_obj, const u32 N,
                                                     Args&&... args)
    {
        out_obj             = nullptr;
        MemoryHandle handle = Allocate(sizeof(T) * N, {true, _Alignment});

        if (handle.is_valid())
        {
            if constexpr (std::is_default_constructible_v<T>)
            {
                // Construct N elements in place at the AllocAddress
                // address
                void* address = HandleToPtr(handle);
                ::new (address) T[N];
                *out_obj = static_cast<remove_all_pointers_t<T>*>(address);
            }
            else
            {
                // memory footprint of a single element in the array
                constexpr size_t element_size =
                    MemoryUtils::align_forward(sizeof(T), _Alignment);

                // Individually call the constructor for each element in the
                // array
                uintptr_t address = (uintptr_t)HandleToPtr(handle);
                for (u32 i = 0; i < N; i++)
                {
                    ::new (address) T(std::forward(args)...);
                    address += element_size;
                }
                *out_obj = static_cast<remove_all_pointers_t<T>*>(HandleToPtr(handle));
            }
        }

        return handle;
    }
};

template <bool Linear>
class IAllocatorTempl : public IAllocator
{
  public:
    constexpr bool is_linear() const override { return Linear; }
};

template <size_t _Alignment = MemoryUtils::DEFAULT_ALIGNMENT>
class ArenaAllocator final : public IAllocatorTempl<true>
{
  public:
    u8*    buffer        = nullptr;
    size_t buffer_len    = 0;
    size_t buffer_offset = 0;

  public:
    constexpr ArenaAllocator() = default;
    explicit ArenaAllocator(size_t Size)
    {
        buffer_len    = 0;
        buffer_offset = 0;

        if (Size > 0)
        {
            Init(Size);
        }
    }
    ~ArenaAllocator() { delete[] buffer; }

    void GetRawData(void* out_data, u32* out_size) override
    {
        assert(out_data != nullptr);
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
            buffer_offset += offset + Size;

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

// class LinearBlockAllocator final : public IAllocatorTempl<true>
// {
//   public:
//     [[nodiscard]] explicit constexpr LinearBlockAllocator() = default;

//   public:
//     u8*    buffer     = nullptr;
//     size_t buffer_len = 0;

//     // Simple linked list of free memory blocks

//   public:
//     /* IAllocator interface begin */
//     virtual void                       Init(size_t Size)                = 0;
//     [[nodiscard]] virtual MemoryHandle Allocate(size_t        Size,
//                                                 AllocParams&& params)   = 0;
//     virtual void                       Free(const MemoryHandle& handle) = 0;
//     virtual void                       FreeAll()                        = 0;
//     virtual size_t                     GetSize() const                  = 0;
//     virtual void  GetRawData(void* out_data, u32* out_size)             = 0;
//     virtual void* HandleToPtr(const MemoryHandle& handle)               = 0;
//     /* IAllocator interface end */
// };

template <class AllocatorA, class AllocatorB>
    requires std::is_base_of_v<IAllocatorTempl<true>, AllocatorA> &&
             std::is_base_of_v<IAllocatorTempl<true>, AllocatorB>
inline void CopyFrom(const AllocatorA* src, AllocatorB* dst)
{
    assert(src->buffer_offset <= dst->buffer_len);
    std::memcpy(dst->buffer, src->buffer, src->buffer_offset);
}

template <class AllocatorA, class AllocatorB>
    requires std::is_base_of_v<IAllocatorTempl<true>, AllocatorA> &&
             std::is_base_of_v<IAllocatorTempl<true>, AllocatorB>
inline void MoveFrom(AllocatorA* src, AllocatorB* dst)
{
    assert(src->buffer_offset <= dst->buffer_len);
    std::memmove(dst->buffer, src->buffer, src->buffer_offset);
    src->FreeAll();
}
