#pragma once

#include "core.hpp"

// #include <immintrin.h>

namespace Intrinsics
{

constexpr i32 find_lsb(u32 value)
{
    if (value == 0u)
    {
        return -1;
    }
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_ctz(value);
#else
    return -1;
#endif
}

constexpr i32 find_lsb(u64 value)
{
    if (value == 0u)
    {
        return -1;
    }
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_ctzll(value);
#else
    return -1;
#endif
}

constexpr inline i32 find_msb(u32 value)
{
    if (value == 0u)
    {
        return -1;
    }
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_clz(value);
#else
    return -1;
#endif
}

// constexpr i32 find_msb(u64 value)
// {
//     if (value == 0u)
//     {
//         return -1;
//     }
// #if defined(__clang__) || defined(__GNUC__)
// 	return __builtin_clzll(value);
// #endif
// }

} // namespace Intrinsics
