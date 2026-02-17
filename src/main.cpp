#include "core/BitList.hpp"
#include "core/core.hpp"
#include <cassert>
#include <stdio.h>

// #include "core/core.hpp"
#include "core/Allocators.hpp"
#include "core/Containers.hpp"
#include "graphics/renderer.hpp"

int main()
{

    AE_Renderer renderer;

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

    // renderer.init_renderer();

    // uint32 FoundDevices = 0u;
    // VkPhysicalDevice Devices[8] = {};
    // vkEnumeratePhysicalDevices(
    //     Instance,
    //     &FoundDevices,
    //     nullptr
    // );

    // if (FoundDevices == 0u)
    // {
    //     printf("Error \n");
    //     return 0;
    // }

    // vkEnumeratePhysicalDevices(
    //     Instance,
    //     &FoundDevices,
    //     Devices
    // );

    // vk::PhysicalDevice PhysicalDevice(Devices[0]);

    // // PhysicalDevice.getQueueFamilyProperties();

    // Instance.destroy();

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
