#pragma once

#include "core/Allocators.hpp"
#include "core/Containers.hpp"
#include "gfx/cluster_mesh_shared.h"

enum class GltfImportResult : u32
{
    ok,
    file_error,    // .gltf/.glb or an external .bin is missing or unreadable
    parse_error,   // malformed glTF/GLB (tinygltf errors are printed to stderr)
    unsupported,   // Draco, required EXT_mesh_gpu_instancing, sparse indices, > mesh_max_elements, blob >= 4 GiB
    invalid_data,  // bad accessor type/count, index out of range, non-finite/huge values, bad meshopt stream
    no_geometry,   // no (non-degenerate) triangles reachable from the scene
    out_of_memory, // arena too small (or tinygltf ran out of heap)
};

struct GltfImportSettings
{
    u32   max_cluster_vertices  = cluster_max_vertices;  // 3..cluster_max_vertices
    u32   max_cluster_triangles = cluster_max_triangles; // 1..cluster_max_triangles
    float cone_weight           = 0.25f;                 // 0..1, trades cluster size for cone culling
    bool  recompute_tangents    = false;                 // ignore authored TANGENT, regenerate from TEXCOORD_0
};

/*
 * One GPU-ready blob, 16-byte aligned, at the top of the caller's arena:
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

    u32 unique_vertex_count;   // stats: vertices before per-cluster duplication
    u32 source_triangle_count; // stats: triangles before degenerate/duplicate filtering
};

/*
 * Imports every mesh instance of the default scene, flattened into one
 * object-space mesh (node transforms and default morph weights baked in,
 * skinned meshes in bind pose), and builds GPU-ready clusters.
 * The importer's scratch memory comes from the arena too: on `ok` the arena
 * holds exactly the blob above its previous offset; on failure the offset is
 * restored and out_mesh is cleared. tinygltf and meshoptimizer still use the
 * CRT heap internally for their own temporaries.
 */
[[nodiscard]] GltfImportResult import_gltf(ArenaAllocator<>& arena, const char* filepath,
                                           ClusterMesh& out_mesh, const GltfImportSettings& settings = {});

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
