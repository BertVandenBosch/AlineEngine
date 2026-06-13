#pragma once

#include "Allocators.hpp"
#include "core.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <type_traits>

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

    // ---------------- Implicit casts to views ----------------
    operator View<T>() const { return CreateConstView(*this); }
    operator View<T>() { return CreateView(*this); }
};

template <typename T>
class Array final
{
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
            memory_handle = _Allocator.CreateArray<T>(&Data, reservedNum);
            _NumAllocated = round_up_pow2(reservedNum);
        }
    }

    Array(IAllocator& allocator, std::initializer_list<T> initList)
        : _Allocator(allocator)
    {
        const u32 alloc_size = round_up_pow2(initList.size());

        memory_handle = _Allocator.CreateArray<T>(&Data, alloc_size);
        memcpy(Data, initList.begin(), initList.size() * ElemSize);

        _NumAllocated = alloc_size;
        NumElements = initList.size();
    }

    Array(const Array<T>& array) : _Allocator(array._Allocator)
    {
    	const u32 alloc_size = round_up_pow2(array.NumElements);

        memory_handle = _Allocator.CreateArray<T>(&Data, alloc_size);
        memcpy(Data, array.Data, array.NumElements * ElemSize);

        NumElements   = array.NumElements;
        _NumAllocated = alloc_size;
    }

    ~Array() { _Allocator.Free(memory_handle); }

    u32 Size() const { return NumElements; }

    void Resize(u32 newSize)
    {
        const u32 new_size_pow2 = round_up_pow2(newSize);

        T*           temp = nullptr;
        MemoryHandle new_memory =
            _Allocator.CreateArray<T>(&temp, new_size_pow2);
        assert(new_memory.is_valid());

        // copy over old data to new        // allocator?
        memcpy(temp, Data, NumElements * ElemSize);
        _Allocator.Free(memory_handle);

        Data          = temp;
        memory_handle = new_memory;
        _NumAllocated = new_size_pow2;
    }

    void Reserve(u32 newAmount)
    {
        assert(newAmount != 0u);

        if (newAmount > _NumAllocated)
        {
            Resize(newAmount);
        }
    }

    void add_no_init(u32 amount)
    {
    	const u32 requested_size = NumElements + amount;
    	Reserve(requested_size);
    	NumElements = requested_size;
    }

    u32 Add(const T& elem)
    {
        if (NumElements >= _NumAllocated - 1)
        {
            Reserve(2 * _NumAllocated);
        }

        Data[NumElements++] = elem;

        return NumElements;
    }

    template <class... Args>
    u32 Emplace(Args&&... args)
    {
        if (NumElements >= _NumAllocated - 1)
        {
            Reserve(2 * _NumAllocated);
        }

        ::new (Data[NumElements++]) T(std::forward(args)...);

        return NumElements;
    }

    template <class... Args>
    void EmplaceAt(u32 index, Args&&... args)
    {
        assert(index < _NumAllocated);

        ::new (Data[index]) T(std::forward(args)...);
    }

    void RemoveSlack()
    {
        if (_NumAllocated > NumElements)
        {
            // Move the allocation to a perfect fit size
            MemoryHandle new_handle = _Allocator.CreateArray<T>(&Data, NumElements);
            memcpy(_Allocator.HandleToPtr(new_handle), _Allocator.HandleToPtr(memory_handle), NumElements * ElemSize);

            _Allocator.Free(memory_handle);

            memory_handle = new_handle;
            _NumAllocated = NumElements;
        }
    }

    void Append(View<const T> view)
    {
        const u32 new_size = NumElements + view.NumElements;
        if (new_size > _NumAllocated)
        {
            Resize(new_size);
        }

        // copy over new elements
        memcpy(&Data[NumElements], view.Data, ElemSize * view.NumElements);
        NumElements += view.NumElements;
    }

    // ---------------- Operator overloads  ----------------

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

    void operator=(std::initializer_list<T> list)
    {
        if (_NumAllocated < list.size())
        {
            Resize(list.size());
        }
        memcpy(Data, list.begin(), list.size() * ElemSize);
        NumElements = list.size();
    }

    template <u32 NUM_STRINGS>
        requires(std::is_class_v<char>(std::remove_pointer<T>::type))
    void operator=(const char strings[NUM_STRINGS])
    {
        if (_NumAllocated < NUM_STRINGS)
        {
            Resize(NUM_STRINGS);
        }
        memcpy(Data, strings, NUM_STRINGS * ElemSize);
    }

    // ---------------- Ranged for iteration interface ----------------
    T* begin() { return &Data[0]; }

    T* end() { return &Data[NumElements]; }

    // ---------------- Implicit casts to views ----------------
    operator View<T>() const { return CreateConstView(*this); }
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
constexpr inline View<T> CreateView(const T* array, u32 size)
{
    View<T> Result = {
        .Data        = array,
        .NumElements = size,
    };
    return Result;
}

template <typename T>
constexpr inline View<const T> CreateView(T* array, u32 size)
{
    View<T> Result = {
        .Data        = array,
        .NumElements = size,
    };
    return Result;
}
