#include "import/GltfImport.hpp"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <meshoptimizer.h>
#include <tiny_gltf_v3.h>

using Result = GltfImportResult;

static constexpr int    position_min_exponent = -20;    // ~1 um floor; grows automatically with the mesh extent
static constexpr u32    blob_alignment        = 16u;
static constexpr double max_coordinate        = 0x1p100; // keeps bounds math far from float overflow

// One corner of the flattened source mesh. No padding: deduplication compares all bytes.
struct SourceCorner
{
    float position[3];
    u32   normal;
    u32   tangent;
    u32   uv;
};
static_assert(sizeof(SourceCorner) == 24);

struct SourceCorners
{
    SourceCorner* data;
    u32           count;
    float2        uv_min;
    float2        uv_scale;
};

struct MeshInstance
{
    i32    node;
    i32    mesh;
    double transform[16]; // column-major world matrix
    double determinant;   // of the upper 3x3
};

// Model plus the meshopt-decoded copies of compressed buffer views.
struct GltfData
{
    const tg3_model* model;
    const u8* const* decoded; // per buffer view, null when not compressed
};

template <typename T>
static T* arena_push(ArenaAllocator<>& arena, u64 count)
{
    // Counts come from the file: reject anything that cannot fit before multiplying.
    // A zero-sized request would come back as an invalid handle.
    if (count > (arena.buffer_len - arena.buffer_offset) / sizeof(T))
    {
        return nullptr;
    }
    const MemoryHandle handle =
        arena.allocate(sizeof(T) * (count > 0 ? count : 1), {.ensure_contiguous_alloc = true, .alignment = blob_alignment});
    return handle.is_valid() ? static_cast<T*>(arena.handle_to_ptr(handle)) : nullptr;
}

// ---------------------------------------------------------------------------
// Small double-precision vector helpers
// ---------------------------------------------------------------------------

static void cross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static double dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Normalizes in place; false (vector untouched) when the length is zero or not finite.
static bool normalize(double v[3])
{
    const double length = sqrt(dot(v, v));
    if (!(length > 1e-30) || !isfinite(length))
    {
        return false;
    }
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

// Unit tangent perpendicular to a unit normal (Duff et al. 2017), for meshes without UVs.
static void basis_tangent(const double n[3], double out[3])
{
    const double sign = copysign(1.0, n[2]);
    const double a    = -1.0 / (sign + n[2]);
    out[0]            = 1.0 + sign * n[0] * n[0] * a;
    out[1]            = sign * n[0] * n[1] * a;
    out[2]            = -sign * n[0];
}

// Octahedral encoding as biased snorm: (q + bias) per axis, so 0 stays exact. n must be unit length.
static u32 oct_encode(const double n[3], int bits)
{
    const double l1 = fabs(n[0]) + fabs(n[1]) + fabs(n[2]);
    double       u  = n[0] / l1;
    double       v  = n[1] / l1;
    if (n[2] < 0.0)
    {
        const double fold_u = (1.0 - fabs(v)) * (u >= 0.0 ? 1.0 : -1.0);
        v                   = (1.0 - fabs(u)) * (v >= 0.0 ? 1.0 : -1.0);
        u                   = fold_u;
    }
    const int bias = (1 << (bits - 1)) - 1;
    return u32(meshopt_quantizeSnorm(float(u), bits) + bias) | u32(meshopt_quantizeSnorm(float(v), bits) + bias) << bits;
}

// ---------------------------------------------------------------------------
// glTF access
// ---------------------------------------------------------------------------

// Accessor index of a named attribute (primitive attributes or a morph target), -1 if absent or out of range.
static i32 find_accessor(const tg3_model& model, const tg3_str_int_pair* attributes, u32 count, const char* name)
{
    for (u32 i = 0; i < count; i++)
    {
        if (tg3_str_equals_cstr(attributes[i].key, name))
        {
            const i32 index = attributes[i].value;
            return index >= 0 && (u32)index < model.accessors_count ? index : -1;
        }
    }
    return -1;
}

static i32 find_attribute(const tg3_model& model, const tg3_primitive& primitive, const char* name)
{
    return find_accessor(model, primitive.attributes, primitive.attributes_count, name);
}

static const tg3_value* find_extension(const tg3_extras_ext& ext, const char* name)
{
    for (u32 i = 0; i < ext.extensions_count; i++)
    {
        if (tg3_str_equals_cstr(ext.extensions[i].name, name))
        {
            return &ext.extensions[i].value;
        }
    }
    return nullptr;
}

static const tg3_value* find_member(const tg3_value& object, const char* key)
{
    if (object.type != TG3_VALUE_OBJECT)
    {
        return nullptr;
    }
    for (u32 i = 0; i < object.object_count; i++)
    {
        if (tg3_str_equals_cstr(object.object_data[i].key, key))
        {
            return &object.object_data[i].value;
        }
    }
    return nullptr;
}

static u64 member_u64(const tg3_value& object, const char* key, u64 fallback)
{
    const tg3_value* value = find_member(object, key);
    if (value && value->type == TG3_VALUE_INT && value->int_val >= 0)
    {
        return (u64)value->int_val;
    }
    // Extension values are not range-checked by tinygltf; casting a double >= 2^64 would be UB.
    if (value && value->type == TG3_VALUE_REAL && value->real_val >= 0.0 && value->real_val < 0x1p64 &&
        value->real_val == floor(value->real_val))
    {
        return (u64)value->real_val;
    }
    return fallback;
}

static bool member_is(const tg3_value& object, const char* key, const char* string)
{
    const tg3_value* value = find_member(object, key);
    return value && value->type == TG3_VALUE_STRING && tg3_str_equals_cstr(value->string_val, string);
}

// EXT_meshopt_compression / KHR_meshopt_compression buffer view, decoded into dst (byte_length bytes, zeroed).
// Everything is validated first: meshopt's own checks are debug-only asserts.
static bool decode_meshopt_view(const tg3_model& model, const tg3_buffer_view& view, const tg3_value& ext, u8* dst)
{
    const u64 buffer      = member_u64(ext, "buffer", UINT64_MAX);
    const u64 byte_offset = member_u64(ext, "byteOffset", 0);
    const u64 byte_length = member_u64(ext, "byteLength", 0);
    const u64 stride      = member_u64(ext, "byteStride", 0);
    const u64 count       = member_u64(ext, "count", 0);

    if (buffer >= model.buffers_count || stride == 0 || stride > 256 || count > view.byte_length / stride)
    {
        return false;
    }
    const tg3_span_u8& source = model.buffers[buffer].data;
    if (!source.data || byte_offset > source.count || byte_length > source.count - byte_offset)
    {
        return false;
    }
    const u8* src = source.data + byte_offset;

    const bool attributes = member_is(ext, "mode", "ATTRIBUTES");
    int        status     = -1;
    if (attributes && stride % 4 == 0)
    {
        status = meshopt_decodeVertexBuffer(dst, count, stride, src, byte_length);
    }
    else if (member_is(ext, "mode", "TRIANGLES") && (stride == 2 || stride == 4) && count % 3 == 0)
    {
        status = meshopt_decodeIndexBuffer(dst, count, stride, src, byte_length);
    }
    else if (member_is(ext, "mode", "INDICES") && (stride == 2 || stride == 4))
    {
        status = meshopt_decodeIndexSequence(dst, count, stride, src, byte_length);
    }
    if (status != 0)
    {
        return false;
    }

    if (!find_member(ext, "filter") || member_is(ext, "filter", "NONE"))
    {
        return true;
    }
    if (!attributes)
    {
        return false;
    }
    if (member_is(ext, "filter", "OCTAHEDRAL") && (stride == 4 || stride == 8))
    {
        meshopt_decodeFilterOct(dst, count, stride);
    }
    else if (member_is(ext, "filter", "QUATERNION") && stride == 8)
    {
        meshopt_decodeFilterQuat(dst, count, stride);
    }
    else if (member_is(ext, "filter", "EXPONENTIAL"))
    {
        meshopt_decodeFilterExp(dst, count, stride);
    }
    else if (member_is(ext, "filter", "COLOR") && (stride == 4 || stride == 8))
    {
        meshopt_decodeFilterColor(dst, count, stride);
    }
    else
    {
        return false;
    }
    return true;
}

// Bytes of a buffer view (or its decoded copy); null when the data is missing.
static const u8* view_bytes(const GltfData& gltf, i32 view_index)
{
    const tg3_model& model = *gltf.model;
    if (view_index < 0 || (u32)view_index >= model.buffer_views_count)
    {
        return nullptr;
    }
    if (gltf.decoded[view_index])
    {
        return gltf.decoded[view_index];
    }
    const tg3_buffer_view& view = model.buffer_views[view_index];
    if (view.buffer < 0 || (u32)view.buffer >= model.buffers_count)
    {
        return nullptr;
    }
    // No data: a meshopt fallback buffer, or a buffer without a uri.
    const tg3_span_u8& data = model.buffers[view.buffer].data;
    if (!data.data || view.byte_offset > data.count || view.byte_length > data.count - view.byte_offset)
    {
        return nullptr;
    }
    return data.data + view.byte_offset;
}

static float read_component(const u8* p, i32 component_type, bool normalized)
{
    switch (component_type)
    {
    case TG3_COMPONENT_TYPE_FLOAT:
    {
        float value;
        memcpy(&value, p, sizeof(value));
        return value;
    }
    case TG3_COMPONENT_TYPE_BYTE:
    {
        int8 value;
        memcpy(&value, p, sizeof(value));
        return normalized ? fmaxf(value / 127.0f, -1.0f) : (float)value;
    }
    case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
        return normalized ? p[0] / 255.0f : (float)p[0];
    case TG3_COMPONENT_TYPE_SHORT:
    {
        int16 value;
        memcpy(&value, p, sizeof(value));
        return normalized ? fmaxf(value / 32767.0f, -1.0f) : (float)value;
    }
    case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
    {
        uint16 value;
        memcpy(&value, p, sizeof(value));
        return normalized ? value / 65535.0f : (float)value;
    }
    case TG3_COMPONENT_TYPE_UNSIGNED_INT:
    {
        u32 value;
        memcpy(&value, p, sizeof(value));
        return normalized ? (float)(value / 4294967295.0) : (float)value;
    }
    default:
        return NAN; // rejected later as non-finite
    }
}

static u32 read_index(const u8* p, i32 component_type)
{
    switch (component_type)
    {
    case TG3_COMPONENT_TYPE_UNSIGNED_BYTE:
        return p[0];
    case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
    {
        uint16 value;
        memcpy(&value, p, sizeof(value));
        return value;
    }
    case TG3_COMPONENT_TYPE_UNSIGNED_INT:
    {
        u32 value;
        memcpy(&value, p, sizeof(value));
        return value;
    }
    default:
        return UINT32_MAX;
    }
}

// Reads every element as `dst_components` floats (extra components zeroed), applying normalization
// (and so KHR_mesh_quantization) and sparse substitution. Strides and ranges are validated by tinygltf.
static bool read_floats(const GltfData& gltf, const tg3_accessor& accessor, float* dst, u32 dst_components)
{
    const u32  components     = (u32)tg3_num_components(accessor.type);
    const i32  component_size = tg3_component_size(accessor.component_type);
    const u32  copied         = components < dst_components ? components : dst_components;
    const bool normalized     = accessor.normalized != 0;

    if (components > 4u || component_size <= 0)
    {
        return false;
    }

    if (accessor.buffer_view >= 0)
    {
        const u8* base = view_bytes(gltf, accessor.buffer_view);
        if (!base)
        {
            return false;
        }
        base += accessor.byte_offset;
        const u64 stride = (u64)tg3_accessor_byte_stride(&accessor, &gltf.model->buffer_views[accessor.buffer_view]);

        for (u64 i = 0; i < accessor.count; i++)
        {
            float* out = dst + i * dst_components;
            for (u32 c = 0; c < dst_components; c++)
            {
                out[c] = c < copied ? read_component(base + i * stride + c * component_size, accessor.component_type, normalized)
                                    : 0.0f;
            }
        }
    }
    else
    {
        memset(dst, 0, accessor.count * dst_components * sizeof(float));
    }

    if (accessor.sparse.is_sparse)
    {
        const tg3_accessor_sparse& sparse  = accessor.sparse;
        const u8*                  indices = view_bytes(gltf, sparse.indices.buffer_view);
        const u8*                  values  = view_bytes(gltf, sparse.values.buffer_view);
        if (!indices || !values)
        {
            return false;
        }
        indices += sparse.indices.byte_offset;
        values += sparse.values.byte_offset;

        const u32 index_size   = (u32)tg3_component_size(sparse.indices.component_type);
        const u32 element_size = components * (u32)component_size; // sparse values are tightly packed
        for (i32 s = 0; s < sparse.count; s++)
        {
            const u32 target = read_index(indices + (size_t)s * index_size, sparse.indices.component_type);
            if (target >= accessor.count) // not validated by tinygltf
            {
                return false;
            }
            for (u32 c = 0; c < copied; c++)
            {
                dst[(size_t)target * dst_components + c] =
                    read_component(values + (size_t)s * element_size + c * component_size, accessor.component_type, normalized);
            }
        }
    }
    return true;
}

static Result read_indices(const GltfData& gltf, const tg3_accessor& accessor, u32* dst, u32 vertex_count)
{
    const i32 type = accessor.component_type;
    if (accessor.sparse.is_sparse)
    {
        return Result::unsupported;
    }
    const u8* base = view_bytes(gltf, accessor.buffer_view);
    if (accessor.type != TG3_TYPE_SCALAR || !base ||
        (type != TG3_COMPONENT_TYPE_UNSIGNED_BYTE && type != TG3_COMPONENT_TYPE_UNSIGNED_SHORT &&
         type != TG3_COMPONENT_TYPE_UNSIGNED_INT))
    {
        return Result::invalid_data;
    }
    base += accessor.byte_offset;
    const u64 stride = (u64)tg3_accessor_byte_stride(&accessor, &gltf.model->buffer_views[accessor.buffer_view]);

    for (u64 i = 0; i < accessor.count; i++)
    {
        dst[i] = read_index(base + i * stride, type);
        if (dst[i] >= vertex_count)
        {
            return Result::invalid_data;
        }
    }
    return Result::ok;
}

// Triangle-list corner count; 0 for points, lines and primitives without POSITION. Counts above INT32_MAX
// (accessors without a bufferView are not size-checked by tinygltf) come back as INT32_MAX + 1.
static u64 primitive_corner_count(const tg3_model& model, const tg3_primitive& primitive)
{
    const i32 position = find_attribute(model, primitive, "POSITION");
    if (position < 0)
    {
        return 0;
    }
    if (primitive.indices >= 0 && (u32)primitive.indices >= model.accessors_count)
    {
        return (u64)INT32_MAX + 1;
    }
    const u64 count = model.accessors[primitive.indices >= 0 ? primitive.indices : position].count;
    if (count > (u64)INT32_MAX)
    {
        return (u64)INT32_MAX + 1;
    }
    switch (primitive.mode)
    {
    case -1:
    case TG3_MODE_TRIANGLES:
        return count / 3 * 3;
    case TG3_MODE_TRIANGLE_STRIP:
    case TG3_MODE_TRIANGLE_FAN:
        return count >= 3 ? (count - 2) * 3 : 0;
    default:
        return 0;
    }
}

static void node_local_matrix(const tg3_node& node, double out[16])
{
    if (node.has_matrix)
    {
        memcpy(out, node.matrix, sizeof(double) * 16);
        return;
    }

    double q[4] = {node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]};
    const double length = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length > 0.0)
    {
        for (double& c : q)
        {
            c /= length;
        }
    }
    const double  x = q[0], y = q[1], z = q[2], w = q[3];
    const double* s = node.scale;
    const double* t = node.translation;

    // T * R * S, column-major
    out[0]  = (1.0 - 2.0 * (y * y + z * z)) * s[0];
    out[1]  = 2.0 * (x * y + z * w) * s[0];
    out[2]  = 2.0 * (x * z - y * w) * s[0];
    out[3]  = 0.0;
    out[4]  = 2.0 * (x * y - z * w) * s[1];
    out[5]  = (1.0 - 2.0 * (x * x + z * z)) * s[1];
    out[6]  = 2.0 * (y * z + x * w) * s[1];
    out[7]  = 0.0;
    out[8]  = 2.0 * (x * z + y * w) * s[2];
    out[9]  = 2.0 * (y * z - x * w) * s[2];
    out[10] = (1.0 - 2.0 * (x * x + y * y)) * s[2];
    out[11] = 0.0;
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1.0;
}

static void multiply(const double a[16], const double b[16], double out[16])
{
    for (u32 column = 0; column < 4; column++)
    {
        for (u32 row = 0; row < 4; row++)
        {
            double sum = 0.0;
            for (u32 k = 0; k < 4; k++)
            {
                sum += a[k * 4 + row] * b[column * 4 + k];
            }
            out[column * 4 + row] = sum;
        }
    }
}

// Mesh instances of the default scene (or of every parentless node when there are no scenes), with world
// transforms. Iterative DFS; the visited flags guard against cycles and nodes with several parents.
static Result collect_instances(ArenaAllocator<>& arena, const tg3_model& model, MeshInstance*& out_instances,
                                u32& out_count)
{
    const u32     node_count = model.nodes_count;
    MeshInstance* instances  = arena_push<MeshInstance>(arena, node_count);
    double*       world      = arena_push<double>(arena, (size_t)node_count * 16);
    i32*          stack      = arena_push<i32>(arena, node_count);
    u8*           visited    = arena_push<u8>(arena, node_count);
    i32*          roots      = arena_push<i32>(arena, node_count);
    if (!instances || !world || !stack || !visited || !roots)
    {
        return Result::out_of_memory;
    }

    u32 root_count = 0;
    if (model.scenes_count > 0)
    {
        const tg3_scene& scene =
            model.scenes[model.default_scene >= 0 && (u32)model.default_scene < model.scenes_count ? model.default_scene : 0];
        for (u32 i = 0; i < scene.nodes_count && root_count < node_count; i++)
        {
            roots[root_count++] = scene.nodes[i];
        }
    }
    else
    {
        u8* has_parent = visited; // borrowed, cleared again below
        for (u32 n = 0; n < node_count; n++)
        {
            for (u32 c = 0; c < model.nodes[n].children_count; c++)
            {
                const i32 child = model.nodes[n].children[c];
                if (child >= 0 && (u32)child < node_count)
                {
                    has_parent[child] = 1;
                }
            }
        }
        for (u32 n = 0; n < node_count; n++)
        {
            if (!has_parent[n])
            {
                roots[root_count++] = (i32)n;
            }
        }
        memset(visited, 0, node_count);
    }

    u32 instance_count = 0;
    for (u32 r = 0; r < root_count; r++)
    {
        const i32 root = roots[r];
        if (root < 0 || (u32)root >= node_count || visited[root])
        {
            continue;
        }
        visited[root] = 1;
        node_local_matrix(model.nodes[root], &world[root * 16]);

        u32 top      = 0;
        stack[top++] = root;
        while (top > 0)
        {
            const i32       index = stack[--top];
            const tg3_node& node  = model.nodes[index];

            if (node.mesh >= 0 && (u32)node.mesh < model.meshes_count)
            {
                MeshInstance& instance = instances[instance_count++];
                instance.node          = index;
                instance.mesh          = node.mesh;
                memcpy(instance.transform, &world[index * 16], sizeof(instance.transform));
                if (node.skin >= 0)
                {
                    // glTF: a skinned mesh ignores its node transform; without posing, that is the bind pose.
                    static const double identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                    memcpy(instance.transform, identity, sizeof(instance.transform));
                }

                const double* m = instance.transform;
                const double  a[3] = {m[0], m[1], m[2]}, b[3] = {m[4], m[5], m[6]}, c[3] = {m[8], m[9], m[10]};
                double        bc[3];
                cross(b, c, bc);
                instance.determinant = dot(a, bc);
            }

            for (u32 c = 0; c < node.children_count; c++)
            {
                const i32 child = node.children[c];
                if (child < 0 || (u32)child >= node_count || visited[child])
                {
                    continue;
                }
                visited[child] = 1;
                double local[16];
                node_local_matrix(model.nodes[child], local);
                multiply(&world[index * 16], local, &world[child * 16]);
                stack[top++] = child;
            }
        }
    }

    out_instances = instances;
    out_count     = instance_count;
    return Result::ok;
}

// Appends one primitive instance as world-space corners (triangle list, front faces CCW).
// All scratch is allocated from the arena; the caller rewinds it afterwards.
static Result emit_primitive(ArenaAllocator<>& arena, const GltfData& gltf, const tg3_primitive& primitive,
                             const MeshInstance& instance, bool recompute_tangents, SourceCorner* corners,
                             float* corner_uvs, u32& corner_cursor)
{
    const tg3_model& model = *gltf.model;

    const u32 n = (u32)primitive_corner_count(model, primitive); // the caller bounded the total
    if (n == 0)
    {
        return Result::ok;
    }

    // An accessor without a bufferView reads as zeros (glTF), e.g. a base shape driven by morph targets.
    const tg3_accessor& position_accessor = model.accessors[find_attribute(model, primitive, "POSITION")];
    if (position_accessor.type != TG3_TYPE_VEC3)
    {
        return Result::invalid_data;
    }
    if (position_accessor.count > (u64)INT32_MAX)
    {
        return Result::unsupported;
    }
    const u32 vertex_count = (u32)position_accessor.count;

    // glTF: without NORMAL, flat normals are used and any TANGENT must be ignored.
    const i32 normal_index  = find_attribute(model, primitive, "NORMAL");
    const i32 tangent_index = recompute_tangents || normal_index < 0 ? -1 : find_attribute(model, primitive, "TANGENT");
    const i32 uv_index      = find_attribute(model, primitive, "TEXCOORD_0");

    auto matches = [&](i32 index, i32 type)
    { return index < 0 || (model.accessors[index].type == type && model.accessors[index].count == vertex_count); };
    if (!matches(normal_index, TG3_TYPE_VEC3) || !matches(tangent_index, TG3_TYPE_VEC4) || !matches(uv_index, TG3_TYPE_VEC2))
    {
        return Result::invalid_data;
    }

    const u64 raw_count = primitive.indices >= 0 ? model.accessors[primitive.indices].count : vertex_count;

    float* positions = arena_push<float>(arena, (size_t)vertex_count * 3);
    float* normals   = normal_index >= 0 ? arena_push<float>(arena, (size_t)vertex_count * 3) : nullptr;
    float* tangents  = tangent_index >= 0 ? arena_push<float>(arena, (size_t)vertex_count * 4) : nullptr;
    float* uvs       = uv_index >= 0 ? arena_push<float>(arena, (size_t)vertex_count * 2) : nullptr;
    u32*   raw       = arena_push<u32>(arena, raw_count);
    u32*   order     = arena_push<u32>(arena, n);
    float* P         = arena_push<float>(arena, (size_t)n * 3);
    float* N         = arena_push<float>(arena, (size_t)n * 3);
    float* T         = arena_push<float>(arena, (size_t)n * 4);
    float* UV        = arena_push<float>(arena, (size_t)n * 2);
    if (!positions || (normal_index >= 0 && !normals) || (tangent_index >= 0 && !tangents) || (uv_index >= 0 && !uvs) ||
        !raw || !order || !P || !N || !T || !UV)
    {
        return Result::out_of_memory;
    }

    if (!read_floats(gltf, position_accessor, positions, 3) ||
        (normals && !read_floats(gltf, model.accessors[normal_index], normals, 3)) ||
        (tangents && !read_floats(gltf, model.accessors[tangent_index], tangents, 4)) ||
        (uvs && !read_floats(gltf, model.accessors[uv_index], uvs, 2)))
    {
        return Result::invalid_data;
    }

    // Default morph weights are part of the rest shape: node.weights, else mesh.weights.
    const tg3_node& node         = model.nodes[instance.node];
    const tg3_mesh& mesh         = model.meshes[instance.mesh];
    const double*   weights      = node.weights_count > 0 ? node.weights : mesh.weights;
    const u32       weight_count = node.weights_count > 0 ? node.weights_count : mesh.weights_count;
    float*          delta        = nullptr;
    for (u32 t = 0; t < primitive.targets_count && t < weight_count; t++)
    {
        if (weights[t] == 0.0)
        {
            continue;
        }
        float* const streams[3] = {positions, normals, tangents};
        const char*  names[3]   = {"POSITION", "NORMAL", "TANGENT"};
        for (u32 s = 0; s < 3; s++)
        {
            const i32 index = streams[s] ? find_accessor(model, primitive.targets[t], primitive.target_attribute_counts[t], names[s]) : -1;
            if (index < 0)
            {
                continue;
            }
            delta = delta ? delta : arena_push<float>(arena, (u64)vertex_count * 3);
            if (!delta)
            {
                return Result::out_of_memory;
            }
            const tg3_accessor& accessor = model.accessors[index];
            if (accessor.type != TG3_TYPE_VEC3 || accessor.count != vertex_count || !read_floats(gltf, accessor, delta, 3))
            {
                return Result::invalid_data;
            }
            const u32 stride = s == 2 ? 4u : 3u; // tangent deltas are xyz only
            for (u32 v = 0; v < vertex_count; v++)
            {
                for (u32 k = 0; k < 3; k++)
                {
                    streams[s][v * stride + k] += (float)(weights[t] * delta[v * 3 + k]);
                }
            }
        }
    }

    if (primitive.indices >= 0)
    {
        const Result result = read_indices(gltf, model.accessors[primitive.indices], raw, vertex_count);
        if (result != Result::ok)
        {
            return result;
        }
    }
    else
    {
        for (u32 i = 0; i < vertex_count; i++)
        {
            raw[i] = i;
        }
    }

    // Strips and fans become lists with the same winding.
    if (primitive.mode == TG3_MODE_TRIANGLE_STRIP || primitive.mode == TG3_MODE_TRIANGLE_FAN)
    {
        for (u32 i = 2; i < raw_count; i++)
        {
            u32* tri = &order[(i - 2) * 3];
            if (primitive.mode == TG3_MODE_TRIANGLE_STRIP)
            {
                const u32 flip = i & 1u;
                tri[0]         = raw[i - 2 + flip];
                tri[1]         = raw[i - 1 - flip];
            }
            else
            {
                tri[0] = raw[0];
                tri[1] = raw[i - 1];
            }
            tri[2] = raw[i];
        }
    }
    else
    {
        memcpy(order, raw, (size_t)n * sizeof(u32));
    }

    // De-index into per-corner streams.
    for (u32 c = 0; c < n; c++)
    {
        const u32 v = order[c];
        for (u32 k = 0; k < 3; k++)
        {
            P[c * 3 + k] = positions[v * 3 + k];
            N[c * 3 + k] = normals ? normals[v * 3 + k] : 0.0f;
        }
        for (u32 k = 0; k < 4; k++)
        {
            T[c * 4 + k] = tangents ? tangents[v * 4 + k] : 0.0f;
        }
        UV[c * 2 + 0] = uvs ? uvs[v * 2 + 0] : 0.0f;
        UV[c * 2 + 1] = uvs ? uvs[v * 2 + 1] : 0.0f;

        if (!isfinite(P[c * 3 + 0]) || !isfinite(P[c * 3 + 1]) || !isfinite(P[c * 3 + 2]) || !isfinite(UV[c * 2 + 0]) ||
            !isfinite(UV[c * 2 + 1]))
        {
            return Result::invalid_data;
        }
    }

    // Unit normals (meshopt_generateTangents requires them); missing or zero ones fall back to the face normal.
    for (u32 t = 0; t < n; t += 3)
    {
        const float* p = &P[t * 3];
        const double e1[3] = {(double)p[3] - p[0], (double)p[4] - p[1], (double)p[5] - p[2]};
        const double e2[3] = {(double)p[6] - p[0], (double)p[7] - p[1], (double)p[8] - p[2]};
        double       face[3];
        cross(e1, e2, face);
        if (!normalize(face))
        {
            face[0] = 0.0, face[1] = 0.0, face[2] = 1.0;
        }

        for (u32 c = t; c < t + 3; c++)
        {
            double normal[3] = {N[c * 3 + 0], N[c * 3 + 1], N[c * 3 + 2]};
            if (!normals || !normalize(normal))
            {
                memcpy(normal, face, sizeof(normal));
            }
            for (u32 k = 0; k < 3; k++)
            {
                N[c * 3 + k] = (float)normal[k];
            }
        }
    }

    if (!tangents && uvs)
    {
        // MikkTSpace-compatible, which glTF normal maps are baked against. No sign flip (same as gltfpack).
        meshopt_generateTangents(T, nullptr, n, P, n, sizeof(float) * 3, N, sizeof(float) * 3, UV, sizeof(float) * 2,
                                 meshopt_TangentCompatible);
    }

    // World transform: points by M, normals by the cofactor matrix (inverse-transpose up to scale), tangents by
    // M then re-orthogonalized. A mirroring transform flips the bitangent sign and the winding.
    const double* m      = instance.transform;
    const double  a[3]   = {m[0], m[1], m[2]}, b[3] = {m[4], m[5], m[6]}, c3[3] = {m[8], m[9], m[10]};
    const bool    mirror = instance.determinant < 0.0;
    double        bc[3], ca[3], ab[3];
    cross(b, c3, bc);
    cross(c3, a, ca);
    cross(a, b, ab);

    for (u32 t = 0; t < n; t += 3)
    {
        for (u32 j = 0; j < 3; j++)
        {
            const u32 src = t + (mirror && j > 0 ? 3 - j : j);
            const u32 dst = corner_cursor + t + j;

            const float* p = &P[src * 3];
            const float* nn = &N[src * 3];
            const float* tt = &T[src * 4];

            double normal[3], tangent[3];
            for (u32 k = 0; k < 3; k++)
            {
                const double position = a[k] * p[0] + b[k] * p[1] + c3[k] * p[2] + m[12 + k];
                if (!(fabs(position) <= max_coordinate)) // also rejects NaN, before the narrowing cast
                {
                    return Result::invalid_data;
                }
                // + 0.0f folds -0 into +0, so equal positions dedup bitwise.
                corners[dst].position[k] = (float)position + 0.0f;
                normal[k]  = (mirror ? -1.0 : 1.0) * (bc[k] * nn[0] + ca[k] * nn[1] + ab[k] * nn[2]);
                tangent[k] = a[k] * tt[0] + b[k] * tt[1] + c3[k] * tt[2];
            }
            if (!normalize(normal))
            {
                normal[0] = 0.0, normal[1] = 0.0, normal[2] = 1.0;
            }

            const double along = dot(normal, tangent);
            for (u32 k = 0; k < 3; k++)
            {
                tangent[k] -= normal[k] * along;
            }
            bool negative_sign = tt[3] < 0.0f;
            if (!normalize(tangent))
            {
                basis_tangent(normal, tangent); // no UVs, or degenerate input
                negative_sign = false;
            }
            negative_sign = negative_sign != mirror;

            corners[dst].normal  = oct_encode(normal, 16);
            corners[dst].tangent = oct_encode(tangent, 15) | (negative_sign ? 1u << 30 : 0u);
            // NaN marks "no UVs" so these corners stay out of the mesh UV range.
            corner_uvs[dst * 2 + 0] = uvs ? UV[src * 2 + 0] : NAN;
            corner_uvs[dst * 2 + 1] = uvs ? UV[src * 2 + 1] : NAN;
        }
    }
    corner_cursor += n;
    return Result::ok;
}

// Stage 1: parse the file and flatten every reachable triangle into world-space corners.
// tinygltf's memory is released when this returns.
static Result gather_corners(ArenaAllocator<>& arena, const char* filepath, const GltfImportSettings& settings,
                             SourceCorners& out)
{
    tg3_parse_options options;
    tg3_parse_options_init(&options);
    options.memory.memory_budget = UINT64_MAX; // the default 1 GiB cap would reject large but valid files

    tinygltf3::Model      model;
    tinygltf3::ErrorStack errors;
    const tg3_error_code  parsed = tinygltf3::parse_file(model, errors, filepath, &options);
    if (parsed != TG3_OK)
    {
        Result result = parsed == TG3_ERR_FILE_NOT_FOUND || parsed == TG3_ERR_FILE_READ ? Result::file_error : Result::parse_error;
        for (u32 i = 0; i < errors.count(); i++)
        {
            const tg3_error_entry* entry = errors.entry(i);
            if (entry->code == TG3_ERR_OUT_OF_MEMORY)
            {
                result = Result::out_of_memory;
            }
            else if (result == Result::parse_error && (entry->code == TG3_ERR_FILE_NOT_FOUND || entry->code == TG3_ERR_FILE_READ))
            {
                result = Result::file_error;
            }
            fprintf(stderr, "import_gltf: %s: %s%s%s\n", filepath, entry->message ? entry->message : "error",
                    entry->json_path ? " at " : "", entry->json_path ? entry->json_path : "");
        }
        return result;
    }
    const tg3_model& m = *model.get();

    // Only extensions that change what the geometry is matter; material/texture ones are irrelevant here.
    for (u32 i = 0; i < m.extensions_required_count; i++)
    {
        if (tg3_str_equals_cstr(m.extensions_required[i], "KHR_draco_mesh_compression") ||
            tg3_str_equals_cstr(m.extensions_required[i], "EXT_mesh_gpu_instancing"))
        {
            return Result::unsupported;
        }
    }

    const u8** decoded = arena_push<const u8*>(arena, m.buffer_views_count);
    if (!decoded)
    {
        return Result::out_of_memory;
    }
    for (u32 i = 0; i < m.buffer_views_count; i++)
    {
        const tg3_buffer_view& view    = m.buffer_views[i];
        const tg3_value*       meshopt = find_extension(view.ext, "EXT_meshopt_compression");
        if (!meshopt)
        {
            meshopt = find_extension(view.ext, "KHR_meshopt_compression");
        }
        if (!meshopt)
        {
            continue;
        }
        u8* data = arena_push<u8>(arena, view.byte_length);
        if (!data)
        {
            return Result::out_of_memory;
        }
        if (!decode_meshopt_view(m, view, *meshopt, data))
        {
            return Result::invalid_data;
        }
        decoded[i] = data;
    }
    const GltfData gltf = {.model = &m, .decoded = decoded};

    MeshInstance* instances      = nullptr;
    u32           instance_count = 0;
    const Result  collected      = collect_instances(arena, m, instances, instance_count);
    if (collected != Result::ok)
    {
        return collected;
    }

    // Each count is at most INT32_MAX + 1, so the sum cannot wrap before the check.
    u64 total = 0;
    for (u32 i = 0; i < instance_count; i++)
    {
        const tg3_mesh& mesh = m.meshes[instances[i].mesh];
        for (u32 p = 0; p < mesh.primitives_count; p++)
        {
            total += primitive_corner_count(m, mesh.primitives[p]);
            if (total > (u64)INT32_MAX)
            {
                return Result::unsupported;
            }
        }
    }
    if (total == 0)
    {
        return Result::no_geometry;
    }

    SourceCorner* corners = arena_push<SourceCorner>(arena, total);
    if (!corners)
    {
        return Result::out_of_memory;
    }
    const size_t uv_mark    = arena.buffer_offset; // the float UVs are dropped once packed
    float*       corner_uvs = arena_push<float>(arena, total * 2);
    if (!corner_uvs)
    {
        return Result::out_of_memory;
    }

    u32 cursor = 0;
    for (u32 i = 0; i < instance_count; i++)
    {
        // Singular transforms (e.g. scale 0 on one axis) still flatten into visible geometry; whatever
        // collapses is filtered as degenerate later.
        const tg3_mesh& mesh = m.meshes[instances[i].mesh];
        for (u32 p = 0; p < mesh.primitives_count; p++)
        {
            const size_t primitive_mark = arena.buffer_offset;
            const Result result = emit_primitive(arena, gltf, mesh.primitives[p], instances[i], settings.recompute_tangents,
                                                 corners, corner_uvs, cursor);
            arena.buffer_offset = primitive_mark;
            if (result != Result::ok)
            {
                return result;
            }
        }
    }
    assert(cursor == total);

    // UVs as unorm16 over the mesh UV range, packed before dedup so near-equal values merge.
    // Corners without UVs (NaN) are left out of the range and packed as 0.
    float uv_lo[2] = {FLT_MAX, FLT_MAX};
    float uv_hi[2] = {-FLT_MAX, -FLT_MAX};
    for (u32 c = 0; c < cursor; c++)
    {
        for (u32 k = 0; k < 2; k++)
        {
            uv_lo[k] = fminf(uv_lo[k], corner_uvs[c * 2 + k]); // fminf/fmaxf ignore NaN
            uv_hi[k] = fmaxf(uv_hi[k], corner_uvs[c * 2 + k]);
        }
    }
    for (u32 k = 0; k < 2; k++)
    {
        if (uv_lo[k] > uv_hi[k]) // no UVs at all
        {
            uv_lo[k] = uv_hi[k] = 0.0f;
        }
    }
    const double uv_range[2] = {(double)uv_hi[0] - uv_lo[0], (double)uv_hi[1] - uv_lo[1]};
    for (u32 c = 0; c < cursor; c++)
    {
        u32 packed[2] = {0u, 0u};
        for (u32 k = 0; k < 2; k++)
        {
            if (uv_range[k] > 0.0 && !isnan(corner_uvs[c * 2 + k]))
            {
                packed[k] = (u32)lround(((double)corner_uvs[c * 2 + k] - uv_lo[k]) / uv_range[k] * 65535.0);
            }
        }
        corners[c].uv = packed[0] | packed[1] << 16;
    }
    arena.buffer_offset = uv_mark;

    out.data     = corners;
    out.count    = cursor;
    out.uv_min   = {uv_lo[0], uv_lo[1]};
    out.uv_scale = {(float)(uv_range[0] / 65535.0), (float)(uv_range[1] / 65535.0)};
    return Result::ok;
}

// ---------------------------------------------------------------------------
// Clusters
// ---------------------------------------------------------------------------

static bool grid_equal(const i32* grid, u32 a, u32 b)
{
    return grid[a * 3 + 0] == grid[b * 3 + 0] && grid[a * 3 + 1] == grid[b * 3 + 1] && grid[a * 3 + 2] == grid[b * 3 + 2];
}

// Triangles whose corners snapped onto the same grid point are dropped.
static bool collapsed(const i32* grid, const u32* local_vertices, const u8* triangle)
{
    const u32 a = local_vertices[triangle[0]], b = local_vertices[triangle[1]], c = local_vertices[triangle[2]];
    return grid_equal(grid, a, b) || grid_equal(grid, b, c) || grid_equal(grid, a, c);
}

#if !defined(NDEBUG)
struct TriangleKey
{
    u32 corner[3][6]; // grid xyz, normal, tangent, uv
};

static int compare_keys(const void* a, const void* b) { return memcmp(a, b, sizeof(TriangleKey)); }

// Rotates the corners (keeping the winding) so the smallest comes first.
static void canonicalize(TriangleKey& key)
{
    u32 first = 0;
    for (u32 i = 1; i < 3; i++)
    {
        if (memcmp(key.corner[i], key.corner[first], sizeof(key.corner[0])) < 0)
        {
            first = i;
        }
    }
    TriangleKey rotated;
    for (u32 i = 0; i < 3; i++)
    {
        memcpy(rotated.corner[i], key.corner[(first + i) % 3], sizeof(key.corner[0]));
    }
    key = rotated;
}

// Every kept source triangle must come back out of the packed clusters bit-exactly.
static void validate_round_trip(ArenaAllocator<>& arena, const SourceCorner* corners, const i32* grid, const u32* indices,
                                size_t index_count, const MeshCluster* clusters, u32 cluster_count, const u32* triangles,
                                const MeshPosition* positions, const MeshAttribute* attributes, u32 triangle_total,
                                float inv_step)
{
    const size_t mark     = arena.buffer_offset;
    TriangleKey* expected = arena_push<TriangleKey>(arena, triangle_total);
    TriangleKey* actual   = arena_push<TriangleKey>(arena, triangle_total);
    if (!expected || !actual)
    {
        arena.buffer_offset = mark;
        return; // not enough room to check; the import itself is unaffected
    }

    u32 expected_count = 0;
    for (size_t t = 0; t < index_count; t += 3)
    {
        const u32 v[3] = {indices[t], indices[t + 1], indices[t + 2]};
        if (grid_equal(grid, v[0], v[1]) || grid_equal(grid, v[1], v[2]) || grid_equal(grid, v[0], v[2]))
        {
            continue;
        }
        assert(expected_count < triangle_total);
        TriangleKey& key = expected[expected_count++];
        for (u32 i = 0; i < 3; i++)
        {
            const SourceCorner& corner = corners[v[i]];
            const u32 words[6] = {(u32)grid[v[i] * 3], (u32)grid[v[i] * 3 + 1], (u32)grid[v[i] * 3 + 2], corner.normal,
                                  corner.tangent, corner.uv};
            memcpy(key.corner[i], words, sizeof(words));
        }
        canonicalize(key);
    }
    assert(expected_count == triangle_total);

    u32 actual_count = 0;
    for (u32 k = 0; k < cluster_count; k++)
    {
        const MeshCluster& cluster = clusters[k];
        const i32 anchor[3] = {(i32)(cluster.aabb_min.x * inv_step), (i32)(cluster.aabb_min.y * inv_step),
                               (i32)(cluster.aabb_min.z * inv_step)};
        const u32 vertex_offset   = cluster.vertices & 0xFFFFFFu;
        const u32 triangle_offset = cluster.triangles & 0xFFFFFFu;
        for (u32 t = 0; t < (cluster.triangles >> 24) + 1u; t++)
        {
            const u32    packed = triangles[triangle_offset + t];
            TriangleKey& key    = actual[actual_count++];
            for (u32 i = 0; i < 3; i++)
            {
                const u32            v = vertex_offset + ((packed >> (8 * i)) & 0xFFu);
                const MeshPosition&  p = positions[v];
                const MeshAttribute& a = attributes[v];
                const u32 offset[3] = {p.lo & 0x1FFFFFu, (p.lo >> 21) | ((p.hi & 0x3FFu) << 11), (p.hi >> 10) & 0x1FFFFFu};
                const u32 words[6] = {(u32)(anchor[0] + (i32)offset[0]), (u32)(anchor[1] + (i32)offset[1]),
                                      (u32)(anchor[2] + (i32)offset[2]), a.normal, a.tangent, a.uv};
                memcpy(key.corner[i], words, sizeof(words));
            }
            canonicalize(key);
        }
    }
    assert(actual_count == triangle_total);

    qsort(expected, triangle_total, sizeof(TriangleKey), compare_keys);
    qsort(actual, triangle_total, sizeof(TriangleKey), compare_keys);
    assert(memcmp(expected, actual, sizeof(TriangleKey) * triangle_total) == 0);

    arena.buffer_offset = mark;
}
#endif

// Stage 2: dedup, cluster, quantize and pack into a compact blob at the arena mark.
static Result build_clusters(ArenaAllocator<>& arena, const SourceCorners& source, const GltfImportSettings& settings,
                             size_t mark, ClusterMesh& out_mesh)
{
    SourceCorner* corners      = source.data;
    const size_t  corner_count = source.count;

    // Unindexed input: the remap table is the index buffer.
    u32* indices = arena_push<u32>(arena, corner_count);
    if (!indices)
    {
        return Result::out_of_memory;
    }
    const size_t vertex_count =
        meshopt_generateVertexRemap(indices, nullptr, corner_count, corners, corner_count, sizeof(SourceCorner));
    meshopt_remapVertexBuffer(corners, corners, corner_count, sizeof(SourceCorner), indices);

    // Degenerate and duplicate triangles, by position.
    const size_t index_count =
        meshopt_filterIndexBuffer(indices, indices, corner_count, corners, vertex_count, sizeof(float) * 3, sizeof(SourceCorner));
    if (index_count == 0)
    {
        return Result::no_geometry;
    }
    meshopt_optimizeVertexCache(indices, indices, index_count, vertex_count); // spatially coherent cluster order

    const size_t max_vertices  = settings.max_cluster_vertices;
    const size_t max_triangles = settings.max_cluster_triangles;
    const size_t min_triangles = max_triangles / 3 > 0 ? max_triangles / 3 : 1;

    const size_t     meshlet_bound     = meshopt_buildMeshletsBound(index_count, max_vertices, min_triangles);
    meshopt_Meshlet* meshlets          = arena_push<meshopt_Meshlet>(arena, meshlet_bound);
    u32*             meshlet_vertices  = arena_push<u32>(arena, index_count);
    u8*              meshlet_triangles = arena_push<u8>(arena, index_count);
    i32*             grid              = arena_push<i32>(arena, vertex_count * 3);
    if (!meshlets || !meshlet_vertices || !meshlet_triangles || !grid)
    {
        return Result::out_of_memory;
    }

    // Flex with split factor 2 keeps clusters spatially tight, which is what the per-cluster bounds are for.
    const size_t meshlet_count = meshopt_buildMeshletsFlex(
        meshlets, meshlet_vertices, meshlet_triangles, indices, index_count, corners[0].position, vertex_count,
        sizeof(SourceCorner), max_vertices, min_triangles, max_triangles, settings.cone_weight, 2.0f);

    // Optimize each cluster and find the mesh-wide grid that fits every cluster's offsets in 21 bits.
    int exponent = position_min_exponent;
    for (size_t k = 0; k < meshlet_count; k++)
    {
        meshopt_Meshlet& meshlet = meshlets[k];
        u32*             local_vertices  = &meshlet_vertices[meshlet.vertex_offset];
        u8*              local_triangles = &meshlet_triangles[meshlet.triangle_offset];
        meshopt_optimizeMeshletLevel(local_vertices, meshlet.vertex_count, local_triangles, meshlet.triangle_count, 3);

        // Used vertices end up in first-use order at the front.
        u32 used = 0;
        for (u32 i = 0; i < meshlet.triangle_count * 3; i++)
        {
            used = local_triangles[i] + 1u > used ? local_triangles[i] + 1u : used;
        }
        meshlet.vertex_count = used;

        float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (u32 i = 0; i < used; i++)
        {
            for (u32 a = 0; a < 3; a++)
            {
                lo[a] = fminf(lo[a], corners[local_vertices[i]].position[a]);
                hi[a] = fmaxf(hi[a], corners[local_vertices[i]].position[a]);
            }
        }
        const int cluster_exponent = meshopt_computePositionExponent(lo, hi, position_min_exponent, (int)mesh_position_bits);
        exponent                   = cluster_exponent > exponent ? cluster_exponent : exponent;
    }
    const float step     = ldexpf(1.0f, exponent);
    const float inv_step = ldexpf(1.0f, -exponent);

    // Snap clustered vertices onto the grid (exact: |q| < 2^24) and count the triangles that survive it.
    u32* kept_triangles = arena_push<u32>(arena, meshlet_count);
    if (!kept_triangles)
    {
        return Result::out_of_memory;
    }
    u64 vertex_total = 0, triangle_total = 0;
    u32 cluster_count = 0;
    for (size_t k = 0; k < meshlet_count; k++)
    {
        const meshopt_Meshlet& meshlet         = meshlets[k];
        const u32*             local_vertices  = &meshlet_vertices[meshlet.vertex_offset];
        const u8*              local_triangles = &meshlet_triangles[meshlet.triangle_offset];
        for (u32 i = 0; i < meshlet.vertex_count; i++)
        {
            const u32 v = local_vertices[i];
            for (u32 a = 0; a < 3; a++)
            {
                const float p = corners[v].position[a];
                const float q = roundf(p * inv_step);
                assert(fabsf(q * step - p) <= step * 0.5f);
                grid[v * 3 + a]       = (i32)q;
                corners[v].position[a] = q * step;
            }
        }

        u32 kept = 0;
        for (u32 t = 0; t < meshlet.triangle_count; t++)
        {
            kept += collapsed(grid, local_vertices, &local_triangles[t * 3]) ? 0u : 1u;
        }
        kept_triangles[k] = kept;
        if (kept > 0)
        {
            vertex_total += meshlet.vertex_count;
            triangle_total += kept;
            cluster_count++;
        }
    }
    if (cluster_count == 0)
    {
        return Result::no_geometry;
    }
    if (vertex_total > mesh_max_elements || triangle_total > mesh_max_elements) // 24-bit cluster offsets
    {
        return Result::unsupported;
    }

    const size_t clusters_offset   = 0;
    const size_t triangles_offset  = round_to<size_t>(clusters_offset + sizeof(MeshCluster) * cluster_count, blob_alignment);
    const size_t positions_offset  = round_to<size_t>(triangles_offset + sizeof(u32) * triangle_total, blob_alignment);
    const size_t attributes_offset = round_to<size_t>(positions_offset + sizeof(MeshPosition) * vertex_total, blob_alignment);
    const size_t blob_size         = round_to<size_t>(attributes_offset + sizeof(MeshAttribute) * vertex_total, blob_alignment);
    if (blob_size > (size_t)UINT32_MAX)
    {
        return Result::unsupported;
    }

    u8*     blob    = arena_push<u8>(arena, blob_size); // zero-filled, so padding stays zero
    float4* spheres = arena_push<float4>(arena, cluster_count);
    if (!blob || !spheres)
    {
        return Result::out_of_memory;
    }
    MeshCluster*   clusters   = (MeshCluster*)(blob + clusters_offset);
    u32*           triangles  = (u32*)(blob + triangles_offset);
    MeshPosition*  positions  = (MeshPosition*)(blob + positions_offset);
    MeshAttribute* attributes = (MeshAttribute*)(blob + attributes_offset);

    float3 mesh_min = {FLT_MAX, FLT_MAX, FLT_MAX};
    float3 mesh_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    u32    cluster_index = 0, vertex_cursor = 0, triangle_cursor = 0;
    for (size_t k = 0; k < meshlet_count; k++)
    {
        if (kept_triangles[k] == 0)
        {
            continue;
        }
        const meshopt_Meshlet& meshlet         = meshlets[k];
        const u32*             local_vertices  = &meshlet_vertices[meshlet.vertex_offset];
        const u8*              local_triangles = &meshlet_triangles[meshlet.triangle_offset];
        MeshCluster&           cluster         = clusters[cluster_index];

        i32 anchor[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
        i32 top[3]    = {INT32_MIN, INT32_MIN, INT32_MIN};
        for (u32 i = 0; i < meshlet.vertex_count; i++)
        {
            for (u32 a = 0; a < 3; a++)
            {
                const i32 q = grid[local_vertices[i] * 3 + a];
                anchor[a]   = q < anchor[a] ? q : anchor[a];
                top[a]      = q > top[a] ? q : top[a];
            }
        }
        // Exact box: both corners are grid points; the max corner is stored as a grid extent.
        const float3 aabb_max = {(float)top[0] * step, (float)top[1] * step, (float)top[2] * step};
        const u32    extent[3] = {(u32)(top[0] - anchor[0]), (u32)(top[1] - anchor[1]), (u32)(top[2] - anchor[2])};
        assert(extent[0] < (1u << mesh_position_bits) && extent[1] < (1u << mesh_position_bits) &&
               extent[2] < (1u << mesh_position_bits));
        cluster.aabb_min = {(float)anchor[0] * step, (float)anchor[1] * step, (float)anchor[2] * step};
        cluster.extent   = {.lo = extent[0] | extent[1] << 21, .hi = extent[1] >> 11 | extent[2] << 10};

        for (u32 i = 0; i < meshlet.vertex_count; i++)
        {
            const u32 v = local_vertices[i];
            const u32 x = (u32)(grid[v * 3 + 0] - anchor[0]);
            const u32 y = (u32)(grid[v * 3 + 1] - anchor[1]);
            const u32 z = (u32)(grid[v * 3 + 2] - anchor[2]);

            positions[vertex_cursor + i]  = {.lo = x | y << 21, .hi = y >> 11 | z << 10};
            attributes[vertex_cursor + i] = {.normal = corners[v].normal, .tangent = corners[v].tangent, .uv = corners[v].uv};
        }

        u32 kept = 0;
        for (u32 t = 0; t < meshlet.triangle_count; t++)
        {
            const u8* tri = &local_triangles[t * 3];
            if (!collapsed(grid, local_vertices, tri))
            {
                triangles[triangle_cursor + kept++] = (u32)tri[0] | (u32)tri[1] << 8 | (u32)tri[2] << 16;
            }
        }

        // Cone from the snapped positions; zero-area clusters get a cone that never culls.
        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(local_vertices, local_triangles, meshlet.triangle_count,
                                                                   corners[0].position, vertex_count, sizeof(SourceCorner));
        cluster.cone = bounds.radius > 0.0f ? (u32)(u8)bounds.cone_axis_s8[0] | (u32)(u8)bounds.cone_axis_s8[1] << 8 |
                                                  (u32)(u8)bounds.cone_axis_s8[2] << 16 | (u32)(u8)bounds.cone_cutoff_s8 << 24
                                            : mesh_no_cone;

        cluster.vertices  = vertex_cursor | (meshlet.vertex_count - 1) << 24;
        cluster.triangles = triangle_cursor | (kept - 1) << 24;

        // The shader's cluster sphere (box centre, half diagonal), radius rounded up, as input for the mesh sphere.
        const float3 centre = {(cluster.aabb_min.x + aabb_max.x) * 0.5f, (cluster.aabb_min.y + aabb_max.y) * 0.5f,
                               (cluster.aabb_min.z + aabb_max.z) * 0.5f};
        const double size[3] = {(double)aabb_max.x - cluster.aabb_min.x, (double)aabb_max.y - cluster.aabb_min.y,
                                (double)aabb_max.z - cluster.aabb_min.z};
        spheres[cluster_index] = {centre.x, centre.y, centre.z, nextafterf((float)(0.5 * sqrt(dot(size, size))), INFINITY)};

        mesh_min = {fminf(mesh_min.x, cluster.aabb_min.x), fminf(mesh_min.y, cluster.aabb_min.y), fminf(mesh_min.z, cluster.aabb_min.z)};
        mesh_max = {fmaxf(mesh_max.x, aabb_max.x), fmaxf(mesh_max.y, aabb_max.y), fmaxf(mesh_max.z, aabb_max.z)};

        vertex_cursor += meshlet.vertex_count;
        triangle_cursor += kept;
        cluster_index++;
    }
    assert(cluster_index == cluster_count && vertex_cursor == vertex_total && triangle_cursor == triangle_total);

    // meshopt's centre, but the radius recomputed in double: its float updates can miss far from the origin.
    const meshopt_Bounds mesh_sphere =
        meshopt_computeSphereBounds(&spheres[0].x, cluster_count, sizeof(float4), &spheres[0].w, sizeof(float4));
    double mesh_radius = 0.0;
    for (u32 k = 0; k < cluster_count; k++)
    {
        const double d[3] = {(double)spheres[k].x - mesh_sphere.center[0], (double)spheres[k].y - mesh_sphere.center[1],
                             (double)spheres[k].z - mesh_sphere.center[2]};
        mesh_radius       = fmax(mesh_radius, sqrt(dot(d, d)) + spheres[k].w);
    }

#if !defined(NDEBUG)
    validate_round_trip(arena, corners, grid, indices, index_count, clusters, cluster_count, triangles, positions, attributes,
                        (u32)triangle_total, inv_step);
#endif

    // Compact: move the blob down to the mark, dropping all scratch below it.
    u8* dst = (u8*)MemoryUtils::align_forward((uintptr_t)(arena.buffer + mark), blob_alignment);
    memmove(dst, blob, blob_size);
    arena.buffer_offset = (size_t)(dst - arena.buffer) + blob_size;

    out_mesh.blob       = {.data = dst, .num_elements = (u32)blob_size};
    out_mesh.clusters   = {.data = (MeshCluster*)(dst + clusters_offset), .num_elements = cluster_count};
    out_mesh.triangles  = {.data = (u32*)(dst + triangles_offset), .num_elements = (u32)triangle_total};
    out_mesh.positions  = {.data = (MeshPosition*)(dst + positions_offset), .num_elements = (u32)vertex_total};
    out_mesh.attributes = {.data = (MeshAttribute*)(dst + attributes_offset), .num_elements = (u32)vertex_total};
    out_mesh.aabb_min   = mesh_min;
    out_mesh.aabb_max   = mesh_max;
    out_mesh.sphere     = {mesh_sphere.center[0], mesh_sphere.center[1], mesh_sphere.center[2],
                           nextafterf((float)mesh_radius, INFINITY)};
    out_mesh.position_step         = step;
    out_mesh.position_exponent     = exponent;
    out_mesh.uv_min                = source.uv_min;
    out_mesh.uv_scale              = source.uv_scale;
    out_mesh.unique_vertex_count   = (u32)vertex_count;
    out_mesh.source_triangle_count = (u32)(corner_count / 3);
    return Result::ok;
}

GltfImportResult import_gltf(ArenaAllocator<>& arena, const char* filepath, ClusterMesh& out_mesh,
                             const GltfImportSettings& settings)
{
    assert(settings.max_cluster_vertices >= 3u && settings.max_cluster_vertices <= cluster_max_vertices);
    assert(settings.max_cluster_triangles >= 1u && settings.max_cluster_triangles <= cluster_max_triangles);
    assert(settings.cone_weight >= 0.0f && settings.cone_weight <= 1.0f);

    const size_t mark = arena.buffer_offset;
    out_mesh          = {};

    SourceCorners corners = {};
    Result        result  = gather_corners(arena, filepath, settings, corners);
    if (result == Result::ok)
    {
        result = build_clusters(arena, corners, settings, mark, out_mesh);
    }
    if (result != Result::ok)
    {
        arena.buffer_offset = mark;
        out_mesh            = {};
    }
    return result;
}
