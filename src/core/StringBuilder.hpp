#pragma once

#include "Allocators.hpp"
#include "core.hpp"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>

/*
 * Allocator-backed string assembly. Covers the "patch this shader source" and
 * "build this error message" cases without a general string type: append into
 * a growable buffer, read back with c_str().
 *
 * The buffer is always null-terminated. On a linear allocator (arena) growth
 * abandons the old block, so size the initial capacity for the common case.
 */
class StringBuilder
{
  public:
    explicit StringBuilder(IAllocator& allocator, u32 initial_capacity = 256u)
        : allocator_(allocator)
    {
        grow(initial_capacity > 0u ? initial_capacity : 16u);
        buf_[0] = '\0';
    }

    ~StringBuilder() { allocator_.free(handle_); }

    StringBuilder(const StringBuilder&)            = delete;
    StringBuilder& operator=(const StringBuilder&) = delete;

    void append(const char* str) { append(str, (u32)strlen(str)); }

    void append(const char* str, u32 len)
    {
        reserve(len_ + len + 1u);
        memcpy(&buf_[len_], str, len);
        len_ += len;
        buf_[len_] = '\0';
    }

    void appendf(const char* fmt, ...)
    {
        va_list args;

        va_start(args, fmt);
        const i32 needed = vsnprintf(nullptr, 0u, fmt, args);
        va_end(args);

        if (needed <= 0)
        {
            return;
        }

        reserve(len_ + (u32)needed + 1u);

        va_start(args, fmt);
        vsnprintf(&buf_[len_], (size_t)needed + 1u, fmt, args);
        va_end(args);

        len_ += (u32)needed;
    }

    void clear()
    {
        len_    = 0u;
        buf_[0] = '\0';
    }

    const char* c_str() const { return buf_; }
    u32         length() const { return len_; }
    bool        empty() const { return len_ == 0u; }
    IAllocator& allocator() const { return allocator_; }

  private:
    void reserve(u32 min_capacity)
    {
        if (min_capacity > cap_)
        {
            grow(min_capacity);
        }
    }

    void grow(u32 min_capacity)
    {
        const u32 new_cap = round_up_pow2(min_capacity);

        char*        new_buf = nullptr;
        MemoryHandle new_handle =
            allocator_.create_array<char>(new_buf, new_cap);
        assert(new_handle.is_valid());

        if (buf_)
        {
            memcpy(new_buf, buf_, len_ + 1u);
            allocator_.free(handle_);
        }

        buf_    = new_buf;
        handle_ = new_handle;
        cap_    = new_cap;
    }

    char*        buf_ = nullptr;
    u32          len_ = 0u;
    u32          cap_ = 0u;
    IAllocator&  allocator_;
    MemoryHandle handle_;
};
