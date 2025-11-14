#pragma once

#include "../Containers.hpp"

#include <utility>

namespace AE
{

template <typename T, class... Args>
inline constexpr auto Invoke(T&& obj, Args&&... args)
    -> decltype((std::forward(obj))(std::forward(args)...))
{
    return (std::forward(obj))(std::forward(args)...);
}

template<typename T>
struct EqualOp
{
    inline constexpr bool operator()(const T&& a, const T&& b) const { return std::forward(a) == std::forward(b); }
};

template<>
struct EqualOp<void>
{
	template<typename Ta, typename Tb>
    inline constexpr bool operator()(const Ta&& a, const Tb&& b) const { return std::forward(a) == std::forward(b); }
};

template <typename ArrayType, typename ElemType, typename ComparatorT>
[[nodiscard]] inline i32 find_linear(View<const ArrayType>&& container,
                       const ElemType& element, ComparatorT comparator)
{
	const i32 num_iters = (i32)container.NumElements;
    for (i32 i = 0; i < num_iters; i++)
    {
        if (comparator(container.Data[i], element))
        {
            return i;
        }
    }

    return -1;
}

template <typename ArrayType, typename ElemType>
[[nodiscard]] inline i32 find_linear(View<const ArrayType>&& container,
	const ElemType& element)
{
	return find_linear(std::forward(container), element, EqualOp<void>());
}
} // namespace AE
