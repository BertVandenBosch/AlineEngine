#pragma once

#include "core/Allocators.hpp"
#include "core/Pool.hpp"
#include "gfx/GltfImport.hpp"
#include "gfx/scene_shared.h"

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

/*
 * Flat, data-oriented scene (HypeHype style): every object kind is a flat
 * array indexed by a 32-bit generational handle, and the handle index is the
 * same index the GPU uses. The persistent arrays live in GPU memory only:
 * transforms are write-only from the CPU, mesh data is uploaded once and not
 * kept. Each frame's changes go into a mapped upload slice (one entry per
 * changed slot) and are scattered into place by one compute dispatch.
 *
 * Lifetime rules: destroy static meshes before the transforms and meshes they
 * reference. Handles are generation-checked on the CPU; the GPU sees indices.
 */

using TransformHandle  = PoolHandle<u32, 20u, 12u, struct TransformTag>;
using StaticMeshHandle = PoolHandle<u32, 20u, 12u, struct StaticMeshTag>;
using MeshHandle       = PoolHandle<u32, 16u, 16u, struct MeshTag>;

struct SceneDesc
{
    u32 max_transforms    = 1u << 17;
    u32 max_static_meshes = 1u << 17;
    u32 max_meshes        = 4096u;
    u64 mesh_blob_bytes   = MB(256);
    u32 frames_in_flight  = 2u;
};

struct Scene; // opaque

// scatter_shader: compiled scene_scatter.slang (SPIR-V on Vulkan, metallib on Metal).
// The scene's CPU arrays are allocated from `arena`, which must outlive it. Null on failure.
[[nodiscard]] Scene* scene_create(ArenaAllocator<>& arena, gpu::Device* device, const SceneDesc& desc,
                                  gpu::ByteSpan scatter_shader);
void scene_destroy(Scene* scene); // after gpu::wait_idle

// Uploads the mesh blob and its GpuMesh record; the ClusterMesh may be freed right after. Empty handle when full.
[[nodiscard]] MeshHandle scene_add_mesh(Scene* scene, const ClusterMesh& mesh);
// The GPU data is released once the frames that may still use it have completed.
void scene_remove_mesh(Scene* scene, MeshHandle mesh);

[[nodiscard]] TransformHandle scene_create_transform(Scene* scene, const float3x4& world);
void scene_set_transform(Scene* scene, TransformHandle transform, const float3x4& world);
void scene_destroy_transform(Scene* scene, TransformHandle transform);

[[nodiscard]] StaticMeshHandle scene_create_static_mesh(Scene* scene, MeshHandle mesh, TransformHandle transform,
                                                        u32 flags = static_mesh_visible);
void scene_set_static_mesh_flags(Scene* scene, StaticMeshHandle static_mesh, u32 flags);
void scene_destroy_static_mesh(Scene* scene, StaticMeshHandle static_mesh);

// Frame protocol: the caller has waited for frame (frame_index - frames_in_flight) before calling begin.
void scene_begin_frame(Scene* scene, u64 frame_index);
// Records the scatter of this frame's changes (and flushes pending mesh uploads). Call outside a render pass;
// it binds the scatter compute PSO.
void scene_record_updates(Scene* scene, gpu::CommandBuffer* commands);
// Completion of the latest mesh upload; the frame's submit must wait on it. Null semaphore when none.
[[nodiscard]] gpu::TimelinePoint scene_upload_completion(const Scene* scene);

[[nodiscard]] GpuScene scene_gpu_view(const Scene* scene);
