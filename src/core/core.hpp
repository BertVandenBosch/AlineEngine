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
