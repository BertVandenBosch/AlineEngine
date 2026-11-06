#include "gfx/Scene.hpp"

#include <NoGraphicsAPIUtility/heap_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>

// CPU-side record of an instance: enough to rewrite its GPU record (flags changes, destroy).
struct StaticMeshRecord
{
    u32 mesh;
    u32 transform;
    u32 flags;
};

struct MeshSlot
{
    gpu::HeapAllocation<byte> blob;
    u64                       retire_frame; // frame of scene_remove_mesh
    bool                      removed;
};

// Byte layout of one frame's upload slice (all offsets 16-byte aligned).
struct SliceLayout
{
    u64 transform_indices;
    u64 transform_values;
    u64 static_mesh_indices;
    u64 static_mesh_values;
    u64 size;
};

static SliceLayout slice_layout(const SceneDesc& desc)
{
    SliceLayout layout;
    layout.transform_indices   = sizeof(SceneScatterRoot);
    layout.transform_values    = round_to<u64>(layout.transform_indices + sizeof(u32) * desc.max_transforms, 16);
    layout.static_mesh_indices = round_to<u64>(layout.transform_values + sizeof(float3x4) * desc.max_transforms, 16);
    layout.static_mesh_values  = round_to<u64>(layout.static_mesh_indices + sizeof(u32) * desc.max_static_meshes, 16);
    layout.size                = round_to<u64>(layout.static_mesh_values + sizeof(GpuStaticMesh) * desc.max_static_meshes, 16);
    return layout;
}

struct Scene
{
    Scene(ArenaAllocator<>& arena, gpu::Device* device, const SceneDesc& desc, gpu::GpuHeap scene_heap,
          gpu::GpuHeap blob_heap, gpu::GpuHeap upload_heap, gpu::PSO* scatter_pso)
        : desc(desc), slice(slice_layout(desc)), device(device), scene_heap(scene_heap), blob_heap(blob_heap),
          upload_heap(upload_heap), scatter_pso(scatter_pso), blobs(blob_heap.range, desc.max_meshes), uploads(device),
          transforms(arena, desc.max_transforms), static_meshes(arena, desc.max_static_meshes), meshes(arena, desc.max_meshes),
          transform_pending(arena, desc.max_transforms), static_mesh_pending(arena, desc.max_static_meshes),
          touched_transforms(arena, desc.max_transforms), touched_static_meshes(arena, desc.max_static_meshes),
          retiring_meshes(arena, desc.max_meshes)
    {
        // Pools and lists are allocated at full capacity up front, so nothing ever regrows (the arena can't free).
        transform_pending.resize(desc.max_transforms);
        static_mesh_pending.resize(desc.max_static_meshes);

        gpu_transforms    = (float3x4*)scene_heap.range.gpu;
        gpu_static_meshes = (GpuStaticMesh*)(scene_heap.range.gpu + round_to<u64>(sizeof(float3x4) * desc.max_transforms, 16));
        gpu_meshes        = (GpuMesh*)((byte*)gpu_static_meshes + round_to<u64>(sizeof(GpuStaticMesh) * desc.max_static_meshes, 16));
    }

    SceneDesc   desc;
    SliceLayout slice;

    gpu::Device*       device;
    gpu::GpuHeap       scene_heap;  // gpu_only: transforms | static meshes | meshes
    gpu::GpuHeap       blob_heap;   // gpu_only: mesh blobs
    gpu::GpuHeap       upload_heap; // cpu_visible: one slice per frame in flight
    gpu::PSO*          scatter_pso;
    gpu::HeapAllocator blobs;
    gpu::UploadQueue   uploads; // one-shot mesh uploads

    float3x4*      gpu_transforms;
    GpuStaticMesh* gpu_static_meshes;
    GpuMesh*       gpu_meshes;

    Pool<u8, TransformHandle>                  transforms; // generations only; matrices live on the GPU
    Pool<StaticMeshRecord, StaticMeshHandle>   static_meshes;
    Pool<MeshSlot, MeshHandle>                 meshes;

    // Per slot: 1 + this frame's upload entry, or 0. Indexed by slot (not pool payload) so a slot destroyed and
    // reused within one frame still maps to a single entry, and the scatter never writes an element twice.
    Array<u32> transform_pending;
    Array<u32> static_mesh_pending;
    Array<u32> touched_transforms;
    Array<u32> touched_static_meshes;
    Array<MeshHandle> retiring_meshes;

    u64  frame_index      = 0;
    u32  slice_index      = 0;
    u32  transform_count   = 0;
    u32  static_mesh_count = 0;
    bool recorded          = false; // changes after scene_record_updates would be lost

    bool               uploads_pending   = false;
    gpu::TimelinePoint upload_completion = {};
};

template <typename T>
static T* slice_cpu(Scene* scene, u64 offset)
{
    return (T*)(scene->upload_heap.range.cpu + scene->slice.size * scene->slice_index + offset);
}

template <typename T>
static T* slice_gpu(Scene* scene, u64 offset)
{
    return (T*)(scene->upload_heap.range.gpu + scene->slice.size * scene->slice_index + offset);
}

// Mapped memory is write-combined: only ever write it, whole elements at a time.
static void write_transform(Scene* scene, u32 index, const float3x4& world)
{
    assert(!scene->recorded && "scene changes must happen between scene_begin_frame and scene_record_updates");
    u32& pending = scene->transform_pending[index];
    if (pending == 0u)
    {
        pending = ++scene->transform_count;
        scene->touched_transforms.add(index);
        slice_cpu<u32>(scene, scene->slice.transform_indices)[pending - 1u] = index;
    }
    slice_cpu<float3x4>(scene, scene->slice.transform_values)[pending - 1u] = world;
}

static void write_static_mesh(Scene* scene, u32 index, const GpuStaticMesh& record)
{
    assert(!scene->recorded && "scene changes must happen between scene_begin_frame and scene_record_updates");
    u32& pending = scene->static_mesh_pending[index];
    if (pending == 0u)
    {
        pending = ++scene->static_mesh_count;
        scene->touched_static_meshes.add(index);
        slice_cpu<u32>(scene, scene->slice.static_mesh_indices)[pending - 1u] = index;
    }
    slice_cpu<GpuStaticMesh>(scene, scene->slice.static_mesh_values)[pending - 1u] = record;
}

static const MeshSlot* live_mesh(Scene* scene, MeshHandle mesh)
{
    const MeshSlot* slot = scene->meshes.get(mesh);
    return slot && !slot->removed ? slot : nullptr;
}

Scene* scene_create(ArenaAllocator<>& arena, gpu::Device* device, const SceneDesc& desc, gpu::ByteSpan scatter_shader)
{
    assert(desc.max_transforms > 0u && desc.max_transforms <= TransformHandle::MAX_INDEX + 1u);
    assert(desc.max_static_meshes > 0u && desc.max_static_meshes <= StaticMeshHandle::MAX_INDEX + 1u);
    assert(desc.max_meshes > 0u && desc.max_meshes <= MeshHandle::MAX_INDEX + 1u && desc.frames_in_flight > 0u);

    const u64 scene_bytes = round_to<u64>(sizeof(float3x4) * desc.max_transforms, 16) +
                            round_to<u64>(sizeof(GpuStaticMesh) * desc.max_static_meshes, 16) +
                            sizeof(GpuMesh) * desc.max_meshes;

    const gpu::GpuHeap scene_heap  = gpu::create_gpu_heap(device, scene_bytes, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap blob_heap   = gpu::create_gpu_heap(device, desc.mesh_blob_bytes, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap upload_heap = gpu::create_gpu_heap(device, slice_layout(desc).size * desc.frames_in_flight);
    gpu::PSO* const    scatter_pso = gpu::create_compute_pso(
        device, {.code = scatter_shader, .entry_point = "computeMain", .threadgroup_size = {.x = scene_scatter_group_size, .y = 1, .z = 1}});

    Scene* scene = nullptr;
    if (scene_heap.owner && blob_heap.owner && upload_heap.owner && scatter_pso)
    {
        const MemoryHandle memory = arena.create(&scene, arena, device, desc, scene_heap, blob_heap, upload_heap, scatter_pso);
        if (memory.is_valid() && !scene->uploads.valid())
        {
            scene->~Scene();
            scene = nullptr;
        }
    }
    if (!scene)
    {
        if (scatter_pso)
        {
            gpu::destroy_pso(scatter_pso);
        }
        for (const gpu::GpuHeap& heap : {scene_heap, blob_heap, upload_heap})
        {
            if (heap.owner)
            {
                gpu::destroy_gpu_heap(heap);
            }
        }
    }
    return scene;
}

void scene_destroy(Scene* scene)
{
    if (!scene)
    {
        return;
    }
    const gpu::GpuHeap heaps[3] = {scene->scene_heap, scene->blob_heap, scene->upload_heap};
    gpu::PSO* const    pso      = scene->scatter_pso;
    scene->~Scene(); // destroys the upload queue; the arena memory itself is not reclaimed
    gpu::destroy_pso(pso);
    for (const gpu::GpuHeap& heap : heaps)
    {
        gpu::destroy_gpu_heap(heap);
    }
}

MeshHandle scene_add_mesh(Scene* scene, const ClusterMesh& mesh)
{
    if (scene->meshes.num_objects() >= scene->desc.max_meshes || mesh.blob.num_elements == 0u)
    {
        return {};
    }
    const gpu::HeapAllocation<byte> blob = scene->blobs.allocate(mesh.blob.num_elements);
    if (!blob.range.gpu)
    {
        return {};
    }

    const MeshHandle handle = scene->meshes.create(MeshSlot{.blob = blob, .retire_frame = 0u, .removed = false});
    const GpuMesh    record = make_gpu_mesh(mesh, blob.range.gpu);
    scene->uploads.upload_buffer({.gpu = blob.range.gpu, .size = mesh.blob.num_elements},
                                 gpu::ByteSpan(mesh.blob.data, mesh.blob.num_elements));
    scene->uploads.upload_buffer({.gpu = &scene->gpu_meshes[(u32)handle.index], .size = sizeof(GpuMesh)},
                                 gpu::ByteSpan(&record, sizeof(record)));
    scene->uploads_pending = true;
    return handle;
}

void scene_remove_mesh(Scene* scene, MeshHandle mesh)
{
    MeshSlot* slot = scene->meshes.get(mesh);
    if (!slot || slot->removed)
    {
        return;
    }
    // Frames in flight may still read the blob and the record slot: keep both until they retire.
    slot->removed      = true;
    slot->retire_frame = scene->frame_index;
    scene->retiring_meshes.add(mesh);
}

TransformHandle scene_create_transform(Scene* scene, const float3x4& world)
{
    if (scene->transforms.num_objects() >= scene->desc.max_transforms)
    {
        return {};
    }
    const TransformHandle handle = scene->transforms.create(u8{1});
    write_transform(scene, (u32)handle.index, world);
    return handle;
}

void scene_set_transform(Scene* scene, TransformHandle transform, const float3x4& world)
{
    if (scene->transforms.get(transform))
    {
        write_transform(scene, (u32)transform.index, world);
    }
}

void scene_destroy_transform(Scene* scene, TransformHandle transform)
{
    // The GPU value goes stale but is never read: no live instance may reference it.
    scene->transforms.destroy(transform);
}

StaticMeshHandle scene_create_static_mesh(Scene* scene, MeshHandle mesh, TransformHandle transform, u32 flags)
{
    if (scene->static_meshes.num_objects() >= scene->desc.max_static_meshes || !live_mesh(scene, mesh) ||
        !scene->transforms.get(transform))
    {
        return {};
    }
    const StaticMeshRecord record = {.mesh = (u32)mesh.index, .transform = (u32)transform.index, .flags = flags};
    const StaticMeshHandle handle = scene->static_meshes.create(StaticMeshRecord(record));
    write_static_mesh(scene, (u32)handle.index, {.mesh = record.mesh, .transform = record.transform, .flags = flags, .reserved = 0u});
    return handle;
}

void scene_set_static_mesh_flags(Scene* scene, StaticMeshHandle static_mesh, u32 flags)
{
    StaticMeshRecord* record = scene->static_meshes.get(static_mesh);
    if (record)
    {
        record->flags = flags;
        write_static_mesh(scene, (u32)static_mesh.index,
                          {.mesh = record->mesh, .transform = record->transform, .flags = flags, .reserved = 0u});
    }
}

void scene_destroy_static_mesh(Scene* scene, StaticMeshHandle static_mesh)
{
    if (scene->static_meshes.get(static_mesh))
    {
        write_static_mesh(scene, (u32)static_mesh.index, {}); // flags 0: an empty slot to the GPU
        scene->static_meshes.destroy(static_mesh);
    }
}

void scene_begin_frame(Scene* scene, u64 frame_index)
{
    scene->frame_index = frame_index;
    scene->slice_index = (u32)(frame_index % scene->desc.frames_in_flight);
    scene->recorded    = false;

    for (const u32 index : scene->touched_transforms)
    {
        scene->transform_pending[index] = 0u;
    }
    for (const u32 index : scene->touched_static_meshes)
    {
        scene->static_mesh_pending[index] = 0u;
    }
    scene->touched_transforms.clear();
    scene->touched_static_meshes.clear();
    scene->transform_count   = 0u;
    scene->static_mesh_count = 0u;

    // Frames up to frame_index - frames_in_flight have completed: their removed meshes can go.
    for (u32 i = 0; i < scene->retiring_meshes.size();)
    {
        const MeshHandle handle = scene->retiring_meshes[i];
        const MeshSlot*  slot   = scene->meshes.get(handle);
        if (frame_index >= slot->retire_frame + scene->desc.frames_in_flight)
        {
            scene->blobs.free(slot->blob);
            scene->meshes.destroy(handle);
            scene->retiring_meshes[i] = scene->retiring_meshes.back();
            scene->retiring_meshes.pop_back();
        }
        else
        {
            i++;
        }
    }
}

void scene_record_updates(Scene* scene, gpu::CommandBuffer* commands)
{
    scene->recorded = true;
    if (scene->uploads_pending)
    {
        scene->upload_completion = scene->uploads.flush();
        scene->uploads_pending   = false;
    }

    const u32 count = scene->transform_count > scene->static_mesh_count ? scene->transform_count : scene->static_mesh_count;
    if (count == 0u)
    {
        return;
    }

    *slice_cpu<SceneScatterRoot>(scene, 0u) = {
        .transform_indices   = slice_gpu<u32>(scene, scene->slice.transform_indices),
        .transform_values    = slice_gpu<float3x4>(scene, scene->slice.transform_values),
        .transforms          = scene->gpu_transforms,
        .static_mesh_indices = slice_gpu<u32>(scene, scene->slice.static_mesh_indices),
        .static_mesh_values  = slice_gpu<GpuStaticMesh>(scene, scene->slice.static_mesh_values),
        .static_meshes       = scene->gpu_static_meshes,
        .transform_count     = scene->transform_count,
        .static_mesh_count   = scene->static_mesh_count,
        .root_alignment_padding0 = {0u, 0u},
    };

    // Earlier readers of the scene arrays finish before the scatter overwrites them; later ones see the result.
    gpu::barrier(commands, gpu::Stage::all_commands, gpu::Access::shader_read | gpu::Access::transfer_read,
                 gpu::Stage::compute, gpu::Access::shader_write);
    gpu::bind_pso(commands, scene->scatter_pso);
    gpu::dispatch(commands, slice_gpu<SceneScatterRoot>(scene, 0u),
                  {.x = (count + scene_scatter_group_size - 1u) / scene_scatter_group_size, .y = 1u, .z = 1u});
    gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::all_commands,
                 gpu::Access::shader_read | gpu::Access::transfer_read);
}

gpu::TimelinePoint scene_upload_completion(const Scene* scene) { return scene->upload_completion; }

GpuScene scene_gpu_view(const Scene* scene)
{
    return {
        .meshes            = scene->gpu_meshes,
        .transforms        = scene->gpu_transforms,
        .static_meshes     = scene->gpu_static_meshes,
        .static_mesh_count = scene->static_meshes.objects.num_elements,
        .reserved          = 0u,
    };
}
