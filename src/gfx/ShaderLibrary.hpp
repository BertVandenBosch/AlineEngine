#pragma once

#include "core/Allocators.hpp"
#include "core/Pool.hpp"

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#if !defined(ALINE_SHADER_DIR) || !defined(ALINE_SHADER_EXTENSION)
#error "ALINE_SHADER_DIR and ALINE_SHADER_EXTENSION are defined by premake5.lua"
#endif

/*
 * Shaders are declared in C++ where they are used:
 *
 *   ALINE_SHADER(scene_scatter, "gfx/shaders/scene_scatter.slang", compute, "computeMain");
 *
 * premake5.lua scans src/ for these lines and generates one build edge per
 * declaration (build/shaders.ninja), so only declared shaders are compiled and
 * ninja tracks each one's includes via slangc's depfile. Re-run premake after
 * adding, removing or changing a declaration (as for adding a .cpp file).
 *
 * Rules for the scanner: one declaration per line, starting the line, with
 * literal arguments. `id` is unique engine-wide and names the output file;
 * `source` is relative to src/; `stage` is a slangc stage name (compute,
 * vertex, fragment, mesh, amplification).
 */
struct ShaderRef
{
    const char* path;  // compiled file: ALINE_SHADER_DIR/<id>ALINE_SHADER_EXTENSION
    const char* entry; // entry point name
};

#define ALINE_SHADER(id, source, stage, entry_point)                                                                   \
    inline constexpr ShaderRef id { ALINE_SHADER_DIR "/" #id ALINE_SHADER_EXTENSION, entry_point }

/*
 * ShaderLibrary owns PSOs built from declared shaders. Callers keep a
 * PsoHandle and resolve it with pso_get when binding, so a reload can swap the
 * PSO behind a handle.
 */
using PsoHandle = PoolHandle<u32, 16u, 16u, struct PsoTag>;

struct ShaderLibrary; // opaque

// Records live in `arena`. Compiled shader bytes are read into `scratch` during pso_create_* and
// shader_library_reload, which rewind it to its previous offset before returning, so it may be shared with other
// temporary data (or be `arena` itself). Its free space must hold all stages of one PSO. Both arenas must outlive
// the library. Null on failure.
[[nodiscard]] ShaderLibrary* shader_library_create(ArenaAllocator<>& arena, ArenaAllocator<>& scratch,
                                                   gpu::Device* device, u32 max_psos = 256u);
void shader_library_destroy(ShaderLibrary* library); // after gpu::wait_idle; destroys every remaining PSO

// Empty handle when a shader file is missing or PSO creation fails (details are printed).
// Fixed state is copied: color target spans need not outlive the call. Shader fields in `state` are ignored.
[[nodiscard]] PsoHandle pso_create_compute(ShaderLibrary* library, const ShaderRef& cs, gpu::uint32x3 threadgroup_size);
[[nodiscard]] PsoHandle pso_create_graphics(ShaderLibrary* library, const ShaderRef& vs, const ShaderRef& fs,
                                            const gpu::GraphicsPSODesc& state = {});
// task may be null; threadgroup sizes are for Metal (must match the compiled numthreads).
[[nodiscard]] PsoHandle pso_create_mesh(ShaderLibrary* library, const ShaderRef* task, gpu::uint32x3 task_threadgroup_size,
                                        const ShaderRef& ms, gpu::uint32x3 mesh_threadgroup_size, const ShaderRef& fs,
                                        const gpu::MeshPSODesc& state = {});
void pso_destroy(ShaderLibrary* library, PsoHandle pso); // after the GPU is done with it

[[nodiscard]] gpu::PSO* pso_get(const ShaderLibrary* library, PsoHandle pso); // null for a stale handle

struct ShaderReloadResult
{
    bool build_ok; // shader build succeeded (always true when built without ALINE_BUILD_DIR)
    u32  reloaded; // PSOs rebuilt with changed shaders
    u32  failed;   // PSOs whose changed shaders failed to load or link; they keep their old PSO
};

// Rebuilds changed shaders (`ninja shaders` in ALINE_BUILD_DIR, when defined), then recreates every PSO whose
// compiled shader bytes changed since it was built. Call between frames, outside command recording; waits for
// the GPU to go idle only when a PSO is replaced. A failed build still reloads whatever did compile.
ShaderReloadResult shader_library_reload(ShaderLibrary* library);
