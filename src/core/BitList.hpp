#pragma once

#include "Allocators.hpp"
#include "Containers.hpp"
#include "Intrinsics.hpp"
#include "core.hpp"

#include <cassert>
#include <cstring>
#include <immintrin.h>
#include <initializer_list>

/*
 * Free list helper DS.
 *
 * 0|false:	free slot
 * 1|true:	taken
 */
template <u32 N, typename WordType = u32>
struct BitList final
{
    static constexpr u32 BitsPerWord = sizeof(WordType) * 8u;
    static constexpr u32 NumWords    = round_to(N, BitsPerWord) / BitsPerWord;
    static constexpr u32 NumBits     = BitsPerWord * NumWords;

  public:
    alignas(16) WordType data[NumWords] = {0};

  public:
    constexpr BitList() {}

    explicit BitList(std::initializer_list<WordType> il)
    {
        const u32 usable_size =
            __min((u32)il.size(), NumWords) * sizeof(NumWords);
        memcpy(&data, il.begin(), usable_size);
    }

    void set_bit(u32 index)
    {
        assert(index < N);

        u32 word_index, bit_index;
        get_indices(index, word_index, bit_index);

        u32 mask = 1 << bit_index;

        data[word_index] |= mask;
    }

    void unset_bit(u32 index)
    {
        assert(index < N);

        u32 word_index, bit_index;
        get_indices(index, word_index, bit_index);

        WordType mask = ~(1 << bit_index);

        data[word_index] &= mask;
    }

    /*
     * Find first flag starting from the given index. start_index excluded.
     */
    i32 find_first(bool flag, u32 start_index = 0u) const
    {
        u32 word_start_index, bit_start_index;
        get_indices(start_index, word_start_index, bit_start_index);

        for (u32 word_i = word_start_index; word_i < NumWords; word_i++)
        {
            const u32 bit_start_offset =
                word_i == word_start_index ? bit_start_index : 0u;

            WordType word = data[word_i] >> bit_start_offset;

            word = flag ? word : ~word;

            i32 set_index = -1;
            if constexpr (BitsPerWord <= 32u)
            {
                set_index = Intrinsics::find_lsb((u32)word);
            }
            else
            {
                set_index = Intrinsics::find_lsb((u64)word);
            }
            if (set_index >= 0)
            {
                return set_index + bit_start_offset + (word_i * BitsPerWord);
            }
        }

        return -1;
    }

    inline constexpr void get_indices(u32 index, u32& out_word_index,
                                      u32& out_bit_index) const
    {
        out_word_index = word_index_from_index(index);
        out_bit_index  = index - (out_word_index * BitsPerWord);
    }

    inline constexpr u32 word_index_from_index(u32 index) const
    {
        return round_down(index, BitsPerWord) / BitsPerWord;
    }

    inline constexpr bool operator[](u32 index) const
    {
        assert(index < N);
        u32 word_index, bit_index;
        get_indices(index, word_index, bit_index);

        WordType mask = 1 << bit_index;

        return (data[word_index] & mask);
    }

    inline bool operator==(const BitList<N, WordType>& other) const
    {
        return memcmp(&data, &other.data, N * sizeof(WordType));
    }
};

template <u32 ChunkSize = 32u>
    requires is_power_of_two_v<ChunkSize>
struct DynamicBitlist
{
    static constexpr u32 BitsPerChunk = BitList<ChunkSize>::NumBits;

    constexpr DynamicBitlist(IAllocator& allocator, u32 num_chunks = 2)
        : chunks(allocator, num_chunks),
          total_capacity(num_chunks * BitsPerChunk)
    {
        chunks.NumElements = chunks._NumAllocated;
    }

    Array<BitList<ChunkSize>> chunks;

    u32 total_capacity;

    void set_bit(u32 index)
    {
        assert(index < total_capacity);

        const u32 chunk_idx    = index_to_chunk_index(index);
        const u32 in_chunk_idx = index - chunk_idx;

        assert(chunk_idx < num_chunks());

        chunks[chunk_idx].set_bit(in_chunk_idx);
    }

    void unset_bit(u32 index)
    {
        assert(index < total_capacity);

        const u32 chunk_idx    = index_to_chunk_index(index);
        const u32 in_chunk_idx = index - chunk_idx;

        assert(chunk_idx < num_chunks());

        chunks[chunk_idx].unset_bit(in_chunk_idx);
    }

    void resize(u32 new_capacity)
    {
        if (new_capacity <= total_capacity)
        {
            return;
        }

        const u32 new_num_chunks = round_up_pow2(new_capacity / BitsPerChunk);
        chunks.Resize(new_num_chunks);

        total_capacity = BitsPerChunk * new_num_chunks;
    }

    inline const u32 index_to_chunk_index(u32 index) const
    {
        return round_down(index, BitsPerChunk) / BitsPerChunk;
    }

    /*
     * Find first flag starting from the given index. start_index excluded.
     */
    i32 find_first(bool flag, u32 start_index = 0u) const
    {
        for (u32 i = 0; i < chunks.NumElements; i++)
        {
            i32 first_in_chunk = chunks[i].find_first(flag);
            if (first_in_chunk >= 0)
            {
                return first_in_chunk + i;
            }
        }
    }

    inline const u32 size_bits() const { num_chunks() * ChunkSize; }

    inline const u32 num_chunks() const { return chunks.NumElements; }
};

static constexpr u32 aantal_bits = BitList<128>::NumBits;

template <u32 N>
BitList<N> bitlist_changed(const BitList<N>& a, const BitList<N>& b)
{
    assert(BitList<N>::BitsPerWord <= 128u);

    BitList<N> result;

    constexpr u32 words_per_chunk = 128u / BitList<N>::BitsPerWord;

    for (u32 i = 0; i < BitList<N>::NumWords; i += words_per_chunk)
    {
        __m128i chunk_a;
        __m128i chunk_b;

        u128 pad_chunk_a;
        u128 pad_chunk_b;

        bool pad_chunk = i + words_per_chunk >= BitList<N>::NumWords;
        if (pad_chunk)
        {
            const u32 num_remaining_bytes =
                (BitList<N>::NumWords - i) * (BitList<N>::BitsPerWord / 8u);

            // init padded chunks to zero
            pad_chunk_a = {.value = {.ll = {0u, 0u}}};
            pad_chunk_b = {.value = {.ll = {0u, 0u}}};

            memcpy(&pad_chunk_a, &a.data[i], num_remaining_bytes);
            memcpy(&pad_chunk_b, &b.data[i], num_remaining_bytes);

            chunk_a = _mm_load_si128((__m128i*)&pad_chunk_a.value);
            chunk_b = _mm_load_si128((__m128i*)&pad_chunk_b.value);
        }
        else
        {
            chunk_a = _mm_load_si128((__m128i*)&a.data[i]);
            chunk_b = _mm_load_si128((__m128i*)&b.data[i]);
        }

        __m128i chunk_result = _mm_xor_si128(chunk_a, chunk_b);

        // copy result from register to resulting array
        memcpy(&result.data[i], &chunk_result, sizeof(__m128i));
    }

// VALIDATION
#if 0
    BitList<N> result_validation;
    for (u32 i = 0u; i < BitList<N>::NumWords; i++)
    {
        result_validation.data[i] = a.data[i] ^ b.data[i];
    }

    assert(result == result_validation);
#endif

    return result;
}
