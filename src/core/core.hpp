#pragma once

// CORE INCLUDES
#include <assert.h>

// CORE TYPES

typedef char      i8;
typedef short     i16;
typedef int       i32;
typedef long long i64;

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

struct u128
{
    union
    {
        u64 ll[2];
        u32 l[4];
        u16 s[8];
        u8  ch[16];
    } value;
};

#define KB(size) (size_t)(size * 1024)
#define MB(size) (size_t)(size * 1024 * 1024)
#define GB(size) (size_t)(size * 1024 * 1024 * 1024)

template <typename SizeType>
constexpr bool is_power_of_two(SizeType x)
{
    if (x == 0)
    {
        return false;
    }
    return (x & (x - 1)) == 0;
}

template <typename SizeType>
constexpr SizeType round_to(SizeType value, SizeType roundTo)
{
    assert(is_power_of_two(roundTo)); // to make the &~ op work
    return (value + (roundTo - 1)) & ~(roundTo - 1);
}

template <typename SizeType>
constexpr SizeType round_down(SizeType value, SizeType roundTo)
{
    assert(is_power_of_two(roundTo)); // to make the &~ op work
    return value & ~(roundTo - 1);
}

constexpr inline u32 round_up_pow2(u32 value)
{
    return value == 1u ? 1u : (1u << (32 - __builtin_clz(value - 1u)));
}

constexpr inline u32 round_down_pow2(u32 value)
{
    return value == 1u ? 0u : (1u << (32 - __builtin_clz(value) - 1u));
}

constexpr inline u64 round_up_pow2(u64 value)
{
    return value == 1u ? 1u : (1u << (64 - __builtin_clzll(value - 1u)));
}

constexpr inline u64 round_down_pow2(u64 value)
{
    return value == 1u ? 0u : (1u << (64 - __builtin_clzll(value) - 1u));
}
