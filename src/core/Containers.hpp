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
 * The lowercase size()/empty() accessors mirror std::span's surface so
 * span-shaped code can consume Views without a
 * rename pass.
 */
template <typename T>
struct View
{
    T*  data         = nullptr;
    u32 num_elements = 0;

    inline T* begin() { return data; }
    inline T* end() { return data + num_elements; }

    inline const T* begin() const { return data; }
    inline const T* end() const { return data + num_elements; }

    inline const T* cbegin() const { return data; }
    inline const T* cend() const { return data + num_elements; }

    inline constexpr u32  size() const { return num_elements; }
    inline constexpr bool empty() const { return num_elements == 0u; }

    inline const T& operator[](const u32 idx) const
    {
        assert(idx < num_elements);
        return data[idx];
    }

    inline T& operator[](const u32 idx)
    {
        assert(idx < num_elements);
        return data[idx];
    }
};

template <typename T, u32 N>
    requires std::is_default_constructible_v<T>
class StaticArray final
{
    static constexpr size_t elem_size      = sizeof(T);
    static constexpr size_t container_size = elem_size * N;

  public:
    T data[N];

    constexpr StaticArray() = default;
    explicit StaticArray(const T&& default_value)
    {
        for (u32 i = 0; i < N; i++)
        {
            data[i] = default_value;
        }
    }
    explicit StaticArray(std::initializer_list<T> elems)
    {
        assert(elems.size() <= N);
        memcpy(data, elems.begin(), elems.size() * elem_size);
    }

    ~StaticArray() = default;

    constexpr bool is_valid_index(const u32 idx) const { return idx < N; }

    inline constexpr u32 size() const { return N; }

    inline const T& operator[](const u32 idx) const
    {
        assert(is_valid_index(idx));
        return data[idx];
    }

    inline T& operator[](const u32 idx)
    {
        assert(is_valid_index(idx));
        return data[idx];
    }

    template <u32 M>
    inline void operator=(const StaticArray<T, M>& other)
    {
        constexpr u32 count = std::min(N, M);
        memcpy(data, other.data, count * elem_size);
    }

    inline void operator=(const View<T>&& src)
    {
        const u32 count = std::min(N, src.num_elements);
        memcpy(data, src.data, count * elem_size);
    }

    // ---------------- Ranged for iteration interface ----------------
    T*       begin() { return &data[0]; }
    T*       end() { return &data[N]; }
    const T* begin() const { return &data[0]; }
    const T* end() const { return &data[N]; }

    // ---------------- Implicit casts to views ----------------
    operator View<const T>() const { return create_view(*this); }
    operator View<T>() { return create_view(*this); }
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
                  "array grows with memcpy -- T must be trivially copyable");

  public:
    static constexpr u32 elem_size = sizeof(T);

  public:
    T*  data         = nullptr;
    u32 num_elements = 0;

    u32          num_allocated = 0;
    IAllocator&  allocator;
    MemoryHandle handle;

    explicit Array(IAllocator& allocator, u32 reserved_num = 0)
        : allocator(allocator)
    {
        if (reserved_num > 0)
        {
            grow_capacity(reserved_num);
        }
    }

    Array(IAllocator& allocator, std::initializer_list<T> init_list)
        : allocator(allocator)
    {
        const u32 num = static_cast<u32>(init_list.size());
        if (num > 0)
        {
            grow_capacity(num);
            memcpy(data, init_list.begin(), num * elem_size);
            num_elements = num;
        }
    }

    Array(const Array<T>& arr) : allocator(arr.allocator)
    {
        if (arr.num_elements > 0)
        {
            grow_capacity(arr.num_elements);
            memcpy(data, arr.data, arr.num_elements * elem_size);
            num_elements = arr.num_elements;
        }
    }

    ~Array() { allocator.free(handle); }

    // lowercase shims mirroring std::vector's read surface (see View)
    u32  size() const { return num_elements; }
    bool empty() const { return num_elements == 0u; }
    T&   front()
    {
        assert(num_elements > 0);
        return data[0];
    }
    const T& front() const
    {
        assert(num_elements > 0);
        return data[0];
    }
    T& back()
    {
        assert(num_elements > 0);
        return data[num_elements - 1];
    }
    const T& back() const
    {
        assert(num_elements > 0);
        return data[num_elements - 1];
    }

    /*
     * Ensure room for at least min_capacity elements (rounded up to a power of
     * two). Does not touch num_elements.
     */
    void grow_capacity(u32 min_capacity)
    {
        const u32 new_capacity = round_up_pow2(min_capacity);
        if (new_capacity <= num_allocated)
        {
            return;
        }

        T*           temp       = nullptr;
        MemoryHandle new_memory = allocator.create_array<T>(temp, new_capacity);
        assert(new_memory.is_valid());

        if (data)
        {
            memcpy(temp, data, num_elements * elem_size);
            allocator.free(handle);
        }

        data          = temp;
        handle        = new_memory;
        num_allocated = new_capacity;
    }

    void reserve(u32 new_amount)
    {
        if (new_amount > num_allocated)
        {
            grow_capacity(new_amount);
        }
    }

    /*
     * Set the element count, growing capacity when needed. New elements are
     * zero-initialized (allocators zero their blocks; shrinking then regrowing
     * within existing capacity keeps old bytes -- don't rely on those).
     */
    void resize(u32 new_size)
    {
        reserve(new_size);
        num_elements = new_size;
    }

    void add_no_init(u32 amount)
    {
        const u32 requested_size = num_elements + amount;
        reserve(requested_size);
        num_elements = requested_size;
    }

    u32 add(const T& elem)
    {
        if (num_elements >= num_allocated)
        {
            grow_capacity(num_allocated > 0 ? 2 * num_allocated : 4u);
        }

        data[num_elements++] = elem;

        return num_elements;
    }

    template <class... Args>
    u32 emplace(Args&&... args)
    {
        if (num_elements >= num_allocated)
        {
            grow_capacity(num_allocated > 0 ? 2 * num_allocated : 4u);
        }

        ::new (static_cast<void*>(&data[num_elements++]))
            T(std::forward<Args>(args)...);

        return num_elements;
    }

    template <class... Args>
    void emplace_at(u32 index, Args&&... args)
    {
        assert(index < num_allocated);

        ::new (static_cast<void*>(&data[index])) T(std::forward<Args>(args)...);
    }

    /*
     * Insert at index, shifting everything from index onwards one slot right.
     */
    void insert_at(u32 index, const T& elem)
    {
        assert(index <= num_elements);

        if (num_elements >= num_allocated)
        {
            grow_capacity(num_allocated > 0 ? 2 * num_allocated : 4u);
        }

        memmove(&data[index + 1], &data[index],
                (num_elements - index) * elem_size);
        data[index] = elem;
        num_elements++;
    }

    /*
     * Remove at index, shifting everything after it one slot left (keeps
     * order; use swap-with-last manually when order doesn't matter).
     */
    void remove_at(u32 index)
    {
        assert(index < num_elements);

        memmove(&data[index], &data[index + 1],
                (num_elements - index - 1) * elem_size);
        num_elements--;
    }

    void clear() { num_elements = 0; }

    void pop_back()
    {
        assert(num_elements > 0);
        num_elements--;
    }

    void remove_slack()
    {
        if (num_elements == 0)
        {
            allocator.free(handle);
            data          = nullptr;
            handle        = {};
            num_allocated = 0;
            return;
        }

        if (num_allocated > num_elements)
        {
            // Move the allocation to a perfect fit size
            MemoryHandle new_handle =
                allocator.create_array<T>(data, num_elements);
            memcpy(allocator.handle_to_ptr(new_handle),
                   allocator.handle_to_ptr(handle), num_elements * elem_size);

            allocator.free(handle);

            handle        = new_handle;
            num_allocated = num_elements;
        }
    }

    void append(View<const T> src)
    {
        reserve(num_elements + src.num_elements);

        // copy over new elements
        memcpy(&data[num_elements], src.data, elem_size * src.num_elements);
        num_elements += src.num_elements;
    }

    // ---------------- Operator overloads  ----------------

    const T& operator[](const u32 index) const
    {
        assert(index < num_elements);
        return data[index];
    }

    T& operator[](const u32 index)
    {
        assert(index < num_elements);
        return data[index];
    }

    void operator=(View<T> src)
    {
        reserve(src.num_elements);

        memcpy(data, src.data, src.num_elements * elem_size);
        num_elements = src.num_elements;
    }

    void operator=(const Array<T>& arr)
    {
        reserve(arr.num_elements);

        memcpy(data, arr.data, arr.num_elements * elem_size);
        num_elements = arr.num_elements;
    }

    void operator=(std::initializer_list<T> list)
    {
        const u32 num = static_cast<u32>(list.size());
        reserve(num);

        memcpy(data, list.begin(), num * elem_size);
        num_elements = num;
    }

    // ---------------- Ranged for iteration interface ----------------
    T*       begin() { return &data[0]; }
    T*       end() { return &data[num_elements]; }
    const T* begin() const { return &data[0]; }
    const T* end() const { return &data[num_elements]; }
    const T* cbegin() const { return &data[0]; }
    const T* cend() const { return &data[num_elements]; }

    // ---------------- Implicit casts to views ----------------
    operator View<const T>() const { return create_const_view(*this); }
    operator View<T>() { return create_view(*this); }
};

template <typename T, u32 N>
constexpr inline View<T> create_view(StaticArray<T, N>& arr, u32 size = N,
                                     u32 start_index = 0)
{
    const u32 size_clamped = std::min(N - start_index, size);
    View<T> result = {.data = &arr[start_index], .num_elements = size_clamped};
    return result;
}

template <typename T, u32 N>
constexpr inline View<const T> create_view(const StaticArray<T, N>& arr,
                                           u32 size = N, u32 start_index = 0)
{
    const u32 size_clamped = std::min(N - start_index, size);

    View<const T> result = {
        .data         = &arr[start_index],
        .num_elements = size_clamped,
    };
    return result;
}

template <typename T>
constexpr inline View<T> create_view(Array<T>& arr, u32 size, u32 start_index)
{
    const u32 size_clamped = std::min(arr.num_elements - start_index, size);
    View<T> result = {.data = &arr[start_index], .num_elements = size_clamped};
    return result;
}

template <typename T>
constexpr inline View<const T> create_const_view(const Array<T>& arr, u32 size,
                                                 u32 start_index)
{
    const u32 size_clamped = std::min(arr.num_elements - start_index, size);

    View<const T> result = {
        .data         = &arr[start_index],
        .num_elements = size_clamped,
    };
    return result;
}

template <typename T>
constexpr inline View<T> create_view(Array<T>& arr)
{
    return create_view(arr, arr.num_elements, 0u);
}

template <typename T>
constexpr inline View<const T> create_const_view(const Array<T>& arr)
{
    return create_const_view(arr, arr.num_elements, 0u);
}

template <typename T>
constexpr inline View<T> create_view(T* arr, u32 size)
{
    View<T> result = {
        .data         = arr,
        .num_elements = size,
    };
    return result;
}

template <typename T>
constexpr inline View<const T> create_const_view(const T* arr, u32 size)
{
    View<const T> result = {
        .data         = arr,
        .num_elements = size,
    };
    return result;
}
