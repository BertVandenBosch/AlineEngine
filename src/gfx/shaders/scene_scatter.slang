#include <NoGraphicsAPI/shader.slang>
#include "gfx/scene_shared.h"

GPU_ROOT(SceneScatterRoot, root);

// Applies this frame's transform and static mesh changes to the persistent scene arrays. Each slot appears at
// most once per frame, so threads never write the same element.
[shader("compute")]
[numthreads(scene_scatter_group_size, 1, 1)]
void computeMain(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    uint i = dispatch_thread_id.x;
    if (i < root.transform_count)
    {
        root.transforms[root.transform_indices[i]] = root.transform_values[i];
    }
    if (i < root.static_mesh_count)
    {
        root.static_meshes[root.static_mesh_indices[i]] = root.static_mesh_values[i];
    }
}
