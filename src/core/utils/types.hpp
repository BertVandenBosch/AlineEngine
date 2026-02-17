#pragma once

#include <type_traits>

template <typename T>
struct remove_all_pointers
    : public std::conditional_t<std::is_pointer_v<T>, std::remove_pointer<T>,
                                std::type_identity<T>>
{
};

template <typename T>
using remove_all_pointers_t = typename remove_all_pointers<T>::type;
