#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

/*
 * GPU layout of a clustered (meshlet) mesh, shared by C++ and Slang.
 *
 * Every cluster owns a contiguous vertex range, so triangles index
 * vertex_offset + local directly (no meshlet_vertices indirection). Positions
 * are offsets from the cluster's aabb_min on one mesh-wide power-of-two grid,
 * which decodes exactly in fp32: shared border vertices land on identical
 * positions, so the mesh stays watertight. Every field is read as a 32-bit
 * word (the Vulkan backend has no 8-bit storage and no shaderInt64).
 *
 * Section order in a mesh blob: clusters | triangles | positions | attributes.
 */

// Mesh shaders are compiled for these output sizes; import settings may be lower, never higher.
static const uint32 cluster_max_vertices  = 64;
static const uint32 cluster_max_triangles = 96;
static const uint32 cluster_thread_count  = 32;
static const uint32 mesh_position_bits    = 21;
static const uint32 mesh_max_elements     = 1u << 24;    // vertices, and triangles, per mesh (24-bit cluster offsets)
static const uint32 mesh_no_cone          = 0x7F000000u; // cutoff 127, axis 0: never cone-culls

// 8 bytes. Grid-step offsets from the cluster anchor: x bits 0-20, y 21-41, z 42-62, bit 63 = 0.
// position = cluster.aabb_min + float3(x, y, z) * mesh.position_step
struct MeshPosition
{
    uint32 lo; // x | y << 21
    uint32 hi; // y >> 11 | z << 10
};

// 32 bytes: two aligned 16-byte loads.
// aabb_max = aabb_min + decoded extent * mesh.position_step (exact, same decode as a MeshPosition).
// Bounding sphere: centre (aabb_min + aabb_max) * 0.5, radius 0.5 * length(aabb_max - aabb_min).
struct MeshCluster
{
    float3       aabb_min;  // exact grid point, also the anchor of this cluster's MeshPositions
    uint32       cone;      // snorm8 axis.x | axis.y << 8 | axis.z << 16 | cutoff << 24 (meshopt *_s8, decode /127)
    MeshPosition extent;    // aabb_max - aabb_min in grid steps
    uint32       vertices;  // vertex_offset | (vertex_count - 1) << 24; offset into positions[] and attributes[]
    uint32       triangles; // triangle_offset | (triangle_count - 1) << 24; offset into triangles[]
};

// 12 bytes. Octahedral normal/tangent stored as biased snorm (value + bias), so 0 is exact.
struct MeshAttribute
{
    uint32 normal;  // u bits 0-15, v bits 16-31, bias 32767
    uint32 tangent; // u bits 0-14, v bits 15-29, bias 16383; bit 30 set = bitangent sign -1
    uint32 uv;      // unorm16 u | v << 16; uv = mesh.uv_min + float2(u, v) * mesh.uv_scale
};

// Triangles are a plain uint32 array: a | b << 8 | c << 16 (cluster-local indices), top byte 0.
// bitangent = cross(normal, tangent) * sign (glTF convention).

// 96 bytes. Per-mesh record in GPU memory, read through a pointer.
struct GpuMesh
{
    MeshCluster*   clusters;
    uint32*        triangles;
    MeshPosition*  positions;
    MeshAttribute* attributes;
    float3         aabb_min;
    float          position_step;
    float3         aabb_max;
    uint32         cluster_count;
    float4         sphere; // xyz centre, w radius
    float2         uv_min;
    float2         uv_scale;
};

#if !defined(__SLANG__)
#include <stddef.h>
static_assert(sizeof(MeshCluster) == 32 && offsetof(MeshCluster, cone) == 12 &&
              offsetof(MeshCluster, extent) == 16 && offsetof(MeshCluster, vertices) == 24);
static_assert(sizeof(MeshPosition) == 8 && sizeof(MeshAttribute) == 12);
static_assert(cluster_max_vertices <= 256 && cluster_max_triangles <= 256); // counts - 1 fit in 8 bits
static_assert(sizeof(GpuMesh) == 96 && offsetof(GpuMesh, aabb_min) == 32 &&
              offsetof(GpuMesh, sphere) == 64 && offsetof(GpuMesh, uv_min) == 80);
#endif
