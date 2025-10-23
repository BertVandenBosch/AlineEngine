#pragma once

#include "Allocators.hpp"
#include "core.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <initializer_list>

template <typename T>
struct View
{
    T*        Data;
    const u32 NumElements;

    inline T* begin() { return &Data[0]; }
    inline T* end() { return &Data[NumElements - 1]; }

    inline const T* begin() const { return &Data[0]; }
    inline const T* end() const { return &Data[NumElements - 1]; }

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
        assert(elems.size() < N);
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
};

template <typename T>
class Array final
{
    static constexpr u32 ElemSize = sizeof(T);

  public:
    T*  Data        = nullptr;
    u32 NumElements = 0;

    u32         _NumAllocated = 0;
    IAllocator& _Allocator;

    explicit Array(IAllocator& allocator, u32 reservedNum = 4)
        : _Allocator(allocator)
    {
        Data          = _Allocator.CreateArray<T>(reservedNum);
        _NumAllocated = reservedNum;
    }

    explicit Array(IAllocator& allocator, std::initializer_list<T> initList)
        : _Allocator(allocator)
    {
        Data          = _Allocator.CreateArray<T>(initList.size());
        _NumAllocated = initList.size();
        NumElements   = initList.size();

        memcpy(Data, initList.begin(), initList.size() * ElemSize);
    }

    Array(const Array<T>& array) : _Allocator(array._Allocator)
    {
        Data = _Allocator.CreateArray<T>(array.NumElements);

        memcpy(Data, array.Data, array.NumElements * ElemSize);
        NumElements   = array.NumElements;
        _NumAllocated = array.NumElements;
    }

    ~Array() { _Allocator.Free(Data); }

    u32 Size() const { return NumElements; }

    void Resize(u32 newSize)
    {
        // TODO: make size a power of 2?
        T* temp = _Allocator.CreateArray<T>(newSize);
        assert(temp != nullptr);
        // copy over old data to new TODO: what if we work with a paged
        // allocator?
        memcpy(temp, Data, NumElements * ElemSize);
        _Allocator.Free(Data);

        Data          = temp;
        _NumAllocated = newSize;
    }

    void Reserve(u32 newAmount)
    {
        assert(newAmount != 0u);

        if (newAmount > _NumAllocated)
        {
            Resize(newAmount);
        }
    }

    u32 Add(const T&& elem)
    {
        if (NumElements >= _NumAllocated - 1)
        {
            Reserve(2 * _NumAllocated);
        }

        NumElements++;
        Data[NumElements] = elem;

        return NumElements;
    }

    template <class... Args>
    u32 Emplace(Args&&... args)
    {
        if (NumElements >= _NumAllocated - 1)
        {
            Reserve(2 * _NumAllocated);
        }

        NumElements++;
        ::new (Data[NumElements]) T(std::forward(args)...);

        return NumElements;
    }

    template <class... Args>
    void EmplaceAt(u32 index, Args&&... args)
    {
        assert(index < _NumAllocated);

        NumElements++;
        ::new (Data[index]) T(std::forward(args)...);
    }

    void RemoveSlack()
    {
        if (_NumAllocated > NumElements)
        {
            Resize(NumElements);
        }
    }

    const T& operator[](const u32 index) const
    {
        assert(index <= NumElements);
        return Data[index];
    }

    T& operator[](const u32 index)
    {
        assert(index <= NumElements);
        return Data[index];
    }

    void operator=(View<T> view)
    {
        if (_NumAllocated < view.NumElements)
        {
            Resize(view.NumElements);
        }

        memcpy(Data, view.Data, view.NumElements * ElemSize);
        NumElements = view.NumElements;
    }

    void operator=(const Array<T>& array)
    {
        if (_NumAllocated < array._NumAllocated)
        {
            Resize(array._NumAllocated);
        }

        memcpy(Data, array.Data, array.NumElements * array.ElemSize);
        NumElements = array.NumElements;
    }

    // ---------------- Ranged for iteration interface ----------------
    T* begin() { return &Data[0]; }

    T* end() { return &Data[NumElements]; }
};

template <typename T, u32 N>
inline View<T> CreateView(StaticArray<T, N>& array, u32 startIndex = 0,
                          u32 size = N)
{
    const u32 size_clamped = std::min(N - startIndex, size);
    View<T> Result = {.Data = &array[startIndex], .NumElements = size_clamped};
    return Result;
}

template <typename T, u32 N>
inline View<const T> CreateView(const StaticArray<T, N>& array,
                                u32 startIndex = 0, u32 size = N)
{
    const u32 size_clamped = std::min(N - startIndex, size);

    View<const T> Result = {
        .Data        = &array[startIndex],
        .NumElements = size_clamped,
    };
    return Result;
}

template <typename T, u32 N>
inline View<T> CreateView(const T* array)
{
    View<T> Result = {
        .Data        = array,
        .NumElements = N,
    };
    return Result;
}

template <typename T, u32 N>
inline View<const T> CreateView(T* array)
{
    View<T> Result = {
        .Data        = array,
        .NumElements = N,
    };
    return Result;
}
