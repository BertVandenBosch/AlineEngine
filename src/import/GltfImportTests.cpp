#define _CRT_SECURE_NO_WARNINGS // plain fopen for the crafted test files

#include "import/GltfImport.hpp"
#include "gfx/MeshLod.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef ALINE_ROOT_DIR
#error "ALINE_ROOT_DIR must name the repository root (defined by premake5.lua)"
#endif

/*
 * import_gltf tests over the sample models shipped in the tinygltf and
 * meshoptimizer submodules. They rely on assert, so they only run in Debug.
 */

#if defined(NDEBUG)

void run_gltf_import_tests() { printf("gltf import tests skipped (asserts disabled)\n"); }

#else

static const char* asset(const char* relative)
{
    static char path[1024];
    snprintf(path, sizeof(path), "%s/%s", ALINE_ROOT_DIR, relative);
    return path;
}

static u32 vertex_offset_of(const MeshCluster& cluster) { return cluster.vertices & 0xFFFFFFu; }
static u32 vertex_count_of(const MeshCluster& cluster) { return (cluster.vertices >> 24) + 1u; }
static u32 triangle_offset_of(const MeshCluster& cluster) { return cluster.triangles & 0xFFFFFFu; }
static u32 triangle_count_of(const MeshCluster& cluster) { return (cluster.triangles >> 24) + 1u; }

// Same arithmetic as the shader: exact on the shared grid.
static void decode_position(const ClusterMesh& mesh, const MeshCluster& cluster, const MeshPosition& p, double out[3])
{
    const u32 x = p.lo & 0x1FFFFFu;
    const u32 y = (p.lo >> 21) | ((p.hi & 0x3FFu) << 11);
    const u32 z = (p.hi >> 10) & 0x1FFFFFu;
    out[0]      = cluster.aabb_min.x + (float)x * mesh.position_step;
    out[1]      = cluster.aabb_min.y + (float)y * mesh.position_step;
    out[2]      = cluster.aabb_min.z + (float)z * mesh.position_step;
}

// Cluster box and sphere exactly as a shader derives them.
struct ClusterBounds
{
    float aabb_max[3];
    float centre[3];
    float radius;
};

static ClusterBounds cluster_bounds(const ClusterMesh& mesh, const MeshCluster& cluster)
{
    double        aabb_max[3];
    ClusterBounds bounds;
    decode_position(mesh, cluster, cluster.extent, aabb_max);
    float size_squared = 0.0f;
    for (u32 k = 0; k < 3; k++)
    {
        bounds.aabb_max[k] = (float)aabb_max[k];
        bounds.centre[k]   = (cluster.aabb_min[k] + bounds.aabb_max[k]) * 0.5f;
        const float size   = bounds.aabb_max[k] - cluster.aabb_min[k];
        size_squared += size * size;
    }
    bounds.radius = 0.5f * sqrtf(size_squared);
    return bounds;
}

static void oct_decode(u32 word, u32 bits, double out[3])
{
    const u32    mask = (1u << bits) - 1u;
    const double bias = (double)(mask >> 1);
    double       x    = ((double)(word & mask) - bias) / bias;
    double       y    = ((double)((word >> bits) & mask) - bias) / bias;
    const double z    = 1.0 - fabs(x) - fabs(y);
    const double fold = z < 0.0 ? -z : 0.0;
    x += x >= 0.0 ? -fold : fold;
    y += y >= 0.0 ? -fold : fold;
    const double length = sqrt(x * x + y * y + z * z);
    out[0] = x / length, out[1] = y / length, out[2] = z / length;
}

static double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross3(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// Decoded corners of triangle t of a cluster.
static void triangle_positions(const ClusterMesh& mesh, const MeshCluster& cluster, u32 t, double p[3][3])
{
    const u32 packed = mesh.triangles[triangle_offset_of(cluster) + t];
    for (u32 i = 0; i < 3; i++)
    {
        decode_position(mesh, cluster, mesh.positions[vertex_offset_of(cluster) + ((packed >> (8 * i)) & 0xFFu)], p[i]);
    }
}

// Cluster range [first, end) of a LOD.
static u32 lod_end(const ClusterMesh& mesh, u32 lod)
{
    return lod + 1u < mesh.lod_count ? mesh.lods[lod + 1u].first_cluster : mesh.clusters.num_elements;
}

static u32 lod_triangle_count(const ClusterMesh& mesh, u32 lod)
{
    u32 count = 0;
    for (u32 k = mesh.lods[lod].first_cluster; k < lod_end(mesh, lod); k++)
    {
        count += triangle_count_of(mesh.clusters[k]);
    }
    return count;
}

static bool all_zero(const u8* begin, const u8* end)
{
    for (const u8* p = begin; p < end; p++)
    {
        if (*p != 0u)
        {
            return false;
        }
    }
    return true;
}

// Format invariants that must hold for every successful import.
static void validate_cluster_mesh(const ClusterMesh& mesh, const ArenaAllocator<>& arena, size_t mark,
                                  const GltfImportSettings& settings)
{
    // Layout: one 16-byte aligned blob at the mark, sections in order, padding zeroed.
    const u8* blob = mesh.blob.data;
    const u8* end  = blob + mesh.blob.num_elements;
    assert(blob == (const u8*)MemoryUtils::align_forward((uintptr_t)(arena.buffer + mark), 16));
    assert(arena.buffer_offset == (size_t)(end - arena.buffer) && mesh.blob.num_elements % 16u == 0u);

    const u8* sections[4]     = {(const u8*)mesh.clusters.data, (const u8*)mesh.triangles.data, (const u8*)mesh.positions.data,
                                 (const u8*)mesh.attributes.data};
    const u8* section_ends[4] = {sections[0] + sizeof(MeshCluster) * mesh.clusters.num_elements,
                                 sections[1] + sizeof(u32) * mesh.triangles.num_elements,
                                 sections[2] + sizeof(MeshPosition) * mesh.positions.num_elements,
                                 sections[3] + sizeof(MeshAttribute) * mesh.attributes.num_elements};
    assert(sections[0] == blob);
    for (u32 s = 0; s < 4; s++)
    {
        assert(((uintptr_t)sections[s] & 15u) == 0u);
        const u8* next = s < 3 ? sections[s + 1] : end;
        assert(section_ends[s] <= next && all_zero(section_ends[s], next));
    }
    assert(mesh.positions.num_elements == mesh.attributes.num_elements);

    const double centre[3] = {mesh.sphere.x, mesh.sphere.y, mesh.sphere.z};
    u32          vertex_cursor = 0, triangle_cursor = 0;
    for (const MeshCluster& cluster : mesh.clusters)
    {
        const u32 vcount = vertex_count_of(cluster);
        const u32 tcount = triangle_count_of(cluster);

        // Ranges: dense prefix sums within the build limits.
        assert(vcount <= settings.max_cluster_vertices && tcount <= settings.max_cluster_triangles);
        assert(vertex_offset_of(cluster) == vertex_cursor && triangle_offset_of(cluster) == triangle_cursor);
        assert((cluster.extent.hi >> 31) == 0u);
        vertex_cursor += vcount;
        triangle_cursor += tcount;

        // Triangles: valid, distinct local indices.
        for (u32 t = 0; t < tcount; t++)
        {
            const u32 packed = mesh.triangles[triangle_offset_of(cluster) + t];
            const u32 a = packed & 0xFFu, b = (packed >> 8) & 0xFFu, c = (packed >> 16) & 0xFFu;
            assert((packed >> 24) == 0u && a < vcount && b < vcount && c < vcount && a != b && b != c && a != c);
        }

        // Positions: inside a tight cluster AABB on the grid, inside the cluster and mesh spheres.
        // The derived radius is computed in float like a shader would, so allow it an ulp-sized slack.
        const ClusterBounds bounds            = cluster_bounds(mesh, cluster);
        const double        cluster_centre[3] = {bounds.centre[0], bounds.centre[1], bounds.centre[2]};
        const double        lo[3]             = {cluster.aabb_min.x, cluster.aabb_min.y, cluster.aabb_min.z};
        const double        hi[3]             = {bounds.aabb_max[0], bounds.aabb_max[1], bounds.aabb_max[2]};
        bool                reached_lo[3] = {}, reached_hi[3] = {};
        for (u32 i = 0; i < vcount; i++)
        {
            const MeshPosition& p = mesh.positions[vertex_offset_of(cluster) + i];
            assert((p.hi >> 31) == 0u);
            double position[3];
            decode_position(mesh, cluster, p, position);
            const double d[3] = {position[0] - cluster_centre[0], position[1] - cluster_centre[1], position[2] - cluster_centre[2]};
            assert(sqrt(dot3(d, d)) <= bounds.radius * (1.0 + 1e-6));
            for (u32 k = 0; k < 3; k++)
            {
                assert(position[k] >= lo[k] && position[k] <= hi[k]);
                reached_lo[k] |= position[k] == lo[k];
                reached_hi[k] |= position[k] == hi[k];
            }

            const MeshAttribute& attribute = mesh.attributes[vertex_offset_of(cluster) + i];
            double               normal[3], tangent[3];
            oct_decode(attribute.normal, 16, normal);
            oct_decode(attribute.tangent & 0x3FFFFFFFu, 15, tangent);
            assert((attribute.tangent >> 31) == 0u && fabs(dot3(normal, tangent)) < 1e-3);
        }
        for (u32 k = 0; k < 3; k++)
        {
            assert(reached_lo[k] && reached_hi[k]);
            assert(lo[k] >= mesh.aabb_min[k] && hi[k] <= mesh.aabb_max[k]);
            assert(fmod(lo[k], mesh.position_step) == 0.0);
        }
        const double to_mesh[3] = {cluster_centre[0] - centre[0], cluster_centre[1] - centre[1], cluster_centre[2] - centre[2]};
        assert(sqrt(dot3(to_mesh, to_mesh)) + bounds.radius <= mesh.sphere.w);
    }
    assert(vertex_cursor == mesh.positions.num_elements && triangle_cursor == mesh.triangles.num_elements);

    // LODs: LOD0 starts the cluster array exactly, then non-empty ranges with fewer triangles and no smaller error.
    assert(mesh.lod_count >= 1u && mesh.lod_count <= settings.max_lods);
    assert(mesh.lods[0].first_cluster == 0u && mesh.lods[0].error == 0.0f);
    for (u32 lod = 1; lod < mesh.lod_count; lod++)
    {
        assert(mesh.lods[lod].first_cluster > mesh.lods[lod - 1].first_cluster);
        assert(mesh.lods[lod].first_cluster < mesh.clusters.num_elements);
        assert(mesh.lods[lod].error >= mesh.lods[lod - 1].error && isfinite(mesh.lods[lod].error));
        assert(lod_triangle_count(mesh, lod) < lod_triangle_count(mesh, lod - 1));
    }
    for (u32 lod = mesh.lod_count; lod < mesh_max_lods; lod++)
    {
        assert(mesh.lods[lod].first_cluster == 0u && mesh.lods[lod].error == 0.0f);
    }
}

// Every cluster the shader's s8 cone test culls must be entirely back-facing.
static u32 check_cone_culling(const ClusterMesh& mesh)
{
    u32  state  = 12345u;
    auto random = [&]()
    {
        state = state * 1664525u + 1013904223u;
        return (double)(state >> 8) / 16777216.0;
    };

    u32 culled = 0;
    for (u32 camera_index = 0; camera_index < 2000u; camera_index++)
    {
        double direction[3];
        double length = 0.0;
        do
        {
            for (double& c : direction)
            {
                c = random() * 2.0 - 1.0;
            }
            length = sqrt(dot3(direction, direction));
        } while (length > 1.0 || length < 1e-3);
        const double distance  = mesh.sphere.w * (0.5 + 9.5 * random());
        const float  camera[3] = {(float)(mesh.sphere.x + direction[0] / length * distance),
                                  (float)(mesh.sphere.y + direction[1] / length * distance),
                                  (float)(mesh.sphere.z + direction[2] / length * distance)};

        for (const MeshCluster& cluster : mesh.clusters)
        {
            const ClusterBounds bounds   = cluster_bounds(mesh, cluster);
            const i32           axis[4]  = {(int8)(cluster.cone & 0xFFu), (int8)((cluster.cone >> 8) & 0xFFu),
                                            (int8)((cluster.cone >> 16) & 0xFFu), (int8)(cluster.cone >> 24)};
            const float         d[3]     = {bounds.centre[0] - camera[0], bounds.centre[1] - camera[1], bounds.centre[2] - camera[2]};
            const float         d_length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2] < axis[3] * d_length + 127.0f * bounds.radius)
            {
                continue;
            }

            culled++;
            for (u32 t = 0; t < triangle_count_of(cluster); t++)
            {
                double p[3][3];
                triangle_positions(mesh, cluster, t, p);
                const double e1[3]   = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
                const double e2[3]   = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
                const double view[3] = {p[0][0] - camera[0], p[0][1] - camera[1], p[0][2] - camera[2]};
                double       normal[3];
                cross3(e1, e2, normal);
                assert(dot3(normal, view) >= -1e-6 * sqrt(dot3(normal, normal)) * sqrt(dot3(view, view)));
            }
        }
    }
    return culled;
}

// Closed meshes: every directed edge has exactly one opposite edge, and the volume is positive (outward CCW).
static void check_watertight(const ClusterMesh& mesh)
{
    struct Edge
    {
        double from[3], to[3];
    };
    const u32 edge_count = mesh.triangles.num_elements * 3;
    Edge      edges[256];
    assert(edge_count <= 256u);

    double volume = 0.0;
    u32    e      = 0;
    for (const MeshCluster& cluster : mesh.clusters)
    {
        for (u32 t = 0; t < triangle_count_of(cluster); t++)
        {
            double p[3][3], bc[3];
            triangle_positions(mesh, cluster, t, p);
            cross3(p[1], p[2], bc);
            volume += dot3(p[0], bc) / 6.0;
            for (u32 i = 0; i < 3; i++)
            {
                memcpy(edges[e].from, p[i], sizeof(p[i]));
                memcpy(edges[e].to, p[(i + 1) % 3], sizeof(p[i]));
                e++;
            }
        }
    }
    for (u32 i = 0; i < edge_count; i++)
    {
        u32 opposite = 0;
        for (u32 j = 0; j < edge_count; j++)
        {
            opposite += memcmp(edges[i].from, edges[j].to, sizeof(edges[i].from)) == 0 &&
                        memcmp(edges[i].to, edges[j].from, sizeof(edges[i].to)) == 0;
        }
        assert(opposite == 1u);
    }
    assert(volume > 0.0);
}

// select_mesh_lod on a unit-radius mesh at the origin with LOD errors {0, 0.01, 0.1} and a 1 px threshold.
static void run_lod_selection_tests()
{
    GpuMesh mesh   = {};
    mesh.sphere    = {0.0f, 0.0f, 0.0f, 1.0f};
    mesh.lod_count = 3u;
    mesh.lods[1]   = {.first_cluster = 1u, .error = 0.01f};
    mesh.lods[2]   = {.first_cluster = 2u, .error = 0.1f};

    float3x4 identity = {};
    for (u32 r = 0; r < 3; r++)
    {
        identity.rows[r][r] = 1.0f;
    }
    float3x4 scaled = identity;
    for (u32 r = 0; r < 3; r++)
    {
        scaled.rows[r][r] = 2.0f;
    }

    // Projected error (px) = error * scale * projection_scale / (camera distance - radius * scale).
    const auto at = [](float distance) { return LodView{.camera_position = {0.0f, 0.0f, distance}, .projection_scale = 1000.0f, .threshold_px = 1.0f}; };
    assert(select_mesh_lod(mesh, identity, at(0.5f)) == 0u);   // inside the sphere
    assert(select_mesh_lod(mesh, identity, at(5.0f)) == 0u);   // LOD1 would be 2.5 px
    assert(select_mesh_lod(mesh, identity, at(20.0f)) == 1u);  // LOD1 0.53 px, LOD2 5.3 px
    assert(select_mesh_lod(mesh, identity, at(200.0f)) == 2u); // LOD2 0.5 px
    assert(select_mesh_lod(mesh, scaled, at(20.0f)) == 0u);    // 2x scale: LOD1 1.1 px
    assert(select_mesh_lod(mesh, scaled, at(200.0f)) == 1u);   // LOD2 1.01 px, LOD1 0.1 px

    // Translation moves the sphere with the instance.
    float3x4 moved      = identity;
    moved.rows[2][3]    = 180.0f; // sphere now 20 units from a camera at z = 200
    assert(select_mesh_lod(mesh, moved, at(200.0f)) == 1u);

    // A single-LOD mesh always gives 0.
    mesh.lod_count = 1u;
    assert(select_mesh_lod(mesh, identity, at(1e6f)) == 0u);
}

static void print_stats(const char* name, const ClusterMesh& mesh)
{
    const double triangles = mesh.triangles.num_elements;
    const double depth_only = (double)((const u8*)mesh.attributes.data - mesh.blob.data);
    printf("  %-28s %5u clusters %6u tris %6u verts (unique %u, d=%.2f)  %.1f B/tri, depth %.1f B/tri, step 2^%d\n", name,
           mesh.clusters.num_elements, mesh.triangles.num_elements, mesh.positions.num_elements, mesh.unique_vertex_count,
           (double)mesh.positions.num_elements / mesh.unique_vertex_count, mesh.blob.num_elements / triangles,
           depth_only / triangles, mesh.position_exponent);
    if (mesh.lod_count > 1u)
    {
        printf("  %-28s", "");
        for (u32 lod = 0; lod < mesh.lod_count; lod++)
        {
            printf(" LOD%u %u tris (err %.2g)", lod, lod_triangle_count(mesh, lod), (double)mesh.lods[lod].error);
        }
        printf("\n");
    }
}

// Imports, validates and checks that a second import is byte-identical.
static void import_ok(ArenaAllocator<>& arena, const char* relative, ClusterMesh& mesh, const GltfImportSettings& settings = {})
{
    arena.free_all();
    const size_t mark = arena.buffer_offset;
    if (import_gltf(arena, asset(relative), mesh, settings) != GltfImportResult::ok)
    {
        fprintf(stderr, "import failed: %s\n", relative);
        assert(false);
        return;
    }
    validate_cluster_mesh(mesh, arena, mark, settings);
    check_cone_culling(mesh);

    ClusterMesh again = {};
    assert(import_gltf(arena, asset(relative), again, settings) == GltfImportResult::ok);
    assert(again.blob.num_elements == mesh.blob.num_elements &&
           memcmp(again.blob.data, mesh.blob.data, mesh.blob.num_elements) == 0);
    assert(memcmp(&again.sphere, &mesh.sphere, sizeof(mesh.sphere)) == 0);

    print_stats(strrchr(relative, '/') + 1, mesh);
}

static void base64_encode(const void* data, u32 size, char* out)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const u8*         in      = (const u8*)data;
    u32               o       = 0;
    for (u32 i = 0; i < size; i += 3)
    {
        const u32 n = (u32)in[i] << 16 | (i + 1 < size ? (u32)in[i + 1] << 8 : 0u) | (i + 2 < size ? (u32)in[i + 2] : 0u);
        out[o++]    = table[(n >> 18) & 63u];
        out[o++]    = table[(n >> 12) & 63u];
        out[o++]    = i + 1 < size ? table[(n >> 6) & 63u] : '=';
        out[o++]    = i + 2 < size ? table[n & 63u] : '=';
    }
    out[o] = '\0';
}

// Writes `json` to build/ and imports it.
static GltfImportResult import_json(ArenaAllocator<>& arena, const char* json, ClusterMesh& mesh)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/build/gltf_import_test.gltf", ALINE_ROOT_DIR);
    FILE* file = fopen(path, "wb");
    assert(file);
    fputs(json, file);
    fclose(file);
    const GltfImportResult result = import_gltf(arena, path, mesh);
    remove(path);
    return result;
}

// One triangle (0,0,0) (1,0,0) (0,1,0) as accessor 0, a +2 z morph delta as accessor 1, UVs in [0.25, 0.75] as
// accessor 2; `scene` supplies meshes, nodes, skins and scenes.
static GltfImportResult import_triangle(ArenaAllocator<>& arena, const char* scene, ClusterMesh& mesh)
{
    static const float data[24] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 2, 0, 0, 2, 0.25f, 0.25f, 0.75f, 0.25f, 0.25f, 0.75f};
    char               encoded[160];
    base64_encode(data, sizeof(data), encoded);

    static char json[8192];
    snprintf(json, sizeof(json),
             "{\"asset\":{\"version\":\"2.0\"},"
             "\"buffers\":[{\"byteLength\":96,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
             "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24}],"
             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,2],\"max\":[0,0,2]},"
             "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],%s}",
             encoded, scene);
    return import_json(arena, json, mesh);
}

// Cases from the code review: malicious sizes, and valid glTF features the sample models do not cover.
static void run_crafted_tests()
{
    ArenaAllocator arena(MB(4));
    ClusterMesh    mesh = {};

    // A meshopt view on a data-less buffer declaring ~2^64 bytes, with the arena already 4 KB in: the size
    // used to wrap the arena's capacity check and memset far out of bounds.
    {
        const MemoryHandle padding = arena.allocate(KB(4), {});
        const size_t       mark    = arena.buffer_offset;
        assert(padding.is_valid());
        const GltfImportResult result = import_json(
            arena,
            "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"EXT_meshopt_compression\"],"
            "\"buffers\":[{\"byteLength\":18446744073709547520}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteLength\":18446744073709547520,\"extensions\":{\"EXT_meshopt_compression\":"
            "{\"buffer\":0,\"byteLength\":4,\"byteStride\":4,\"count\":1,\"mode\":\"ATTRIBUTES\"}}}]}",
            mesh);
        assert(result == GltfImportResult::out_of_memory && arena.buffer_offset == mark);
        arena.free_all();
    }

    // Index counts near 2^64 (accessors without a bufferView) used to wrap the corner total.
    {
        static const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        char               encoded[64];
        base64_encode(positions, sizeof(positions), encoded);
        char json[2048];
        snprintf(json, sizeof(json),
                 "{\"asset\":{\"version\":\"2.0\"},"
                 "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
                 "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                 "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
                 "{\"componentType\":5125,\"count\":18446744073709547520,\"type\":\"SCALAR\"},"
                 "{\"componentType\":5125,\"count\":2052,\"type\":\"SCALAR\"}],"
                 "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1},"
                 "{\"attributes\":{\"POSITION\":0},\"indices\":2}]}],"
                 "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}",
                 encoded);
        assert(import_json(arena, json, mesh) == GltfImportResult::unsupported && arena.buffer_offset == 0u);
    }

    // Default morph weights are part of the rest shape: z = 0 + 0.5 * 2.
    assert(import_triangle(arena,
                           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"targets\":[{\"POSITION\":1}]}],"
                           "\"weights\":[0.5]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]",
                           mesh) == GltfImportResult::ok);
    assert(mesh.aabb_min.z == 1.0f && mesh.aabb_max.z == 1.0f);
    arena.free_all();

    // node.weights override mesh.weights.
    assert(import_triangle(arena,
                           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"targets\":[{\"POSITION\":1}]}],"
                           "\"weights\":[0.5]}],\"nodes\":[{\"mesh\":0,\"weights\":[1.0]}],\"scenes\":[{\"nodes\":[0]}]",
                           mesh) == GltfImportResult::ok);
    assert(mesh.aabb_min.z == 2.0f);
    arena.free_all();

    // A singular node transform (scale z = 0) still flattens into visible geometry.
    assert(import_triangle(arena,
                           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                           "\"nodes\":[{\"mesh\":0,\"scale\":[1,1,0]}],\"scenes\":[{\"nodes\":[0]}]",
                           mesh) == GltfImportResult::ok);
    assert(mesh.triangles.num_elements == 1u);
    arena.free_all();

    // A skinned mesh ignores its node transform (bind pose).
    assert(import_triangle(arena,
                           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"skins\":[{\"joints\":[1]}],"
                           "\"nodes\":[{\"mesh\":0,\"skin\":0,\"translation\":[10,0,0]},{}],\"scenes\":[{\"nodes\":[0,1]}]",
                           mesh) == GltfImportResult::ok);
    assert(mesh.aabb_min.x == 0.0f);
    arena.free_all();

    // A primitive without UVs does not widen the mesh UV range.
    assert(import_triangle(arena,
                           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":2}},"
                           "{\"attributes\":{\"POSITION\":1}}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]",
                           mesh) == GltfImportResult::ok);
    assert(mesh.uv_min.x == 0.25f && mesh.uv_min.y == 0.25f && fabsf(mesh.uv_scale.x * 65535.0f - 0.5f) < 1e-6f);
    arena.free_all();

    // Coordinates beyond 2^100 are rejected instead of overflowing the bounds math.
    assert(import_triangle(arena,
                           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                           "\"nodes\":[{\"mesh\":0,\"translation\":[1e35,0,0]}],\"scenes\":[{\"nodes\":[0]}]",
                           mesh) == GltfImportResult::invalid_data);
}

static void expect_failure(ArenaAllocator<>& arena, const char* path, GltfImportResult expected, bool exact = true)
{
    const size_t     mark   = arena.buffer_offset;
    ClusterMesh      mesh   = {};
    GltfImportResult result = import_gltf(arena, path, mesh, {});
    assert(result != GltfImportResult::ok && (!exact || result == expected));
    assert(arena.buffer_offset == mark && mesh.blob.data == nullptr && mesh.clusters.num_elements == 0u);
    (void)result;
}

void run_gltf_import_tests()
{
    printf("gltf import:\n");
    ArenaAllocator arena(MB(64));
    ClusterMesh    mesh = {};

    // Cube: P/N/T/UV0, one cluster, closed. Authored tangents must match regenerated ones (no w flip).
    {
        import_ok(arena, "src/tinygltf/models/Cube/Cube.gltf", mesh);
        assert(mesh.clusters.num_elements == 1u && mesh.triangles.num_elements == 12u && mesh.positions.num_elements == 24u);
        // Accessor bounds are [-1, 1] except max z = 1.000001; snapping moves them by at most half a step.
        const float expected_max[3] = {1.0f, 1.0f, 1.000001f};
        for (u32 k = 0; k < 3; k++)
        {
            assert(fabsf(mesh.aabb_min[k] + 1.0f) <= mesh.position_step * 0.5f);
            assert(fabsf(mesh.aabb_max[k] - expected_max[k]) <= mesh.position_step * 0.5f);
        }
        check_watertight(mesh);

        MeshAttribute authored[24];
        double        authored_position[24][3];
        for (u32 i = 0; i < 24u; i++)
        {
            authored[i] = mesh.attributes[i];
            decode_position(mesh, mesh.clusters[0], mesh.positions[i], authored_position[i]);
        }

        ArenaAllocator regenerated_arena(MB(4));
        ClusterMesh    regenerated = {};
        assert(import_gltf(regenerated_arena, asset("src/tinygltf/models/Cube/Cube.gltf"), regenerated,
                           {.recompute_tangents = true}) == GltfImportResult::ok);
        assert(regenerated.positions.num_elements == 24u);
        for (u32 i = 0; i < 24u; i++)
        {
            const MeshAttribute& attribute = regenerated.attributes[i];
            double               position[3];
            decode_position(regenerated, regenerated.clusters[0], regenerated.positions[i], position);

            u32 match = 0;
            while (match < 24u && !(memcmp(authored_position[match], position, sizeof(position)) == 0 &&
                                    authored[match].normal == attribute.normal && authored[match].uv == attribute.uv))
            {
                match++;
            }
            assert(match < 24u);

            double a[3], b[3];
            oct_decode(authored[match].tangent & 0x3FFFFFFFu, 15, a);
            oct_decode(attribute.tangent & 0x3FFFFFFFu, 15, b);
            assert(dot3(a, b) > cos(2.0 * 3.14159265358979 / 180.0));
            assert((authored[match].tangent >> 30) == (attribute.tangent >> 30));
        }
    }

    // Blob placement must re-align when the arena top is not 16-byte aligned.
    {
        arena.free_all();
        const MemoryHandle pad  = arena.allocate(3u, {.ensure_contiguous_alloc = true, .alignment = 1u});
        const size_t       mark = arena.buffer_offset;
        assert(pad.is_valid() && mark == 3u);
        assert(import_gltf(arena, asset("src/tinygltf/models/Cube/Cube.gltf"), mesh) == GltfImportResult::ok);
        validate_cluster_mesh(mesh, arena, mark, {});
    }

    // UBYTE indices, generated tangents.
    import_ok(arena, "src/tinygltf/models/CubeImageUriSpaces/CubeImageUriSpaces.gltf", mesh);
    check_watertight(mesh);

    // POSITION only: flat normals, so all corners of a triangle share one normal.
    import_ok(arena, "src/tinygltf/models/Extensions-overwrite-issue261/issue-261.gltf", mesh);
    for (const MeshCluster& cluster : mesh.clusters)
    {
        for (u32 t = 0; t < triangle_count_of(cluster); t++)
        {
            const u32            packed     = mesh.triangles[triangle_offset_of(cluster) + t];
            const MeshAttribute* attributes = &mesh.attributes[vertex_offset_of(cluster)];
            assert(attributes[(packed >> 8) & 0xFFu].normal == attributes[packed & 0xFFu].normal);
            assert(attributes[(packed >> 16) & 0xFFu].normal == attributes[packed & 0xFFu].normal);
        }
    }

    // Two accessors in one strided view, a rotated root node; a data: URI; morph targets; a single triangle.
    import_ok(arena, "src/meshoptimizer/gltf/fuzz.glb", mesh);
    import_ok(arena, "src/tinygltf/models/box01.glb", mesh);
    import_ok(arena, "src/tinygltf/models/SparseMorphTargets-issue280/singleBlendshapeCube_sparse.glb", mesh);
    {
        arena.free_all();
        const GltfImportResult result =
            import_gltf(arena, asset("src/tinygltf/models/regression/zero-sized-bin-chunk-issue-440.glb"), mesh);
        assert(result == GltfImportResult::ok || result == GltfImportResult::no_geometry);
        if (result == GltfImportResult::ok)
        {
            validate_cluster_mesh(mesh, arena, 0, {});
            assert(mesh.clusters.num_elements == 1u && mesh.triangles.num_elements == 1u);
        }
    }

    // pirate.glb: EXT_meshopt_compression + KHR_mesh_quantization, the only multi-cluster asset.
    const char*                    pirate        = "src/meshoptimizer/demo/pirate.glb";
    const GltfImportSettings pirate_settings[3] = {{}, {.max_cluster_vertices = 32, .max_cluster_triangles = 48},
                                                   {.max_cluster_vertices = 64, .max_cluster_triangles = 64}};
    for (const GltfImportSettings& settings : pirate_settings)
    {
        import_ok(arena, pirate, mesh, settings);
        const u32 lod0_triangles = lod_triangle_count(mesh, 0);
        const u32 minimum        = (lod0_triangles + settings.max_cluster_triangles - 1) / settings.max_cluster_triangles;
        assert(lod_end(mesh, 0) >= minimum && lod0_triangles <= 5010u);
        assert(mesh.lod_count >= 2u);
        const u32 culled = check_cone_culling(mesh);
        assert(culled > 0u); // the soundness check above is vacuous if nothing is ever culled
        printf("    cone test culled %u cluster views\n", culled);
    }

    // LODs off: a single LOD, identical to LOD0 of the default import.
    {
        import_ok(arena, pirate, mesh);
        const u32 lod0_triangles = lod_triangle_count(mesh, 0), lod0_clusters = lod_end(mesh, 0);
        import_ok(arena, pirate, mesh, {.max_lods = 1u});
        assert(mesh.lod_count == 1u && mesh.triangles.num_elements == lod0_triangles &&
               mesh.clusters.num_elements == lod0_clusters);
    }
    run_lod_selection_tests();

    // Failures leave the arena untouched.
    arena.free_all();
    expect_failure(arena, asset("src/tinygltf/models/does-not-exist.gltf"), GltfImportResult::file_error);
    const char* invalid[] = {
        "src/tinygltf/models/BoundsChecking/integer-out-of-bounds.gltf",
        "src/tinygltf/models/BoundsChecking/invalid-buffer-index.gltf",
        "src/tinygltf/models/BoundsChecking/invalid-buffer-view-index.gltf",
        "src/tinygltf/models/BoundsChecking/invalid-primitive-indices.gltf",
        "src/tinygltf/tests/v3/security/negstride.gltf",
        "src/tinygltf/tests/v3/security/oob_buffer_view.gltf",
        "src/tinygltf/tests/v3/security/oob_extension_indices.gltf",
        "src/tinygltf/tests/v3/security/traversal_relative.gltf",
    };
    for (const char* path : invalid)
    {
        expect_failure(arena, asset(path), GltfImportResult::parse_error, false);
    }
    {
        ArenaAllocator small(KB(16));
        expect_failure(small, asset(pirate), GltfImportResult::out_of_memory);
    }
    run_crafted_tests();

    printf("gltf import tests passed\n");
    fflush(stdout);
}

#endif
