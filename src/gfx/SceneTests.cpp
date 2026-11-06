#define _CRT_SECURE_NO_WARNINGS // plain fopen for the shader file

#include "gfx/Scene.hpp"

#include <stdio.h>
#include <string.h>

#if !defined(ALINE_ROOT_DIR) || !defined(ALINE_SHADER_DIR) || !defined(ALINE_SHADER_EXTENSION)
#error "ALINE_ROOT_DIR, ALINE_SHADER_DIR and ALINE_SHADER_EXTENSION are defined by premake5.lua"
#endif

/*
 * Scene tests on a headless device: after every frame the persistent GPU
 * arrays are read back and compared with values the test tracks itself.
 * They rely on assert, so they only run in Debug.
 */

#if defined(NDEBUG)

void run_scene_tests() { printf("scene tests skipped (asserts disabled)\n"); }

#else

namespace
{
constexpr u32 test_transforms = 64u, test_static_meshes = 64u, test_meshes = 4u;

struct TestGpu
{
    gpu::Device*            device;
    gpu::TimelineSemaphore* timeline;
    gpu::CommandPool*       pool;
    gpu::GpuHeap            readback;
    u64                     timeline_value;
};

// Expected contents of the persistent arrays, tracked independently of the scene.
struct Expected
{
    float3x4      transforms[test_transforms];
    bool          transform_live[test_transforms];
    GpuStaticMesh static_meshes[test_static_meshes];
};

float3x4 test_matrix(float seed)
{
    float3x4 m;
    for (u32 r = 0; r < 3; r++)
    {
        m.rows[r] = {seed + (float)r, seed * 2.0f + (float)r, seed * 3.0f - (float)r, seed + 0.5f * (float)r};
    }
    return m;
}

TransformHandle create_transform(Scene* scene, Expected& expected, float seed)
{
    const TransformHandle handle = scene_create_transform(scene, test_matrix(seed));
    assert(handle.valid());
    expected.transforms[(u32)handle.index]     = test_matrix(seed);
    expected.transform_live[(u32)handle.index] = true;
    return handle;
}

void set_transform(Scene* scene, Expected& expected, TransformHandle handle, float seed)
{
    scene_set_transform(scene, handle, test_matrix(seed));
    expected.transforms[(u32)handle.index] = test_matrix(seed);
}

StaticMeshHandle create_static_mesh(Scene* scene, Expected& expected, MeshHandle mesh, TransformHandle transform)
{
    const StaticMeshHandle handle = scene_create_static_mesh(scene, mesh, transform);
    assert(handle.valid());
    expected.static_meshes[(u32)handle.index] = {
        .mesh = (u32)mesh.index, .transform = (u32)transform.index, .flags = static_mesh_visible, .reserved = 0u};
    return handle;
}

gpu::CommandBuffer* begin(TestGpu& state)
{
    gpu::reset_command_pool(state.pool);
    return gpu::begin_commands(state.pool);
}

// Ends, submits (after the scene's mesh uploads) and waits; then readback holds the copies.
void submit_and_wait(TestGpu& state, Scene* scene, gpu::CommandBuffer* commands)
{
    gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    gpu::end_commands(commands);
    const gpu::TimelinePoint upload = scene_upload_completion(scene);
    const gpu::TimelinePoint done   = {.semaphore = state.timeline, .value = ++state.timeline_value};
    gpu::submit(state.device, {.commands = {commands},
                               .waits = upload.semaphore ? gpu::Span<const gpu::TimelinePoint>(&upload, 1u)
                                                         : gpu::Span<const gpu::TimelinePoint>(),
                               .completion = done});
    gpu::wait_timeline(done);
}

// One frame: `changes` edits the scene, the scatter is recorded, the persistent arrays are copied back and
// compared. Every frame is waited on, so frame f - 2 has completed before scene_begin_frame(f).
template <typename Changes>
void run_frame(TestGpu& state, Scene* scene, u64 frame, const Expected& expected, Changes&& changes)
{
    gpu::CommandBuffer* commands = begin(state);
    scene_begin_frame(scene, frame);
    changes();
    scene_record_updates(scene, commands);

    const GpuScene view       = scene_gpu_view(scene);
    byte*          readback   = state.readback.range.gpu;
    const u64      transforms = sizeof(float3x4) * test_transforms;
    const u64      statics    = sizeof(GpuStaticMesh) * test_static_meshes;
    gpu::copy_memory(commands, {.gpu = view.transforms, .size = transforms}, {.gpu = readback, .size = transforms});
    gpu::copy_memory(commands, {.gpu = view.static_meshes, .size = statics}, {.gpu = readback + transforms, .size = statics});
    submit_and_wait(state, scene, commands);

    const float3x4*      actual_transforms = (const float3x4*)state.readback.range.cpu;
    const GpuStaticMesh* actual_statics    = (const GpuStaticMesh*)(state.readback.range.cpu + transforms);
    for (u32 i = 0; i < test_transforms; i++)
    {
        assert(!expected.transform_live[i] || memcmp(&actual_transforms[i], &expected.transforms[i], sizeof(float3x4)) == 0);
    }
    for (u32 i = 0; i < view.static_mesh_count; i++)
    {
        assert(memcmp(&actual_statics[i], &expected.static_meshes[i], sizeof(GpuStaticMesh)) == 0);
    }
}

// The uploaded GpuMesh must equal make_gpu_mesh at its blob address, and the blob bytes must match the source.
void check_mesh(TestGpu& state, Scene* scene, MeshHandle handle, const ClusterMesh& mesh, const u8* blob)
{
    gpu::CommandBuffer* commands = begin(state);
    gpu::copy_memory(commands, {.gpu = &scene_gpu_view(scene).meshes[(u32)handle.index], .size = sizeof(GpuMesh)},
                     {.gpu = state.readback.range.gpu, .size = sizeof(GpuMesh)});
    submit_and_wait(state, scene, commands);

    GpuMesh record;
    memcpy(&record, state.readback.range.cpu, sizeof(record));
    const GpuMesh expected = make_gpu_mesh(mesh, (byte*)record.clusters); // only uses offsets within the blob
    assert(memcmp(&record, &expected, sizeof(GpuMesh)) == 0);

    commands = begin(state);
    gpu::copy_memory(commands, {.gpu = record.clusters, .size = mesh.blob.num_elements},
                     {.gpu = state.readback.range.gpu, .size = mesh.blob.num_elements});
    submit_and_wait(state, scene, commands);
    assert(memcmp(state.readback.range.cpu, blob, mesh.blob.num_elements) == 0);
}

// Reads the compiled scatter shader into the arena (16-byte aligned; SPIR-V needs 4).
gpu::ByteSpan load_shader(ArenaAllocator<>& arena, const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return {};
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    u8*                data   = nullptr;
    const MemoryHandle memory = arena.create_array<u8>(data, (u32)size);
    const bool         ok     = memory.is_valid() && fread(data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    return ok ? gpu::ByteSpan(data, (size_t)size) : gpu::ByteSpan();
}

// Imports a sample model into scratch arena memory, keeping a copy of the blob for check_mesh.
ClusterMesh import_sample(ArenaAllocator<>& scratch, const char* relative, u8* blob_copy, u64 blob_capacity)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", ALINE_ROOT_DIR, relative);
    ClusterMesh mesh = {};
    assert(import_gltf(scratch, path, mesh) == GltfImportResult::ok && mesh.blob.num_elements <= blob_capacity);
    memcpy(blob_copy, mesh.blob.data, mesh.blob.num_elements);
    return mesh;
}
} // namespace

void run_scene_tests()
{
    gpu::DeviceInit init = gpu::create_device();
    if (init.error != gpu::Error::none)
    {
        printf("scene tests skipped (no compatible GPU device)\n");
        return;
    }

    ArenaAllocator arena(MB(1));    // shader + the scene's CPU arrays
    ArenaAllocator scratch(MB(16)); // glTF imports, rewound once uploaded
    TestGpu        state = {.device = init.device};
    state.timeline       = gpu::create_timeline_semaphore(state.device);
    state.pool           = gpu::create_command_pool(state.device);
    state.readback       = gpu::create_gpu_heap(state.device, MB(1), gpu::MemoryType::readback);

    const gpu::ByteSpan shader = load_shader(arena, ALINE_SHADER_DIR "/scene_scatter" ALINE_SHADER_EXTENSION);
    assert(shader.data && "scene_scatter shader missing");
    Scene* scene = scene_create(arena, state.device,
                                {.max_transforms = test_transforms, .max_static_meshes = test_static_meshes,
                                 .max_meshes = test_meshes, .mesh_blob_bytes = MB(1), .frames_in_flight = 2u},
                                shader);
    assert(scene);

    static Expected expected = {};
    static u8       cube_blob[KB(64)], pirate_blob[KB(256)];
    ClusterMesh     cube, pirate;
    MeshHandle      cube_mesh, pirate_mesh;
    TransformHandle t0, t1, t2;
    StaticMeshHandle s0, s1, s2, s3;

    // Frame 0: two meshes (their CPU data is dropped right after), three transforms, three instances.
    // t1 is written twice in the frame: one scatter entry, last value wins. s0 and s1 share t0.
    run_frame(state, scene, 0u, expected, [&]
    {
        cube        = import_sample(scratch, "src/tinygltf/models/Cube/Cube.gltf", cube_blob, sizeof(cube_blob));
        cube_mesh   = scene_add_mesh(scene, cube);
        pirate      = import_sample(scratch, "src/meshoptimizer/demo/pirate.glb", pirate_blob, sizeof(pirate_blob));
        pirate_mesh = scene_add_mesh(scene, pirate);
        assert(cube_mesh.valid() && pirate_mesh.valid());
        scratch.free_all(); // no mesh data left on the CPU

        t0 = create_transform(scene, expected, 1.0f);
        t1 = create_transform(scene, expected, 2.0f);
        t2 = create_transform(scene, expected, 3.0f);
        set_transform(scene, expected, t1, 4.0f);
        s0 = create_static_mesh(scene, expected, cube_mesh, t0);
        s1 = create_static_mesh(scene, expected, pirate_mesh, t0);
        s2 = create_static_mesh(scene, expected, cube_mesh, t1);
    });
    check_mesh(state, scene, cube_mesh, cube, cube_blob);
    check_mesh(state, scene, pirate_mesh, pirate, pirate_blob);

    // Frame 1: destroy + recreate within one frame reuses the slot with a new generation, and the stale handle
    // is rejected. Destroying an instance writes an empty record (flags 0), recreating in that slot overrides it.
    run_frame(state, scene, 1u, expected, [&]
    {
        set_transform(scene, expected, t0, 5.0f);

        scene_destroy_transform(scene, t2);
        const TransformHandle t3 = create_transform(scene, expected, 6.0f);
        assert(t3.index == t2.index && t3.gen != t2.gen);
        scene_set_transform(scene, t2, test_matrix(99.0f)); // stale: ignored

        scene_destroy_static_mesh(scene, s2);
        expected.static_meshes[(u32)s2.index] = {};
        s3 = create_static_mesh(scene, expected, pirate_mesh, t3);
        assert(s3.index == s2.index && s3.gen != s2.gen);
        scene_set_static_mesh_flags(scene, s2, 7u); // stale: ignored

        scene_set_static_mesh_flags(scene, s0, 0u);
        expected.static_meshes[(u32)s0.index].flags = 0u;
    });

    // Frame 2: nothing changed, so no scatter; the arrays keep their values.
    run_frame(state, scene, 2u, expected, [] {});

    // Frame 3: remove the pirate mesh after destroying its instances. Its slot stays reserved until frame 5
    // (frames in flight), so a new mesh can't reuse it yet; instances can no longer reference it.
    MeshHandle cube_again = {};
    run_frame(state, scene, 3u, expected, [&]
    {
        for (const StaticMeshHandle handle : {s1, s3})
        {
            scene_destroy_static_mesh(scene, handle);
            expected.static_meshes[(u32)handle.index] = {};
        }
        scene_remove_mesh(scene, pirate_mesh);
        assert(!scene_create_static_mesh(scene, pirate_mesh, t0).valid());
    });
    run_frame(state, scene, 4u, expected, [] {});
    run_frame(state, scene, 5u, expected, [&]
    {
        cube        = import_sample(scratch, "src/tinygltf/models/Cube/Cube.gltf", cube_blob, sizeof(cube_blob));
        cube_again  = scene_add_mesh(scene, cube);
        scratch.free_all();
        assert(cube_again.index == pirate_mesh.index && cube_again.gen != pirate_mesh.gen); // retired slot reused
    });
    check_mesh(state, scene, cube_again, cube, cube_blob);

    gpu::wait_idle(state.device);
    scene_destroy(scene);
    gpu::destroy_gpu_heap(state.readback);
    gpu::destroy_command_pool(state.pool);
    gpu::destroy_timeline_semaphore(state.timeline);
    gpu::destroy_device(state.device);

    printf("scene tests passed\n");
    fflush(stdout);
}

#endif
