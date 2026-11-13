#pragma once

#include "gfx/scene_shared.h"

/*
 * Root of the basic mesh draw (shaders/mesh.slang): one static mesh instance, LOD0 only,
 * drawn as one instance per cluster.
 */

// 96 bytes.
struct MeshDrawRoot
{
    float4x4       view_projection;
    GpuMesh*       meshes;
    float3x4*      transforms;
    GpuStaticMesh* static_meshes;
    uint32         static_mesh; // index into static_meshes
    uint32         root_alignment_padding0[1];
};

#if !defined(__SLANG__)
static_assert(sizeof(MeshDrawRoot) == 96);
#endif
