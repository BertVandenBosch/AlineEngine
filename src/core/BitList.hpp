#pragma once

#include "core.hpp"
#include <cassert>

/*
 * Free list helper DS.
 *
 * 0|false:	free slot
 * 1|true:	taken
 */
template <u32 N, typename WordType = u8>
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

        u32 mask = ~(1 << bit_index);

        data[word_index] &= mask;
    }

    i32 find_first(bool flag, u32 start_index = 0u) const
    {
        u32 word_index, bit_index;
        get_indices(start_index, word_index, bit_index);

        for (u32 word_i = word_index; word_i < NumWords; word_i++)
        {
            // TODO(Bert): use intrinsics to do this in a single instruction
            // instead of for every bit in the word
            const u32 bit_start_index = (word_i == word_index) ? bit_index : 0u;
            for (u32 bit_i = bit_start_index; bit_i < BitsPerWord; bit_i++)
            {
                const u32 mask  = 1u << bit_i;
                const u32 value = flag ? data[word_i] : ~data[word_i];
                if (value & mask)
                {
                    return bit_i + (word_i * BitsPerWord);
                }
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

        u32 mask = 1 << bit_index;

        return (data[word_index] & mask);
    }
};
