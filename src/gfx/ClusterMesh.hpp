#pragma once

#include "core/Containers.hpp"
#include "gfx/cluster_mesh_shared.h"

/*
 * CPU-side storage of a clustered mesh: one GPU-ready blob, 16-byte aligned:
 * [clusters | triangles | positions | attributes], each section 16-byte
 * aligned, padding zeroed. Every View aliases the blob, so uploading is a
 * single memcpy of `blob`.
 */
struct ClusterMesh
{
    View<u8>            blob;
    View<MeshCluster>   clusters;
    View<u32>           triangles;
    View<MeshPosition>  positions;
    View<MeshAttribute> attributes;

    float3 aabb_min;          // object space, union of the cluster AABBs
    float3 aabb_max;
    float4 sphere;            // xyz centre, w radius
    float  position_step;     // 2^position_exponent
    i32    position_exponent;
    float2 uv_min;
    float2 uv_scale;
};

// gpu_blob: GPU address of the uploaded copy of mesh.blob (16-byte aligned).
inline GpuMesh make_gpu_mesh(const ClusterMesh& mesh, byte* gpu_blob)
{
    auto rebase = [&](const void* cpu) { return gpu_blob + ((const u8*)cpu - mesh.blob.data); };

    return {
        .clusters      = (MeshCluster*)rebase(mesh.clusters.data),
        .triangles     = (uint32*)rebase(mesh.triangles.data),
        .positions     = (MeshPosition*)rebase(mesh.positions.data),
        .attributes    = (MeshAttribute*)rebase(mesh.attributes.data),
        .aabb_min      = mesh.aabb_min,
        .position_step = mesh.position_step,
        .aabb_max      = mesh.aabb_max,
        .cluster_count = mesh.clusters.num_elements,
        .sphere        = mesh.sphere,
        .uv_min        = mesh.uv_min,
        .uv_scale      = mesh.uv_scale,
    };
}
