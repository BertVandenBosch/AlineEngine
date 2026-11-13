#include "core/BitList.hpp"
#include "core/core.hpp"
#include <cassert>
#include <stdio.h>

// #include "core/core.hpp"
#include "core/Allocators.hpp"
#include "core/Containers.hpp"
#include "core/Pool.hpp"
#include "core/StringBuilder.hpp"
#include "gfx/Scene.hpp"
#include "gfx/ShaderLibrary.hpp"
#include "gfx/mesh_shared.h"
#include "import/GltfImport.hpp"
#include "platform/Window.hpp"

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/math.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>

void run_gltf_import_tests(); // import/GltfImportTests.cpp
void run_scene_tests();       // gfx/SceneTests.cpp

ALINE_SHADER(debug_background_vs, "gfx/shaders/debug_background.slang", vertex, "vertexMain");
ALINE_SHADER(debug_background_fs, "gfx/shaders/debug_background.slang", fragment, "fragmentMain");

ALINE_SHADER(mesh_vs, "gfx/shaders/mesh.slang", vertex, "vertexMain");
ALINE_SHADER(mesh_fs, "gfx/shaders/mesh.slang", fragment, "fragmentMain");

//TODO: handle this gracefully inside import directory
static const char* asset(const char* relative)
{
    static char path[1024];
    snprintf(path, sizeof(path), "%s/%s", ALINE_ROOT_DIR, relative);
    return path;
}

// Opens a window and draws the imported pirate, N.L lit, over a fullscreen gradient until it is closed.
static int run_window()
{
    using namespace gpu;

    // Until there is an input system, shader reload runs once at this frame (~10 s at 60 Hz):
    // edit debug_background.slang or mesh.slang before then to see it picked up.
    constexpr u64 reload_test_frame = 600;
    constexpr u32 frames_in_flight  = 2;

    ArenaAllocator         scratch_import(MB(4));
    ClusterMesh            pirate_cpu    = {};
    const GltfImportResult import_result = import_gltf(scratch_import, asset("src/meshoptimizer/demo/pirate.glb"), pirate_cpu);
    if (import_result != GltfImportResult::ok)
    {
        printf("pirate import failed (%u)\n", (u32)import_result);
        return 1;
    }
    // Drawn without LOD selection: LOD0 is the leading cluster range.
    const u32    pirate_clusters = pirate_cpu.lod_count > 1u ? pirate_cpu.lods[1].first_cluster : pirate_cpu.clusters.num_elements;
    const float4 pirate_sphere   = pirate_cpu.sphere;

    Window* window = window_create({});
    if (!window)
        return 1;
    Device* device = create_device({.window = window_native_handle(window), .swapchain_format = Format::bgra8_srgb}).device;
    if (!device)
    {
        window_destroy(window);
        return 1;
    }
    printf("Using %s\n", get_device_caps(device).device_name);

    ArenaAllocator  arena(MB(64));
    ArenaAllocator  scratch(KB(256)); // compiled shader bytes while a PSO is built
    ShaderLibrary*  shaders     = shader_library_create(arena, scratch, device);
    if (!shaders)
    {
        destroy_device(device);
        window_destroy(window);
        return 1;
    }

    Scene* scene = scene_create(arena, device, {.frames_in_flight = frames_in_flight}, shaders);

    // Upload the mesh to the scene and the GPU
    StaticMeshHandle pirate_static_mesh = {};
    if (scene)
    {
        const MeshHandle      pirate_mesh      = scene_add_mesh(scene, pirate_cpu);
        const TransformHandle pirate_transform = scene_create_transform(scene, math::identity3x4());
        pirate_static_mesh                     = scene_create_static_mesh(scene, pirate_mesh, pirate_transform);
    }

    // The pirate mesh data is queued for GPU upload from a host visible heap: the CPU copy is no longer needed.
    scratch_import.free_all();

    ColorTargetDesc color      = {.format = Format::bgra8_srgb};
    const PsoHandle background = pso_create_graphics(shaders, debug_background_vs, debug_background_fs,
                                                     {.color_targets = {&color, 1}, .depth_format = Format::d32_float});
    const PsoHandle render_mesh_pso = pso_create_graphics(shaders, mesh_vs, mesh_fs,
                                                          {.color_targets = {&color, 1},
                                                           .depth_format  = Format::d32_float,
                                                           .rasterization = {.cull = CullMode::clockwise}});

    // One draw root per frame in flight, rewritten by the CPU each frame.
    const GpuHeap     root_heap  = create_gpu_heap(device, sizeof(MeshDrawRoot) * frames_in_flight);
    const TextureHeap depth_heap = create_texture_heap(device, MB(64));
    if (!pirate_static_mesh.valid() || !background || !render_mesh_pso || !root_heap.owner || !depth_heap.owner)
    {
        if (depth_heap.owner)
            destroy_texture_heap(depth_heap);
        if (root_heap.owner)
            destroy_gpu_heap(root_heap);
        if (scene)
            scene_destroy(scene);
        shader_library_destroy(shaders);
        destroy_device(device);
        window_destroy(window);
        return 1;
    }
    MeshDrawRoot* const roots_cpu = (MeshDrawRoot*)root_heap.range.cpu;
    MeshDrawRoot* const roots_gpu = (MeshDrawRoot*)root_heap.range.gpu;

    TextureAllocator depth_allocator(device, depth_heap, 1);
    PlacedTexture    depth        = {};
    RenderView*      depth_view   = nullptr;
    uint32x2         depth_extent = {};

    // Camera on +Z, framing the bounding sphere.
    const float3   pirate_center = {.x = pirate_sphere.x, .y = pirate_sphere.y, .z = pirate_sphere.z};
    const float4x4 view          = math::look_at_rh({.x = pirate_center.x, .y = pirate_center.y, .z = pirate_center.z + 3.0f * pirate_sphere.w},
                                                    pirate_center, {.x = 0.0f, .y = 1.0f, .z = 0.0f});
    const GpuScene gpu_scene     = scene_gpu_view(scene);

    TimelinePoint latest_completion{.semaphore = create_timeline_semaphore(device)};
    CommandPool*  command_pools[frames_in_flight];
    for (CommandPool*& pool : command_pools)
        pool = create_command_pool(device);

    while (window_pump(window))
    {
        if (latest_completion.value == reload_test_frame)
        {
            const ShaderReloadResult reload = shader_library_reload(shaders);
            printf("shader reload: build_ok=%d reloaded=%u failed=%u\n", reload.build_ok, reload.reloaded,
                   reload.failed);
            fflush(stdout);
        }

        // Frame f waits for frame f - frames_in_flight, as scene_begin_frame requires.
        const u64 frame_index = latest_completion.value;
        if (frame_index >= frames_in_flight)
            wait_timeline({.semaphore = latest_completion.semaphore, .value = frame_index - (frames_in_flight - 1)});
        CommandPool* command_pool = command_pools[frame_index % frames_in_flight];
        reset_command_pool(command_pool);
        CommandBuffer*       commands = begin_commands(command_pool);
        const SwapchainFrame frame    = acquire(commands);
        if (!frame.render_view)
        {
            // Never submitted: leave the scene alone so no staged change is consumed by these commands.
            end_commands(commands);
            continue;
        }

        scene_begin_frame(scene, frame_index);
        scene_record_updates(scene, commands);

        if (frame.extent.x != depth_extent.x || frame.extent.y != depth_extent.y)
        {
            if (depth.texture)
            {
                wait_timeline(latest_completion); // submitted frames may still use the old depth target
                destroy_render_view(depth_view);
                depth_allocator.free(depth);
            }
            depth = depth_allocator.allocate({.extent = {.x = frame.extent.x, .y = frame.extent.y, .z = 1},
                                              .format = Format::d32_float,
                                              .usage  = TextureUsage::depth_stencil_attachment});
            assert(depth.texture && "depth target does not fit its texture heap");
            initialize_texture(commands, depth.texture);
            depth_view   = create_render_view(depth.texture);
            depth_extent = frame.extent;
        }

        float4x4 projection = math::perspective_rh_zo(45.0f * math::pi / 180.0f, float(frame.extent.x) / float(frame.extent.y),
                                                      0.05f * pirate_sphere.w, 20.0f * pirate_sphere.w);
        projection.rows[1].y = -projection.rows[1].y;
        const u32 root_slot  = (u32)(frame_index % frames_in_flight);
        roots_cpu[root_slot] = {
            .view_projection = projection * view,
            .meshes          = gpu_scene.meshes,
            .transforms      = gpu_scene.transforms,
            .static_meshes   = gpu_scene.static_meshes,
            .static_mesh     = (u32)pirate_static_mesh.index,
        };

        barrier(commands, Stage::depth_stencil_tests, Access::depth_stencil_write, Stage::depth_stencil_tests,
                Access::depth_stencil_write);
        begin_render_pass(commands, {.colors = {{.render_view = frame.render_view, .load = LoadOp::discard}},
                                     .depth  = {.render_view = depth_view, .load = LoadOp::clear, .store = StoreOp::discard}});
        bind_pso(commands, pso_get(shaders, background));
        draw(commands, {}, 3);
        set_depth_stencil(commands, {.depth_test = true, .depth_write = true});
        bind_pso(commands, pso_get(shaders, render_mesh_pso));
        draw(commands, roots_gpu + root_slot, cluster_max_triangles * 3u, pirate_clusters);
        end_render_pass(commands);
        end_commands(commands);

        // Frames must not run before the mesh upload has landed.
        const TimelinePoint upload = scene_upload_completion(scene);
        latest_completion.value++;
        submit_and_present(device, {.commands   = {commands},
                                    .waits      = upload.semaphore ? Span<const TimelinePoint>(&upload, 1u) : Span<const TimelinePoint>(),
                                    .completion = latest_completion});
    }

    wait_idle(device);
    for (CommandPool* pool : command_pools)
        destroy_command_pool(pool);
    destroy_timeline_semaphore(latest_completion.semaphore);
    if (depth.texture)
    {
        destroy_render_view(depth_view);
        depth_allocator.free(depth);
    }
    destroy_texture_heap(depth_heap);
    destroy_gpu_heap(root_heap);
    scene_destroy(scene);
    shader_library_destroy(shaders);
    destroy_device(device);
    window_destroy(window); // after the device: the swapchain uses its layer
    return 0;
}

static void run_core_tests()
{
    ArenaAllocator arena(KB(64));

    // ---- Array: grow from empty, insert, remove ----
    {
        Array<u32> arr(arena);
        for (u32 i = 0; i < 10u; i++)
        {
            arr.add(i); // must grow from capacity 0 without faulting
        }
        assert(arr.size() == 10u && arr[9] == 9u);

        arr.insert_at(0u, 100u);
        assert(arr.size() == 11u && arr[0] == 100u && arr[1] == 0u &&
               arr[10] == 9u);

        arr.remove_at(0u);
        assert(arr.size() == 10u && arr[0] == 0u && arr[9] == 9u);

        arr.emplace(77u);
        assert(arr.size() == 11u && arr[10] == 77u);

        arr.pop_back();
        arr.clear();
        assert(arr.size() == 0u);
    }

    // ---- View: iteration covers every element ----
    {
        Array<u32> arr(arena, {1, 2, 3, 4, 5});
        u32        count = 0;
        u32        sum   = 0;
        for (const u32 v : create_view(arr))
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

        Pool<u64, TestHandle> test_pool(arena, 2u);

        TestHandle a = test_pool.create(11u);
        assert(a.index == 0u && a.valid()); // index 0 is a normal slot
        assert(test_pool.get(a) && *test_pool.get(a) == 11u);
        assert(test_pool.num_objects() == 1u);

        TestHandle stale = a;
        test_pool.destroy(a);
        assert(test_pool.get(stale) == nullptr); // stale handle rejected
        assert(test_pool.num_objects() == 0u);

        TestHandle b = test_pool.create(22u);
        assert(b.index == 0u && b.gen != stale.gen); // slot reused, gen bumped
        assert(test_pool.get(stale) == nullptr && *test_pool.get(b) == 22u);

        TestHandle c = test_pool.create(33u);
        TestHandle d = test_pool.create(44u); // forces growth past start_size 2
        assert(*test_pool.get(c) == 33u && *test_pool.get(d) == 44u &&
               *test_pool.get(b) == 22u);
        assert(test_pool.num_objects() == 3u);

        assert(TestHandle{}.empty()); // default handle is never valid
        assert(test_pool.get(TestHandle{}) == nullptr);

        test_pool.clear();
        assert(test_pool.num_objects() == 0u && test_pool.get(b) == nullptr);

        // 64-bit handle layout must instantiate
        typedef PoolHandle<u64, 32u, 32u> WideHandle;
        WideHandle                        w{};
        w.index = 0xFFFFFFFFu;
        w.gen   = 1u;
        assert(w.index == 0xFFFFFFFFu && w.valid());
    }

    // ---- LinearBlockAllocator: free + coalesce ----
    {
        LinearBlockAllocator blocks(KB(1));

        MemoryHandle a = blocks.allocate(100u, {});
        MemoryHandle b = blocks.allocate(100u, {});
        MemoryHandle c = blocks.allocate(100u, {});
        assert(a.is_valid() && b.is_valid() && c.is_valid());

        blocks.free(b);
        MemoryHandle d = blocks.allocate(100u, {});
        assert(d.is_valid() && d.offset == b.offset); // hole gets reused

        // free everything (out of order) -> blocks must coalesce back into
        // one span large enough for a big allocation
        blocks.free(a);
        blocks.free(d);
        blocks.free(c);
        MemoryHandle big = blocks.allocate(KB(1) - 64u, {});
        assert(big.is_valid());
        blocks.free(big);
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
    run_gltf_import_tests();
    run_scene_tests();

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

    ArenaAllocator arena(KB(2));

    struct Peer
    {
        Peer() = default;
        Peer(u32 val) : data(val) {};

        u32 flags = 0;
        u32 data  = 69;
    };

    // size_t peer_size = sizeof(Peer);

    arena.free_all();

    // Array allocated on the stack of the scope
    StaticArray<Peer, 16> peren;

    StaticArray<Peer, 16>* allocated_peren = nullptr;
    MemoryHandle           peren_memory    = arena.create(&allocated_peren);

    printf("gewone peer: %u \n", peren[4].data);
    printf("gewone peer: %u \n", allocated_peren->data[4].data);

    arena.free(peren_memory);

    StaticArray<u32, 8> numbers({1, 2, 3, 4, 5});

    // for (uint32 i = 0; i < numbers.size(); i++) {
    //     printf("%u, ", numbers[i]);
    // }

    Array<u32> dyn_peren = Array<u32>(arena, {1, 2, 3, 4, 5, 6});

    Array<Peer> more_peren(arena);
    more_peren = create_view(peren);

    for (const Peer& p : more_peren)
    {
        printf("%u, %u \n", p.data, p.flags);
    }

    int* getallen = new int[8];

    delete[] getallen;

    return run_window();
}
