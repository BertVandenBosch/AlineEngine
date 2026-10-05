#pragma once

#include "Allocators.hpp"
#include "core.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <type_traits>

/*
 * Non-owning span over contiguous elements. Kept an aggregate so
 * designated-initializer construction keeps working.
 *
 * The lowercase size()/data()/empty() accessors mirror std::span's surface so
 * span-shaped code can consume Views without a
 * rename pass.
 */
template <typename T>
struct View
{
    T*  Data        = nullptr;
    u32 NumElements = 0;

    inline T* begin() { return Data; }
    inline T* end() { return Data + NumElements; }

    inline const T* begin() const { return Data; }
    inline const T* end() const { return Data + NumElements; }

    inline const T* cbegin() const { return Data; }
    inline const T* cend() const { return Data + NumElements; }

    inline constexpr u32  size() const { return NumElements; }
    inline constexpr T*   data() const { return Data; }
    inline constexpr bool empty() const { return NumElements == 0u; }

    inline const T& operator[](const u32 _idx) const
    {
        assert(_idx < NumElements);
        return Data[_idx];
    }

    inline T& operator[](const u32 _idx)
    {
        assert(_idx < NumElements);
        return Data[_idx];
    }
};

template <typename T, u32 N>
    requires std::is_default_constructible_v<T>
class StaticArray final
{
    static constexpr size_t ElemSize      = sizeof(T);
    static constexpr size_t ContainerSize = ElemSize * N;

  public:
    T Data[N];

    constexpr StaticArray() = default;
    explicit StaticArray(const T&& defaultValue)
    {
        for (u32 i = 0; i < N; i++)
        {
            Data[i] = defaultValue;
        }
    }
    explicit StaticArray(std::initializer_list<T> elems)
    {
        assert(elems.size() <= N);
        memcpy(Data, elems.begin(), elems.size() * ElemSize);
    }

    ~StaticArray() = default;

    constexpr bool is_valid_index(const u32 idx) const { return idx < N; }

    inline constexpr u32 Size() const { return N; }

    inline const T& operator[](const u32 _idx) const
    {
        assert(is_valid_index(_idx));
        return Data[_idx];
    }

    inline T& operator[](const u32 _idx)
    {
        assert(is_valid_index(_idx));
        return Data[_idx];
    }

    template <u32 M>
    inline void operator=(const StaticArray<T, M>& other)
    {
        constexpr u32 Size = std::min(N, M);
        memcpy(Data, other.Data, Size * ElemSize);
    }

    inline void operator=(const View<T>&& view)
    {
        const u32 Size = std::min(N, view.NumElements);
        memcpy(Data, view.Data, Size * ElemSize);
    }

    // ---------------- Ranged for iteration interface ----------------
    T*       begin() { return &Data[0]; }
    T*       end() { return &Data[N]; }
    const T* begin() const { return &Data[0]; }
    const T* end() const { return &Data[N]; }

    // ---------------- Implicit casts to views ----------------
    operator View<const T>() const { return CreateView(*this); }
    operator View<T>() { return CreateView(*this); }
};

/*
 * Dynamic array. Always constructed against an explicit allocator; growth is
 * memcpy-based, so element types must be trivially copyable (enforced below).
 * On a linear allocator (arena) every regrowth abandons the old block --
 * reserve realistically up front.
 */
template <typename T>
class Array final
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "Array grows with memcpy -- T must be trivially copyable");

  public:
    static constexpr u32 ElemSize = sizeof(T);

  public:
    T*  Data        = nullptr;
    u32 NumElements = 0;

    u32          _NumAllocated = 0;
    IAllocator&  _Allocator;
    MemoryHandle memory_handle;

    explicit Array(IAllocator& allocator, u32 reservedNum = 0)
        : _Allocator(allocator)
    {
        if (reservedNum > 0)
        {
            GrowCapacity(reservedNum);
        }
    }

    Array(IAllocator& allocator, std::initializer_list<T> initList)
        : _Allocator(allocator)
    {
        const u32 num = static_cast<u32>(initList.size());
        if (num > 0)
        {
            GrowCapacity(num);
            memcpy(Data, initList.begin(), num * ElemSize);
            NumElements = num;
        }
    }

    Array(const Array<T>& array) : _Allocator(array._Allocator)
    {
        if (array.NumElements > 0)
        {
            GrowCapacity(array.NumElements);
            memcpy(Data, array.Data, array.NumElements * ElemSize);
            NumElements = array.NumElements;
        }
    }

    ~Array() { _Allocator.Free(memory_handle); }

    u32 Size() const { return NumElements; }

    // lowercase shims mirroring std::vector's read surface (see View)
    u32      size() const { return NumElements; }
    T*       data() { return Data; }
    const T* data() const { return Data; }
    bool     empty() const { return NumElements == 0u; }
    T&       front() { assert(NumElements > 0); return Data[0]; }
    const T& front() const { assert(NumElements > 0); return Data[0]; }
    T&       back() { assert(NumElements > 0); return Data[NumElements - 1]; }
    const T& back() const { assert(NumElements > 0); return Data[NumElements - 1]; }

    /*
     * Ensure room for at least minCapacity elements (rounded up to a power of
     * two). Does not touch NumElements.
     */
    void GrowCapacity(u32 minCapacity)
    {
        const u32 new_capacity = round_up_pow2(minCapacity);
        if (new_capacity <= _NumAllocated)
        {
            return;
        }

        T*           temp       = nullptr;
        MemoryHandle new_memory = _Allocator.CreateArray<T>(temp, new_capacity);
        assert(new_memory.is_valid());

        if (Data)
        {
            memcpy(temp, Data, NumElements * ElemSize);
            _Allocator.Free(memory_handle);
        }

        Data          = temp;
        memory_handle = new_memory;
        _NumAllocated = new_capacity;
    }

    void Reserve(u32 newAmount)
    {
        if (newAmount > _NumAllocated)
        {
            GrowCapacity(newAmount);
        }
    }

    /*
     * Set the element count, growing capacity when needed. New elements are
     * zero-initialized (allocators zero their blocks; shrinking then regrowing
     * within existing capacity keeps old bytes -- don't rely on those).
     */
    void Resize(u32 newSize)
    {
        Reserve(newSize);
        NumElements = newSize;
    }

    void add_no_init(u32 amount)
    {
        const u32 requested_size = NumElements + amount;
        Reserve(requested_size);
        NumElements = requested_size;
    }

    u32 Add(const T& elem)
    {
        if (NumElements >= _NumAllocated)
        {
            GrowCapacity(_NumAllocated > 0 ? 2 * _NumAllocated : 4u);
        }

        Data[NumElements++] = elem;

        return NumElements;
    }

    template <class... Args>
    u32 Emplace(Args&&... args)
    {
        if (NumElements >= _NumAllocated)
        {
            GrowCapacity(_NumAllocated > 0 ? 2 * _NumAllocated : 4u);
        }

        ::new (static_cast<void*>(&Data[NumElements++])) T(std::forward<Args>(args)...);

        return NumElements;
    }

    template <class... Args>
    void EmplaceAt(u32 index, Args&&... args)
    {
        assert(index < _NumAllocated);

        ::new (static_cast<void*>(&Data[index])) T(std::forward<Args>(args)...);
    }

    /*
     * Insert at index, shifting everything from index onwards one slot right.
     */
    void InsertAt(u32 index, const T& elem)
    {
        assert(index <= NumElements);

        if (NumElements >= _NumAllocated)
        {
            GrowCapacity(_NumAllocated > 0 ? 2 * _NumAllocated : 4u);
        }

        memmove(&Data[index + 1], &Data[index], (NumElements - index) * ElemSize);
        Data[index] = elem;
        NumElements++;
    }

    /*
     * Remove at index, shifting everything after it one slot left (keeps
     * order; use swap-with-last manually when order doesn't matter).
     */
    void RemoveAt(u32 index)
    {
        assert(index < NumElements);

        memmove(&Data[index], &Data[index + 1], (NumElements - index - 1) * ElemSize);
        NumElements--;
    }

    void Clear() { NumElements = 0; }

    void PopBack()
    {
        assert(NumElements > 0);
        NumElements--;
    }

    void RemoveSlack()
    {
        if (NumElements == 0)
        {
            _Allocator.Free(memory_handle);
            Data          = nullptr;
            memory_handle = {};
            _NumAllocated = 0;
            return;
        }

        if (_NumAllocated > NumElements)
        {
            // Move the allocation to a perfect fit size
            MemoryHandle new_handle = _Allocator.CreateArray<T>(Data, NumElements);
            memcpy(_Allocator.HandleToPtr(new_handle), _Allocator.HandleToPtr(memory_handle), NumElements * ElemSize);

            _Allocator.Free(memory_handle);

            memory_handle = new_handle;
            _NumAllocated = NumElements;
        }
    }

    void Append(View<const T> view)
    {
        Reserve(NumElements + view.NumElements);

        // copy over new elements
        memcpy(&Data[NumElements], view.Data, ElemSize * view.NumElements);
        NumElements += view.NumElements;
    }

    // ---------------- Operator overloads  ----------------

    const T& operator[](const u32 index) const
    {
        assert(index < NumElements);
        return Data[index];
    }

    T& operator[](const u32 index)
    {
        assert(index < NumElements);
        return Data[index];
    }

    void operator=(View<T> view)
    {
        Reserve(view.NumElements);

        memcpy(Data, view.Data, view.NumElements * ElemSize);
        NumElements = view.NumElements;
    }

    void operator=(const Array<T>& array)
    {
        Reserve(array.NumElements);

        memcpy(Data, array.Data, array.NumElements * ElemSize);
        NumElements = array.NumElements;
    }

    void operator=(std::initializer_list<T> list)
    {
        const u32 num = static_cast<u32>(list.size());
        Reserve(num);

        memcpy(Data, list.begin(), num * ElemSize);
        NumElements = num;
    }

    // ---------------- Ranged for iteration interface ----------------
    T*       begin() { return &Data[0]; }
    T*       end() { return &Data[NumElements]; }
    const T* begin() const { return &Data[0]; }
    const T* end() const { return &Data[NumElements]; }
    const T* cbegin() const { return &Data[0]; }
    const T* cend() const { return &Data[NumElements]; }

    // ---------------- Implicit casts to views ----------------
    operator View<const T>() const { return CreateConstView(*this); }
    operator View<T>() { return CreateView(*this); }
};

template <typename T, u32 N>
constexpr inline View<T> CreateView(StaticArray<T, N>& array, u32 size = N,
                                    u32 startIndex = 0)
{
    const u32 size_clamped = std::min(N - startIndex, size);
    View<T> Result = {.Data = &array[startIndex], .NumElements = size_clamped};
    return Result;
}

template <typename T, u32 N>
constexpr inline View<const T> CreateView(const StaticArray<T, N>& array,
                                          u32 size = N, u32 startIndex = 0)
{
    const u32 size_clamped = std::min(N - startIndex, size);

    View<const T> Result = {
        .Data        = &array[startIndex],
        .NumElements = size_clamped,
    };
    return Result;
}

template <typename T>
constexpr inline View<T> CreateView(Array<T>& array, u32 size, u32 startIndex)
{
    const u32 size_clamped = std::min(array.NumElements - startIndex, size);
    View<T> Result = {.Data = &array[startIndex], .NumElements = size_clamped};
    return Result;
}

template <typename T>
constexpr inline View<const T> CreateConstView(const Array<T>& array, u32 size,
                                               u32 startIndex)
{
    const u32 size_clamped = std::min(array.NumElements - startIndex, size);

    View<const T> Result = {
        .Data        = &array[startIndex],
        .NumElements = size_clamped,
    };
    return Result;
}

template <typename T>
constexpr inline View<T> CreateView(Array<T>& array)
{
    return CreateView(array, array.NumElements, 0u);
}

template <typename T>
constexpr inline View<const T> CreateConstView(const Array<T>& array)
{
    return CreateConstView(array, array.NumElements, 0u);
}

template <typename T>
constexpr inline View<T> CreateView(T* array, u32 size)
{
    View<T> Result = {
        .Data        = array,
        .NumElements = size,
    };
    return Result;
}

template <typename T>
constexpr inline View<const T> CreateConstView(const T* array, u32 size)
{
    View<const T> Result = {
        .Data        = array,
        .NumElements = size,
    };
    return Result;
}
