#include "gfx/Scene.hpp"

#include <NoGraphicsAPIUtility/heap_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>

ALINE_SHADER(scene_scatter, "gfx/shaders/scene_scatter.slang", compute, "computeMain");

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
    // Once retire_scheduled: the frame whose scatter carried the last instance change made before the removal.
    u64  retire_frame;
    bool removed;
    bool retire_scheduled;
};

// One frame's block in the upload ring, all offsets 16-byte aligned:
// SceneScatterRoot | static mesh indices | static mesh values | transform indices | transform values
struct BlockLayout
{
    u64 static_mesh_indices;
    u64 static_mesh_values;
    u64 transform_indices;
    u64 transform_values;
    u64 size;
};

static_assert(sizeof(GpuStaticMesh) % 16 == 0 && sizeof(float3x4) % 16 == 0, "value arrays keep 16-byte alignment");

static u64 stream_bytes(u32 count, u64 value_size) { return round_to<u64>(sizeof(u32) * count, 16) + value_size * count; }

static BlockLayout block_layout(u32 static_mesh_count, u32 transform_count)
{
    BlockLayout layout;
    layout.static_mesh_indices = sizeof(SceneScatterRoot);
    layout.static_mesh_values  = layout.static_mesh_indices + round_to<u64>(sizeof(u32) * static_mesh_count, 16);
    layout.transform_indices   = layout.static_mesh_values + sizeof(GpuStaticMesh) * static_mesh_count;
    layout.transform_values    = layout.transform_indices + round_to<u64>(sizeof(u32) * transform_count, 16);
    layout.size                = layout.transform_values + sizeof(float3x4) * transform_count;
    return layout;
}

// Largest count <= total whose stream fits in `bytes`.
static u32 fit_count(u32 total, u64 bytes, u64 value_size)
{
    if (stream_bytes(total, value_size) <= bytes)
    {
        return total;
    }
    // stream_bytes(n) <= (4 + value_size) * n + 12: start from that bound, then take up its slack.
    u32 count = bytes > 12u ? (u32)((bytes - 12u) / (sizeof(u32) + value_size)) : 0u;
    while (count < total && stream_bytes(count + 1u, value_size) <= bytes)
    {
        count++;
    }
    return count;
}

struct Scene
{
	// Everything scene_create acquires before constructing the Scene.
	struct Args
	{
    ArenaAllocator<>& arena;
    gpu::Device*      device;
    const SceneDesc&  desc;
    gpu::GpuHeap      scene_heap;
    gpu::GpuHeap      blob_heap;
    gpu::GpuHeap      upload_heap;
    u64               ring_capacity;
    ShaderLibrary*    shaders;
    PsoHandle         scatter_pso;
	};

    explicit Scene(const Args& init)
        : desc(init.desc), device(init.device), scene_heap(init.scene_heap), blob_heap(init.blob_heap),
          upload_heap(init.upload_heap), shaders(init.shaders), scatter_pso(init.scatter_pso),
          blobs(init.blob_heap.range, init.desc.max_meshes), uploads(init.device),
          transforms(init.arena, init.desc.max_transforms), static_meshes(init.arena, init.desc.max_static_meshes),
          meshes(init.arena, init.desc.max_meshes), transform_pending(init.arena, init.desc.max_transforms),
          static_mesh_pending(init.arena, init.desc.max_static_meshes),
          staged_transform_indices(init.arena, init.desc.max_transforms),
          staged_transform_values(init.arena, init.desc.max_transforms),
          staged_static_mesh_indices(init.arena, init.desc.max_static_meshes),
          staged_static_mesh_values(init.arena, init.desc.max_static_meshes),
          retiring_meshes(init.arena, init.desc.max_meshes), ring_capacity(init.ring_capacity),
          frame_ring_ends(init.arena, init.desc.frames_in_flight)
    {
        // Pools and lists are allocated at full capacity up front, so nothing ever regrows (the arena can't free).
        transform_pending.resize(desc.max_transforms);
        static_mesh_pending.resize(desc.max_static_meshes);
        staged_transform_indices.resize(desc.max_transforms);
        staged_transform_values.resize(desc.max_transforms);
        staged_static_mesh_indices.resize(desc.max_static_meshes);
        staged_static_mesh_values.resize(desc.max_static_meshes);
        frame_ring_ends.resize(desc.frames_in_flight);

        gpu_transforms    = (float3x4*)scene_heap.range.gpu;
        gpu_static_meshes = (GpuStaticMesh*)(scene_heap.range.gpu + round_to<u64>(sizeof(float3x4) * desc.max_transforms, 16));
        gpu_meshes        = (GpuMesh*)((byte*)gpu_static_meshes + round_to<u64>(sizeof(GpuStaticMesh) * desc.max_static_meshes, 16));
    }

    SceneDesc desc;

    gpu::Device*       device;
    gpu::GpuHeap       scene_heap;  // gpu_only: transforms | static meshes | meshes
    gpu::GpuHeap       blob_heap;   // gpu_only: mesh blobs
    gpu::GpuHeap       upload_heap; // cpu_visible: ring of per-frame scatter blocks
    ShaderLibrary*     shaders;
    PsoHandle          scatter_pso;
    gpu::HeapAllocator blobs;
    gpu::UploadQueue   uploads; // one-shot mesh uploads

    float3x4*      gpu_transforms;
    GpuStaticMesh* gpu_static_meshes;
    GpuMesh*       gpu_meshes;

    Pool<u8, TransformHandle>                  transforms; // generations only; matrices live on the GPU
    Pool<StaticMeshRecord, StaticMeshHandle>   static_meshes;
    Pool<MeshSlot, MeshHandle>                 meshes;

    // Changes not yet scattered, in write order; entries [0, *_count) are live. Per slot, *_pending holds
    // 1 + the slot's staged entry, or 0. Indexed by slot (not pool payload) so a slot destroyed and reused
    // before its change is scattered still maps to a single entry, and the scatter never writes an element twice.
    Array<u32>           transform_pending;
    Array<u32>           static_mesh_pending;
    Array<u32>           staged_transform_indices;
    Array<float3x4>      staged_transform_values;
    Array<u32>           staged_static_mesh_indices;
    Array<GpuStaticMesh> staged_static_mesh_values;
    Array<MeshHandle>    retiring_meshes;

    // Upload ring: head and tail are running byte counts (position = count % ring_capacity). Bytes in
    // [tail, head) may still be read by frames in flight. frame_ring_ends[frame % frames_in_flight] is the
    // head after that frame's block: once the frame completes, the tail can move up to it.
    u64       ring_capacity;
    u64       ring_head = 0;
    u64       ring_tail = 0;
    Array<u64> frame_ring_ends;

    u64  frame_index       = 0;
    u32  transform_count   = 0;
    u32  static_mesh_count = 0;
    bool recorded          = false; // changes must wait for the next scene_begin_frame

    u64 ring_peak_bytes = 0;
    u64 deferred_frames = 0;

    bool               uploads_pending   = false;
    gpu::TimelinePoint upload_completion = {};
};

static void write_transform(Scene* scene, u32 index, const float3x4& world)
{
    assert(!scene->recorded && "scene changes must happen between scene_begin_frame and scene_record_updates");
    u32& pending = scene->transform_pending[index];
    if (pending == 0u)
    {
        pending = ++scene->transform_count;
        scene->staged_transform_indices[pending - 1u] = index;
    }
    scene->staged_transform_values[pending - 1u] = world;
}

static void write_static_mesh(Scene* scene, u32 index, const GpuStaticMesh& record)
{
    assert(!scene->recorded && "scene changes must happen between scene_begin_frame and scene_record_updates");
    u32& pending = scene->static_mesh_pending[index];
    if (pending == 0u)
    {
        pending = ++scene->static_mesh_count;
        scene->staged_static_mesh_indices[pending - 1u] = index;
    }
    scene->staged_static_mesh_values[pending - 1u] = record;
}

// Drops the first `consumed` staged entries (just scattered) and moves the rest to the front, in order.
template <typename T>
static void consume_staged(Array<u32>& pending, Array<u32>& indices, Array<T>& values, u32& count, u32 consumed)
{
    for (u32 i = 0; i < consumed; i++)
    {
        pending[indices[i]] = 0u;
    }
    count -= consumed;
    if (consumed == 0u || count == 0u)
    {
        return;
    }
    memmove(indices.data, indices.data + consumed, sizeof(u32) * count);
    memmove(values.data, values.data + consumed, sizeof(T) * count);
    for (u32 i = 0; i < count; i++)
    {
        pending[indices[i]] = i + 1u;
    }
}

static const MeshSlot* live_mesh(Scene* scene, MeshHandle mesh)
{
    const MeshSlot* slot = scene->meshes.get(mesh);
    return slot && !slot->removed ? slot : nullptr;
}

Scene* scene_create(ArenaAllocator<>& arena, gpu::Device* device, const SceneDesc& desc, ShaderLibrary* shaders)
{
    assert(desc.max_transforms > 0u && desc.max_transforms <= TransformHandle::MAX_INDEX + 1u);
    assert(desc.max_static_meshes > 0u && desc.max_static_meshes <= StaticMeshHandle::MAX_INDEX + 1u);
    assert(desc.max_meshes > 0u && desc.max_meshes <= MeshHandle::MAX_INDEX + 1u && desc.frames_in_flight > 0u);
    // Room for at least one block holding a single transform change.
    assert(desc.upload_ring_bytes >= block_layout(0u, 1u).size);

    const u64 scene_bytes = round_to<u64>(sizeof(float3x4) * desc.max_transforms, 16) +
                            round_to<u64>(sizeof(GpuStaticMesh) * desc.max_static_meshes, 16) +
                            sizeof(GpuMesh) * desc.max_meshes;
    const u64 ring_capacity = round_to<u64>(desc.upload_ring_bytes, 16);

    const gpu::GpuHeap scene_heap  = gpu::create_gpu_heap(device, scene_bytes, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap blob_heap   = gpu::create_gpu_heap(device, desc.mesh_blob_bytes, gpu::MemoryType::gpu_only);
    const gpu::GpuHeap upload_heap = gpu::create_gpu_heap(device, ring_capacity);
    const PsoHandle    scatter_pso =
        pso_create_compute(shaders, scene_scatter, {.x = scene_scatter_group_size, .y = 1, .z = 1});

    Scene* scene = nullptr;
    if (scene_heap.owner && blob_heap.owner && upload_heap.owner && scatter_pso)
    {
        const MemoryHandle memory = arena.create(&scene, Scene::Args{.arena         = arena,
                                                                   .device        = device,
                                                                   .desc          = desc,
                                                                   .scene_heap    = scene_heap,
                                                                   .blob_heap     = blob_heap,
                                                                   .upload_heap   = upload_heap,
                                                                   .ring_capacity = ring_capacity,
                                                                   .shaders       = shaders,
                                                                   .scatter_pso   = scatter_pso});
        if (memory.is_valid() && !scene->uploads.valid())
        {
            scene->~Scene();
            scene = nullptr;
        }
    }
    if (!scene)
    {
        pso_destroy(shaders, scatter_pso);
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
    assert(scene);
    const gpu::GpuHeap heaps[3] = {scene->scene_heap, scene->blob_heap, scene->upload_heap};
    pso_destroy(scene->shaders, scene->scatter_pso);
    scene->~Scene(); // destroys the upload queue; the arena memory itself is not reclaimed
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

    const MeshHandle handle =
        scene->meshes.create(MeshSlot{.blob = blob, .retire_frame = 0u, .removed = false, .retire_scheduled = false});
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
    // Frames in flight may still read the blob and the record slot, and instance changes that drop references
    // to the mesh may still be staged: keep both until those changes are scattered and their frames retire.
    slot->removed          = true;
    slot->retire_scheduled = false;
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
    scene->recorded    = false;

    // Frame (frame_index - frames_in_flight) has completed: its ring block, and everything before it, is free.
    // (max: a skipped frame index leaves an older end in its slot.)
    u64& ring_end   = scene->frame_ring_ends[(u32)(frame_index % scene->desc.frames_in_flight)];
    scene->ring_tail = ring_end > scene->ring_tail ? ring_end : scene->ring_tail;
    ring_end         = scene->ring_head;

    // Removed meshes go once the frame that scattered their last instance changes has completed.
    for (u32 i = 0; i < scene->retiring_meshes.size();)
    {
        const MeshHandle handle = scene->retiring_meshes[i];
        const MeshSlot*  slot   = scene->meshes.get(handle);
        if (slot->retire_scheduled && frame_index >= slot->retire_frame + scene->desc.frames_in_flight)
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

// Copies as many staged changes as fit into a new ring block (static meshes first) and records their scatter.
static void scatter_staged(Scene* scene, gpu::CommandBuffer* commands)
{
    const u64 capacity = scene->ring_capacity;
    if (scene->ring_head == scene->ring_tail)
    {
        // Empty ring: restart at a capacity boundary so the whole ring is one contiguous span.
        scene->ring_head = scene->ring_tail = (scene->ring_head + capacity - 1u) / capacity * capacity;
    }

    const u64 offset      = scene->ring_head % capacity;
    const u64 free        = capacity - (scene->ring_head - scene->ring_tail);
    const u64 end_space   = capacity - offset < free ? capacity - offset : free; // from the head to the ring's end
    const u64 start_space = free - end_space;                                    // after wrapping to the start
    const u64 full        = block_layout(scene->static_mesh_count, scene->transform_count).size;
    const bool wrap       = full > end_space && (full <= start_space || start_space > end_space);
    const u64 budget      = wrap ? start_space : end_space;

    u32 static_meshes = 0u, transforms = 0u;
    if (budget >= sizeof(SceneScatterRoot))
    {
        u64 remaining = budget - sizeof(SceneScatterRoot);
        static_meshes = fit_count(scene->static_mesh_count, remaining, sizeof(GpuStaticMesh));
        remaining -= stream_bytes(static_meshes, sizeof(GpuStaticMesh));
        transforms = fit_count(scene->transform_count, remaining, sizeof(float3x4));
    }
    if (static_meshes < scene->static_mesh_count || transforms < scene->transform_count)
    {
        scene->deferred_frames++;
    }
    if (static_meshes == 0u && transforms == 0u)
    {
        return;
    }

    if (wrap)
    {
        scene->ring_head += capacity - offset;
    }
    const BlockLayout layout = block_layout(static_meshes, transforms);
    byte* const       cpu    = scene->upload_heap.range.cpu + scene->ring_head % capacity;
    byte* const       gpu    = scene->upload_heap.range.gpu + scene->ring_head % capacity;
    scene->ring_head += layout.size;
    scene->frame_ring_ends[(u32)(scene->frame_index % scene->desc.frames_in_flight)] = scene->ring_head;
    const u64 in_use       = scene->ring_head - scene->ring_tail;
    scene->ring_peak_bytes = in_use > scene->ring_peak_bytes ? in_use : scene->ring_peak_bytes;

    // Mapped memory is write-combined: fill it sequentially and never read it back.
    const SceneScatterRoot root = {
        .transform_indices       = (u32*)(gpu + layout.transform_indices),
        .transform_values        = (float3x4*)(gpu + layout.transform_values),
        .transforms              = scene->gpu_transforms,
        .static_mesh_indices     = (u32*)(gpu + layout.static_mesh_indices),
        .static_mesh_values      = (GpuStaticMesh*)(gpu + layout.static_mesh_values),
        .static_meshes           = scene->gpu_static_meshes,
        .transform_count         = transforms,
        .static_mesh_count       = static_meshes,
        .root_alignment_padding0 = {0u, 0u},
    };
    memcpy(cpu, &root, sizeof(root));
    memcpy(cpu + layout.static_mesh_indices, scene->staged_static_mesh_indices.data, sizeof(u32) * static_meshes);
    memcpy(cpu + layout.static_mesh_values, scene->staged_static_mesh_values.data, sizeof(GpuStaticMesh) * static_meshes);
    memcpy(cpu + layout.transform_indices, scene->staged_transform_indices.data, sizeof(u32) * transforms);
    memcpy(cpu + layout.transform_values, scene->staged_transform_values.data, sizeof(float3x4) * transforms);

    consume_staged(scene->static_mesh_pending, scene->staged_static_mesh_indices, scene->staged_static_mesh_values,
                   scene->static_mesh_count, static_meshes);
    consume_staged(scene->transform_pending, scene->staged_transform_indices, scene->staged_transform_values,
                   scene->transform_count, transforms);

    const u32 count = transforms > static_meshes ? transforms : static_meshes;

    // Earlier readers of the scene arrays finish before the scatter overwrites them; later ones see the result.
    gpu::barrier(commands, gpu::Stage::all_commands, gpu::Access::shader_read | gpu::Access::transfer_read,
                 gpu::Stage::compute, gpu::Access::shader_write);
    gpu::bind_pso(commands, pso_get(scene->shaders, scene->scatter_pso));
    gpu::dispatch(commands, (SceneScatterRoot*)gpu,
                  {.x = (count + scene_scatter_group_size - 1u) / scene_scatter_group_size, .y = 1u, .z = 1u});
    gpu::barrier(commands, gpu::Stage::compute, gpu::Access::shader_write, gpu::Stage::all_commands,
                 gpu::Access::shader_read | gpu::Access::transfer_read);
}

void scene_record_updates(Scene* scene, gpu::CommandBuffer* commands)
{
    scene->recorded = true;
    if (scene->uploads_pending)
    {
        scene->upload_completion = scene->uploads.flush();
        scene->uploads_pending   = false;
    }

    if (scene->static_mesh_count > 0u || scene->transform_count > 0u)
    {
        scatter_staged(scene, commands);
    }

    // With no instance changes left staged, every reference to a removed mesh is gone once this frame completes.
    if (scene->static_mesh_count == 0u)
    {
        for (const MeshHandle handle : scene->retiring_meshes)
        {
            MeshSlot* slot = scene->meshes.get(handle);
            if (!slot->retire_scheduled)
            {
                slot->retire_scheduled = true;
                slot->retire_frame     = scene->frame_index;
            }
        }
    }
}

gpu::TimelinePoint scene_upload_completion(const Scene* scene) { return scene->upload_completion; }

SceneUploadStats scene_upload_stats(const Scene* scene)
{
    return {
        .ring_capacity         = scene->ring_capacity,
        .ring_peak_bytes       = scene->ring_peak_bytes,
        .deferred_frames       = scene->deferred_frames,
        .pending_transforms    = scene->transform_count,
        .pending_static_meshes = scene->static_mesh_count,
    };
}

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
