#define _CRT_SECURE_NO_WARNINGS // plain fopen for shader files

#include "gfx/ShaderLibrary.hpp"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
enum class PsoKind : u8
{
    compute,
    graphics,
    mesh,
};

constexpr u32 max_stages        = 3u; // compute: [0]; graphics: vs, fs; mesh: task (optional), ms, fs
constexpr u32 max_color_targets = 8u;

// Everything needed to rebuild the PSO from its shader files.
struct PsoRecord
{
    gpu::PSO*                pso;
    PsoKind                  kind;
    ShaderRef                stages[max_stages]; // path == null: stage unused
    gpu::uint32x3            threadgroup_sizes[max_stages];
    u64                      hashes[max_stages]; // of the code the current PSO was built from
    gpu::ColorTargetDesc     color_targets[max_color_targets];
    u32                      color_target_count;
    gpu::Format              depth_format;
    gpu::Format              stencil_format;
    gpu::RasterizationState  rasterization;
};

// Compiled shader bytes in the library's scratch arena; an invalid handle means the stage is unused.
struct ShaderCode
{
    MemoryHandle memory;
    u64          hash;
};

constexpr u32 shader_code_alignment = 16u; // SPIR-V needs 4; metallib has no requirement

// FNV-1a: cheap and good enough to spot changed shader binaries.
u64 hash_bytes(const byte* data, size_t size)
{
    u64 hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; i++)
    {
        hash = (hash ^ data[i]) * 1099511628211ull;
    }
    return hash;
}

// Reads a compiled shader into `scratch`. Prints why on failure; memory taken before a failure is reclaimed when
// the owning LoadedStages rewinds the arena.
bool read_code(ArenaAllocator<>& scratch, const char* path, ShaderCode& code)
{
    code       = {};
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        printf("shader '%s' missing (declared with ALINE_SHADER? re-run premake5 ninja)\n", path);
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    const MemoryHandle memory =
        size > 0 ? scratch.allocate((size_t)size, {.ensure_contiguous_alloc = true, .alignment = shader_code_alignment})
                 : IAllocator::invalid_handle;
    byte* const data = static_cast<byte*>(scratch.handle_to_ptr(memory));
    const bool  ok   = data && fread(data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!ok)
    {
        if (size > 0 && !data)
        {
            printf("shader '%s' (%ld bytes) exceeds the shader scratch budget (%zu of %zu bytes free)\n", path, size,
                   scratch.buffer_len - scratch.buffer_offset, scratch.buffer_len);
        }
        else
        {
            printf("shader '%s' unreadable\n", path);
        }
        return false;
    }
    assert(((uintptr_t)data & (shader_code_alignment - 1u)) == 0u);
    code = {.memory = memory, .hash = hash_bytes(data, (size_t)size)};
    return true;
}

// Shader bytes for one PSO build. Rewinds the scratch arena on scope exit: PSO creation copies the code (Metal
// dispatch_data default destructor, vkCreateShaderModule), so the bytes are dead once build_pso returns.
struct LoadedStages
{
    explicit LoadedStages(ArenaAllocator<>& scratch) : scratch(scratch), mark(scratch.buffer_offset) {}
    ~LoadedStages() { scratch.buffer_offset = mark; }

    LoadedStages(const LoadedStages&)            = delete;
    LoadedStages& operator=(const LoadedStages&) = delete;

    ArenaAllocator<>& scratch;
    const size_t      mark;
    ShaderCode        code[max_stages] = {};
};

// Loads every used stage of `record`, stopping at the first failure (printed by read_code).
bool load_stages(const PsoRecord& record, LoadedStages& loaded)
{
    for (u32 i = 0; i < max_stages; i++)
    {
        const char* path = record.stages[i].path;
        if (path && !read_code(loaded.scratch, path, loaded.code[i]))
        {
            return false;
        }
    }
    return true;
}

gpu::ShaderStage shader_stage(const PsoRecord& record, const LoadedStages& loaded, u32 i)
{
    if (!record.stages[i].path)
    {
        return {}; // empty: stage omitted
    }
    const MemoryHandle& memory = loaded.code[i].memory;
    return {.code             = gpu::ByteSpan(static_cast<const byte*>(loaded.scratch.handle_to_ptr(memory)),
                                              (size_t)memory.size),
            .entry_point      = record.stages[i].entry,
            .threadgroup_size = record.threadgroup_sizes[i]};
}

gpu::PSO* build_pso(gpu::Device* device, const PsoRecord& record, const LoadedStages& loaded)
{
    const gpu::Span<const gpu::ColorTargetDesc> color_targets(record.color_targets, record.color_target_count);
    switch (record.kind)
    {
    case PsoKind::compute:
        return gpu::create_compute_pso(device, shader_stage(record, loaded, 0));
    case PsoKind::graphics:
        return gpu::create_graphics_pso(device, {
                                                    .vertex         = shader_stage(record, loaded, 0),
                                                    .fragment       = shader_stage(record, loaded, 1),
                                                    .color_targets  = color_targets,
                                                    .depth_format   = record.depth_format,
                                                    .stencil_format = record.stencil_format,
                                                    .rasterization  = record.rasterization,
                                                });
    case PsoKind::mesh:
        return gpu::create_mesh_pso(device, {
                                                .task           = shader_stage(record, loaded, 0),
                                                .mesh           = shader_stage(record, loaded, 1),
                                                .fragment       = shader_stage(record, loaded, 2),
                                                .color_targets  = color_targets,
                                                .depth_format   = record.depth_format,
                                                .stencil_format = record.stencil_format,
                                                .rasterization  = record.rasterization,
                                            });
    }
    return nullptr;
}

const char* first_stage_path(const PsoRecord& record)
{
    for (const ShaderRef& stage : record.stages)
    {
        if (stage.path)
        {
            return stage.path;
        }
    }
    return "?";
}

void copy_color_targets(PsoRecord& record, gpu::Span<const gpu::ColorTargetDesc> targets)
{
    assert(targets.size <= max_color_targets);
    record.color_target_count = (u32)targets.size;
    for (u32 i = 0; i < record.color_target_count; i++)
    {
        record.color_targets[i] = targets.data[i];
    }
}
} // namespace

struct ShaderLibrary
{
    ShaderLibrary(ArenaAllocator<>& arena, ArenaAllocator<>& scratch, gpu::Device* device, u32 max_psos)
        : device(device), scratch(scratch), psos(arena, max_psos)
    {
    }

    gpu::Device*               device;
    ArenaAllocator<>&          scratch; // caller-owned; compiled shader bytes during one PSO build
    Pool<PsoRecord, PsoHandle> psos;    // dead slots are zeroed: pso == null
};

ShaderLibrary* shader_library_create(ArenaAllocator<>& arena, ArenaAllocator<>& scratch, gpu::Device* device,
                                     u32 max_psos)
{
    assert(device && max_psos > 0u && max_psos <= PsoHandle::MAX_INDEX + 1u);
    ShaderLibrary*     library = nullptr;
    const MemoryHandle memory  = arena.create(&library, arena, scratch, device, max_psos);
    return memory.is_valid() ? library : nullptr;
}

void shader_library_destroy(ShaderLibrary* library)
{
    assert(library);
    for (PsoRecord& record : library->psos.objects)
    {
        if (record.pso)
        {
            gpu::destroy_pso(record.pso);
        }
    }
    // Owns nothing else: the pool lives in the creation arena and the scratch arena belongs to the caller.
}

static PsoHandle create_pso(ShaderLibrary* library, PsoRecord& record)
{
    {
        // Scoped so the scratch is rewound before the pool can grow, in case scratch and arena are the same.
        LoadedStages loaded(library->scratch);
        if (!load_stages(record, loaded))
        {
            return {};
        }
        record.pso = build_pso(library->device, record, loaded);
        if (!record.pso)
        {
            printf("PSO creation failed for shader '%s'\n", first_stage_path(record));
            return {};
        }
        for (u32 i = 0; i < max_stages; i++)
        {
            record.hashes[i] = loaded.code[i].hash;
        }
    }
    return library->psos.create(static_cast<PsoRecord&&>(record));
}

PsoHandle pso_create_compute(ShaderLibrary* library, const ShaderRef& cs, gpu::uint32x3 threadgroup_size)
{
    PsoRecord record            = {};
    record.kind                 = PsoKind::compute;
    record.stages[0]            = cs;
    record.threadgroup_sizes[0] = threadgroup_size;
    return create_pso(library, record);
}

PsoHandle pso_create_graphics(ShaderLibrary* library, const ShaderRef& vs, const ShaderRef& fs,
                              const gpu::GraphicsPSODesc& state)
{
    PsoRecord record      = {};
    record.kind           = PsoKind::graphics;
    record.stages[0]      = vs;
    record.stages[1]      = fs;
    record.depth_format   = state.depth_format;
    record.stencil_format = state.stencil_format;
    record.rasterization  = state.rasterization;
    copy_color_targets(record, state.color_targets);
    return create_pso(library, record);
}

PsoHandle pso_create_mesh(ShaderLibrary* library, const ShaderRef* task, gpu::uint32x3 task_threadgroup_size,
                          const ShaderRef& ms, gpu::uint32x3 mesh_threadgroup_size, const ShaderRef& fs,
                          const gpu::MeshPSODesc& state)
{
    PsoRecord record = {};
    record.kind      = PsoKind::mesh;
    if (task)
    {
        record.stages[0]            = *task;
        record.threadgroup_sizes[0] = task_threadgroup_size;
    }
    record.stages[1]            = ms;
    record.threadgroup_sizes[1] = mesh_threadgroup_size;
    record.stages[2]            = fs;
    record.depth_format         = state.depth_format;
    record.stencil_format       = state.stencil_format;
    record.rasterization        = state.rasterization;
    copy_color_targets(record, state.color_targets);
    return create_pso(library, record);
}

void pso_destroy(ShaderLibrary* library, PsoHandle pso)
{
    if (PsoRecord* record = library->psos.get(pso))
    {
        gpu::destroy_pso(record->pso);
        library->psos.destroy(pso);
    }
}

gpu::PSO* pso_get(const ShaderLibrary* library, PsoHandle pso)
{
    const PsoRecord* record = library->psos.get(pso);
    return record ? record->pso : nullptr;
}

#if defined(ALINE_BUILD_DIR) && defined(ALINE_NINJA)
// Compiles the shaders whose sources (or includes) changed; ninja prints its own progress and errors.
static bool build_shaders()
{
    const int status = system("\"" ALINE_NINJA "\" -C \"" ALINE_BUILD_DIR "\" shaders");
    fflush(stdout);
    return status == 0;
}
#else
static bool build_shaders() { return true; } // no build tree: only pick up already-compiled files
#endif

ShaderReloadResult shader_library_reload(ShaderLibrary* library)
{
    ShaderReloadResult result = {.build_ok = build_shaders(), .reloaded = 0u, .failed = 0u};
    bool               idle   = false;
    for (PsoRecord& record : library->psos.objects)
    {
        if (!record.pso)
        {
            continue; // free slot
        }
        LoadedStages loaded(library->scratch); // rewound at the end of every iteration
        if (!load_stages(record, loaded))
        {
            result.failed++;
            continue;
        }
        bool changed = false;
        for (u32 i = 0; i < max_stages; i++)
        {
            changed |= loaded.code[i].hash != record.hashes[i];
        }
        if (!changed)
        {
            continue;
        }

        gpu::PSO* const pso = build_pso(library->device, record, loaded);
        if (!pso)
        {
            printf("shader reload: PSO for '%s' failed, keeping the old one\n", first_stage_path(record));
            result.failed++;
            continue;
        }
        if (!idle)
        {
            gpu::wait_idle(library->device); // the old PSO may still be in use by frames in flight
            idle = true;
        }
        gpu::destroy_pso(record.pso);
        record.pso = pso;
        for (u32 i = 0; i < max_stages; i++)
        {
            record.hashes[i] = loaded.code[i].hash;
        }
        result.reloaded++;
    }
    return result;
}
