#pragma once

#include "core/core.hpp"
#include "gfx/cluster_mesh_shared.h"

#include <math.h>

/*
 * Discrete per-mesh LOD selection: each LOD's object-space error is projected
 * to pixels at the instance's nearest possible distance, and the coarsest LOD
 * under the threshold wins. Plain arithmetic so it ports 1:1 to Slang: the
 * GPU cull pass must use the same formula -- keep the two in sync.
 */

struct LodView
{
    float3 camera_position;  // world space
    float  projection_scale; // viewport_height_px / (2 * tan(fov_y / 2))
    float  threshold_px;     // largest acceptable projected error, e.g. 1
};

[[nodiscard]] inline u32 select_mesh_lod(const GpuMesh& mesh, const float3x4& world, const LodView& view)
{
    // Largest axis scale of the instance: errors and the radius grow with it.
    float max_scale_sq = 0.0f;
    for (u32 c = 0; c < 3; c++)
    {
        const float x = world.rows[0][c], y = world.rows[1][c], z = world.rows[2][c];
        const float length_sq = x * x + y * y + z * z;
        max_scale_sq          = length_sq > max_scale_sq ? length_sq : max_scale_sq;
    }
    const float scale = sqrtf(max_scale_sq);

    float offset[3];
    for (u32 r = 0; r < 3; r++)
    {
        const float4& row = world.rows[r];
        offset[r] = row.x * mesh.sphere.x + row.y * mesh.sphere.y + row.z * mesh.sphere.z + row.w - view.camera_position[r];
    }
    const float distance = sqrtf(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]) - mesh.sphere.w * scale;
    if (distance <= 0.0f)
    {
        return 0u; // camera inside the bounds
    }

    const float px_per_unit = scale * view.projection_scale / distance;
    for (u32 lod = mesh.lod_count; lod-- > 1u;)
    {
        if (mesh.lods[lod].error * px_per_unit <= view.threshold_px)
        {
            return lod;
        }
    }
    return 0u;
}
