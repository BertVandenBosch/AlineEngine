#pragma once

#include <cassert>
#include <stdlib.h>
#include <cstring>
#include <utility>

#include "core.hpp"

namespace MemoryUtils
{
    constexpr bool is_power_of_two(size_t x)
    {
	    return (x & (x-1)) == 0;
    }

    constexpr uintptr_t align_forward(uintptr_t ptr, size_t align)
    {
        uintptr_t p, a, modulo;

        assert(is_power_of_two(align));

        p = ptr;
        a = (uintptr_t)align;
        // Same as (p % a) but faster as 'a' is a power of two
        modulo = p & (a-1);

        if (modulo != 0) {
            // If 'p' address is not aligned, push the address to the
            // next value which is aligned
            p += a - modulo;
        }
        return p;
    }
} // namespace MemoryUtils


// Simple Allocator interface
class IAllocator
{
public:
    static constexpr size_t DEFAULT_ALIGNMENT = 2 * sizeof(void *);

    virtual void Init(size_t Size) = 0;
    virtual void* Allocate(size_t Size, size_t Alignment = DEFAULT_ALIGNMENT) = 0;
    virtual void Free(void* ptr) = 0;
    virtual void FreeAll() = 0;
    virtual size_t GetSize() const = 0;
    virtual void GetRawData(void* out_data, u32* out_size) = 0;

    template<typename T>
    constexpr T* Allocate(size_t Alignment = DEFAULT_ALIGNMENT)
    {
        return static_cast<T*>(Allocate(sizeof(T), Alignment));
    }

    template<typename T, class... Args>
    constexpr T* Create(Args&&... args)
    {
        static_assert(std::is_constructible_v<T, Args...>);

        void* AllocAddres = Allocate(sizeof(T));

        if (AllocAddres)
        {
            // Construct object in place
            ::new(AllocAddres) T(std::forward<Args>(args)...);
            return static_cast<T*>(AllocAddres);
        }

        return nullptr;
    }

    template<typename T, size_t _Alignment = alignof(T)>
    constexpr T* CreateArray(const u32 N)
    {
        void* AllocAddress = Allocate(sizeof(T) * N, _Alignment);

        if (AllocAddress)
        {
            // Construct the space for N elements at the AllocAddress address
            ::new (AllocAddress) T[N];
            return static_cast<T*>(AllocAddress);
        }

        return nullptr;
    }
};

//TODO: these implementions will probably only holdup for the ArenaAllocator...
template<class AllocatorA, class AllocatorB>
requires std::is_base_of_v<IAllocator, AllocatorA> && std::is_base_of_v<IAllocator, AllocatorB>
inline void CopyFrom(const AllocatorA* src, AllocatorB* dst)
{
    assert(src->buffer_offset <= dst->buffer_len);
    std::memcpy(dst->buffer, src->buffer, src->buffer_offset);
}

template<class AllocatorA, class AllocatorB>
requires std::is_base_of_v<IAllocator, AllocatorA> && std::is_base_of_v<IAllocator, AllocatorB>
inline void MoveFrom(AllocatorA* src, AllocatorB* dst)
{
    assert(src->buffer_offset <= dst->buffer_len);
    std::memmove(dst->buffer, src->buffer, src->buffer_offset);
    src->FreeAll();
}

template<size_t _Alignment = IAllocator::DEFAULT_ALIGNMENT>
class ArenaAllocator final : public IAllocator
{
public:
    u8* buffer = nullptr;
    size_t buffer_len = 0;
    size_t buffer_offset = 0;

public:
    ArenaAllocator() = default;
    explicit ArenaAllocator(size_t Size)
    {
        buffer_len = 0;
        buffer_offset = 0;

        if (Size > 0)
        {
            Init(Size);
        }
    }
    ~ArenaAllocator(){
        delete [] buffer;
    }

    void GetRawData(void* out_data, u32* out_size) override
    {
        assert(out_data != nullptr);
        out_data = buffer;
        *out_size = static_cast<u32>(buffer_offset);
    }

    void Init(size_t Size) override
    {
        buffer = static_cast<u8*>(malloc(Size));
        buffer_len = Size;
    }

    constexpr void* Allocate(size_t Size, size_t Alignment = _Alignment) override
    {
        const uintptr_t current_address = (uintptr_t)buffer + (uintptr_t)buffer_offset;
        const uintptr_t aligned_offset = MemoryUtils::align_forward(current_address, Alignment);
        const uintptr_t offset = aligned_offset - (uintptr_t)buffer; //Offset in local buffer

        if (offset + Size<= buffer_len)
        {
            void* result = &buffer[offset];
            buffer_offset += offset + Size;

            // memset(result, 0, Size);
            return result;
        }

        // OUT OF MEMORY
        return nullptr;
    }

    size_t GetSize() const override
    {
        return buffer_len;
    }

    void Free(void* ptr = nullptr) override { assert(ptr); }
    void FreeAll() override {
        buffer_offset = 0;
    };
};
