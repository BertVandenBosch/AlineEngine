#pragma once

#include "Intrinsics.hpp"
#include "core.hpp"
#include <cassert>

#define BATCH_SEARCH 1

/*
 * Free list helper DS.
 *
 * 0|false:	free slot
 * 1|true:	taken
 */
template <u32 N, typename WordType = u8, typename BatchWordType = u32>
struct BitList final
{
    static constexpr u32 BitsPerWord = sizeof(WordType) * 8u;
    static constexpr u32 NumWords    = round_to(N, BitsPerWord) / BitsPerWord;

  public:
    WordType data[NumWords] = {0};

  public:
    constexpr BitList() {}

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

    constexpr void get_indices(u32 index, u32& out_word_index,
                               u32& out_bit_index) const
    {
        out_word_index = word_index_from_index(index);
        out_bit_index  = index - (out_word_index * BitsPerWord);
    }

    constexpr u32 word_index_from_index(u32 index) const
    {
        return round_down(index, BitsPerWord) / BitsPerWord;
    }

    constexpr bool operator[](u32 index) const
    {
        assert(index < N);
        u32 word_index, bit_index;
        get_indices(index, word_index, bit_index);

        WordType mask = 1 << bit_index;

        return (data[word_index] & mask);
    }
};
