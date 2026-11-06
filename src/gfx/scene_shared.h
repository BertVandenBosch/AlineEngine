#pragma once

#include "gfx/cluster_mesh_shared.h"

/*
 * Persistent GPU scene, shared by C++ and Slang.
 *
 * Flat arrays indexed by the handle index of the CPU-side pools: world
 * transforms, static mesh instances and mesh records. The CPU never keeps a
 * copy of transforms or mesh data; changes are written into a per-frame
 * upload slice and scattered into these arrays on the GPU (scene_scatter.slang).
 */

static const uint32 static_mesh_visible      = 1u; // flags; 0 = empty slot, skipped by culling
static const uint32 scene_scatter_group_size = 64;

// 16 bytes: one aligned load per instance.
struct GpuStaticMesh
{
    uint32 mesh;      // index into GpuScene::meshes
    uint32 transform; // index into GpuScene::transforms (may be shared between instances)
    uint32 flags;
    uint32 reserved;  // 0 (future: 4 x 8-bit material ids)
};

// 64-byte root of the scatter dispatch, allocated in the frame's upload slice.
struct SceneScatterRoot
{
    uint32*        transform_indices;
    float3x4*      transform_values;
    float3x4*      transforms;
    uint32*        static_mesh_indices;
    GpuStaticMesh* static_mesh_values;
    GpuStaticMesh* static_meshes;
    uint32         transform_count;
    uint32         static_mesh_count;
    uint32         root_alignment_padding0[2]; // roots are a multiple of 16 bytes
};

// The persistent arrays, for passes that read the scene.
struct GpuScene
{
    GpuMesh*       meshes;
    float3x4*      transforms;
    GpuStaticMesh* static_meshes;
    uint32         static_mesh_count; // high-water mark: slots [0, count) may be live
    uint32         reserved;
};

#if !defined(__SLANG__)
static_assert(sizeof(GpuStaticMesh) == 16 && sizeof(float3x4) == 48);
static_assert(sizeof(SceneScatterRoot) == 64);
static_assert(sizeof(GpuScene) == 32);
#endif
