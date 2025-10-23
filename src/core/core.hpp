#pragma once

// CORE INCLUDES
#include <assert.h>

// CORE TYPES

typedef char                i8;
typedef short               i16;
typedef int                 i32;
typedef long long           i64;

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

#define KB(size) (size_t)(size * 1024)
#define MB(size) (size_t)(size * 1024 * 1024)
#define GB(size) (size_t)(size * 1024 * 1024 * 1024)

template<typename SizeType>
constexpr bool is_power_of_two(SizeType x) { return (x & (x - 1)) == 0; }

template<typename SizeType>
constexpr SizeType round_to(SizeType value, SizeType roundTo)
{
	assert(is_power_of_two(roundTo)); //to make the &~ op work
	return (value + (roundTo - 1)) & ~(roundTo - 1);
}

template<typename SizeType>
constexpr SizeType round_down(SizeType value, SizeType roundTo)
{
	assert(is_power_of_two(roundTo)); //to make the &~ op work
	return value & ~(roundTo - 1);
}

constexpr u8 afrondehhh = round_down(1, 8) / 8;
