#include <NoGraphicsAPI/shader.slang>
#include "gfx/mesh_shared.h"

// Basic draw of one static mesh instance at LOD0: one draw instance per cluster, cluster_max_triangles * 3
// vertices each. Vertices of triangles past the cluster's count collapse to one point and rasterize nothing.

GPU_ROOT(MeshDrawRoot, root);

static const float3 light_direction = float3(0.408248f, 0.816497f, 0.408248f); // unit, towards the light
static const float3 albedo          = float3(0.8f, 0.8f, 0.8f);

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
};

// Grid steps from the cluster anchor: x bits 0-20, y 21-41, z 42-62.
float3 decode_position(MeshPosition position)
{
    return float3(uint3(position.lo & 0x1FFFFFu, (position.lo >> 21) | ((position.hi & 0x3FFu) << 11), (position.hi >> 10) & 0x1FFFFFu));
}

// Octahedral normal, 16-bit biased snorm per axis (inverse of the importer's oct_encode).
float3 decode_normal(uint packed)
{
    float2 uv = float2(int2(uint2(packed & 0xFFFFu, packed >> 16)) - 32767) / 32767.0f;
    float3 normal = float3(uv, 1.0f - abs(uv.x) - abs(uv.y));
    if (normal.z < 0.0f)
    {
        normal.xy = (1.0f - abs(uv.yx)) * float2(uv.x >= 0.0f ? 1.0f : -1.0f, uv.y >= 0.0f ? 1.0f : -1.0f);
    }
    return normalize(normal);
}

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID, uint cluster_index : SV_InstanceID)
{
    GpuStaticMesh instance = root.static_meshes[root.static_mesh];
    GpuMesh* mesh = root.meshes + instance.mesh;
    MeshCluster cluster = mesh.clusters[cluster_index];

    VertexOutput output;
    uint triangle = vertex_id / 3u;
    if (triangle > (cluster.triangles >> 24))
    {
        output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);
        output.normal = float3(0.0f, 0.0f, 0.0f);
        return output;
    }

    uint corners = mesh.triangles[(cluster.triangles & 0xFFFFFFu) + triangle];
    uint vertex = (cluster.vertices & 0xFFFFFFu) + ((corners >> ((vertex_id % 3u) * 8u)) & 0xFFu);
    float3 object_position = cluster.aabb_min + decode_position(mesh.positions[vertex]) * mesh.position_step;
    float3x4 world = root.transforms[instance.transform];

    output.position = mul(root.view_projection, float4(mul(world, float4(object_position, 1.0f)), 1.0f));
    output.normal = mul(world, float4(decode_normal(mesh.attributes[vertex].normal), 0.0f));
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    return float4(albedo * saturate(dot(normalize(input.normal), light_direction)), 1.0f);
}
