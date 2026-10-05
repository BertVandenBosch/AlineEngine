#include "core/BitList.hpp"
#include "core/core.hpp"
#include <cassert>
#include <stdio.h>

// #include "core/core.hpp"
#include "core/Allocators.hpp"
#include "core/Containers.hpp"
#include "core/Pool.hpp"
#include "core/StringBuilder.hpp"

static void run_core_tests()
{
    ArenaAllocator arena(KB(64));

    // ---- Array: grow from empty, insert, remove ----
    {
        Array<u32> arr(arena);
        for (u32 i = 0; i < 10u; i++)
        {
            arr.Add(i); // must grow from capacity 0 without faulting
        }
        assert(arr.Size() == 10u && arr[9] == 9u);

        arr.InsertAt(0u, 100u);
        assert(arr.Size() == 11u && arr[0] == 100u && arr[1] == 0u && arr[10] == 9u);

        arr.RemoveAt(0u);
        assert(arr.Size() == 10u && arr[0] == 0u && arr[9] == 9u);

        arr.Emplace(77u);
        assert(arr.Size() == 11u && arr[10] == 77u);

        arr.PopBack();
        arr.Clear();
        assert(arr.Size() == 0u);
    }

    // ---- View: iteration covers every element ----
    {
        Array<u32> arr(arena, {1, 2, 3, 4, 5});
        u32        count = 0;
        u32        sum   = 0;
        for (const u32 v : CreateView(arr))
        {
            count++;
            sum += v;
        }
        assert(count == 5u && sum == 15u);

        View<u32> empty_view = {};
        assert(empty_view.empty() && empty_view.size() == 0u);
    }

    // ---- DynamicBitlist: chunk-boundary bits (chunk size 64) ----
    {
        DynamicBitlist<64u> bits(arena, 2u);
        bits.set_bit(63u);
        bits.set_bit(64u);
        bits.set_bit(65u);
        assert(bits[63u] && bits[64u] && bits[65u] && !bits[62u]);
        assert(bits.find_first(true) == 63);
        assert(bits.find_first(true, 64u) == 64);

        bits.unset_bit(64u);
        assert(!bits[64u]);
        assert(bits.find_first(true, 64u) == 65);
        assert(bits.find_first(false) == 0);

        bits.resize(100u);
        assert(bits.size_bits() >= 100u);
        bits.set_bit(99u);
        assert(bits[99u]);
    }

    // ---- Pool: index 0, get/destroy, generation recycling ----
    {
        typedef PoolHandle<u32, 24u, 8u> TestHandle;

        Pool<u64, TestHandle> pool(arena, 2u);

        TestHandle a = pool.create(11u);
        assert(a.index == 0u && a.valid()); // index 0 is a normal slot
        assert(pool.get(a) && *pool.get(a) == 11u);
        assert(pool.numObjects() == 1u);

        TestHandle stale = a;
        pool.destroy(a);
        assert(pool.get(stale) == nullptr); // stale handle rejected
        assert(pool.numObjects() == 0u);

        TestHandle b = pool.create(22u);
        assert(b.index == 0u && b.gen != stale.gen); // slot reused, gen bumped
        assert(pool.get(stale) == nullptr && *pool.get(b) == 22u);

        TestHandle c = pool.create(33u);
        TestHandle d = pool.create(44u); // forces growth past start_size 2
        assert(*pool.get(c) == 33u && *pool.get(d) == 44u && *pool.get(b) == 22u);
        assert(pool.numObjects() == 3u);

        assert(TestHandle{}.empty()); // default handle is never valid
        assert(pool.get(TestHandle{}) == nullptr);

        pool.clear();
        assert(pool.numObjects() == 0u && pool.get(b) == nullptr);

        // 64-bit handle layout must instantiate
        typedef PoolHandle<u64, 32u, 32u> WideHandle;
        WideHandle w{};
        w.index = 0xFFFFFFFFu;
        w.gen   = 1u;
        assert(w.index == 0xFFFFFFFFu && w.valid());
    }

    // ---- LinearBlockAllocator: free + coalesce ----
    {
        LinearBlockAllocator blocks(KB(1));

        MemoryHandle a = blocks.Allocate(100u, {});
        MemoryHandle b = blocks.Allocate(100u, {});
        MemoryHandle c = blocks.Allocate(100u, {});
        assert(a.is_valid() && b.is_valid() && c.is_valid());

        blocks.Free(b);
        MemoryHandle d = blocks.Allocate(100u, {});
        assert(d.is_valid() && d.offset == b.offset); // hole gets reused

        // free everything (out of order) -> blocks must coalesce back into
        // one span large enough for a big allocation
        blocks.Free(a);
        blocks.Free(d);
        blocks.Free(c);
        MemoryHandle big = blocks.Allocate(KB(1) - 64u, {});
        assert(big.is_valid());
        blocks.Free(big);
    }

    // ---- StringBuilder ----
    {
        StringBuilder sb(arena, 8u); // small capacity to force growth
        sb.append("hello");
        sb.appendf(" %s %u", "world", 42u);
        assert(strcmp(sb.c_str(), "hello world 42") == 0);
        assert(sb.length() == 14u);

        sb.clear();
        assert(sb.empty() && sb.c_str()[0] == '\0');
    }

    printf("core tests passed \n");
    fflush(stdout);
}

int main()
{
    run_core_tests();

    BitList<16> freelist;

    freelist.set_bit(1);

    bool first  = freelist[0];
    bool second = freelist[1];

    u32 first_set = freelist.find_first(true);

    assert(!first);
    assert(second);
    assert(first_set == 1);

    freelist.set_bit(12);

    bool twelf          = freelist[12];
    u32  first_from     = freelist.find_first(true, 9);
    u32  first_not_from = freelist.find_first(false, 12);

    assert(twelf);
    assert(first_from == 12);
    assert(first_not_from == 13);

    freelist.unset_bit(1);

    assert(!freelist[1]);

    BitList<512> list_a({2, 4, 8, 16});
    BitList<512> list_b({2, 8, 4, 16});

    bitlist_changed(list_a, list_b);

    printf("hello world \n");

    ArenaAllocator Arena(KB(2));

    struct Peer
    {
        Peer() = default;
        Peer(u32 val) : Data(val) {};

        u32 Flags = 0;
        u32 Data  = 69;
    };

    // size_t peerSize = sizeof(Peer);

    Arena.FreeAll();

    // Array allocated on the stack of the scope
    StaticArray<Peer, 16> Peren;

    StaticArray<Peer, 16>* AllocatedPeren = nullptr;
    MemoryHandle peren_memory = Arena.Create(&AllocatedPeren);

    printf("gewone peer: %u \n", Peren[4].Data);
    printf("gewone peer: %u \n", AllocatedPeren->Data[4].Data);

    Arena.Free(peren_memory);

    StaticArray<u32, 8> Numbers({1, 2, 3, 4, 5});

    // for (uint32 i = 0; i < Numbers.Size(); i++) {
    //     printf("%u, ", Numbers[i]);
    // }

    Array<u32> DynPeren = Array<u32>(Arena, {1, 2, 3, 4, 5, 6});

    Array<Peer> MorePeren(Arena);
    MorePeren = CreateView(Peren);

    for (const Peer& p : MorePeren)
    {
        printf("%u, %u \n", p.Data, p.Flags);
    }

    int* getallen = new int[8];

    delete[] getallen;

    return 0;
}
