-- AlineEngine build configuration
--
-- Generate the build files, then build with ninja:
--   premake5 ninja           generate build/build.ninja
--   ninja -C build Debug     build the debug target   (default)
--   ninja -C build Release   build the release target
--   ninja -C build -t clean  remove build artifacts
--
-- For editor IntelliSense (clangd), `premake5 ninja` also writes
-- compile_commands.json in the repo root, so there is nothing extra to run.
-- (Manual equivalent: ninja -C build -t compdb cc_clang cxx_clang > compile_commands.json)
--
-- Other generators work too, e.g. `premake5 vs2022` for a Visual Studio
-- solution.
--
-- Third-party code lives in submodules (run `git submodule update --init`
-- after cloning), each compiled by its own project below as a static library
-- -- no CMake involved:
--   src/NoGraphicsAPI   rendering backend ("NoGraphicsAPI" project).
--   src/meshoptimizer   mesh processing ("meshoptimizer" project).
--   src/tinygltf        glTF 2.0 loading/saving, v3 C API ("tinygltf" project).
--
-- Platforms (NoGraphicsAPI picks the backend):
--   Windows  Vulkan 1.4. Needs a Vulkan SDK with headers >= 1.4.357, found
--            via VULKAN_SDK or C:/VulkanSDK; its slangc/spirv-val compile the
--            example shaders.
--   macOS    Metal 4: Apple silicon, macOS 26+, Xcode 26+. Run premake on the
--            Mac itself: the generated files contain absolute paths. The
--            example shaders also need Xcode's Metal toolchain (`xcodebuild
--            -downloadComponent MetalToolchain`) and slangc >= 2026.18.2 on
--            PATH (or SLANGC=/path/to/slangc when running premake); without
--            them premake skips the examples and the engine still builds.
--
-- To add a source folder, a dependency or a
-- new configuration, edit the clearly marked tables -- nothing else should need
-- to change.
--
-- Note: the `do ... end` blocks are plain Lua blocks with no effect on premake
-- (its workspace/project/filter state is global). They exist only so Lua code
-- formatters keep this file's indentation instead of flattening it.

-- ---------------------------------------------------------------------------
-- Fix: header-change detection for the built-in `ninja` action
--
-- premake's bundled ninja action declares `depfile = $out.d` + `deps = gcc`
-- on the compile rules but forgets to actually emit the dependency file, so
-- clang never writes one and ninja can't tell that a .cpp depends on a header.
-- Editing a .hpp then never triggers a rebuild.
--
-- We patch the generated cc/cxx rules to add `-MMD -MF $out.d`, which writes
-- the depfile exactly where ninja looks for it. This lives in the rule command
-- (not buildoptions) because only there is ninja's $out variable in scope.
-- Harmless no-op for every other action (vs2022, etc.).
-- ---------------------------------------------------------------------------

if premake.modules.ninja then
	local cpp = premake.modules.ninja.cpp
	local unpack = table.unpack or unpack
	local function inject_depfile(base, ...)
		local n = select("#", ...)
		local args = { ... }
		local rule = premake.capture(function() base(unpack(args, 1, n)) end)
		rule = rule:gsub("(%-c %$in %-o %$out)", "-MMD -MF $out.d %1", 1)
		premake.outln((rule:gsub("%s+$", "")))
	end
	premake.override(cpp, "ccrule", inject_depfile)
	premake.override(cpp, "cxxrule", inject_depfile)
end

-- ---------------------------------------------------------------------------
-- clangd support: compile_commands.json
--
-- After the ninja files are written, ask ninja for the compilation database and
-- drop it in the repo root, where clangd finds it for every file under src/
-- (including the NoGraphicsAPI submodule). Only the cc/cxx rules are requested;
-- without them ninja also lists link/archive/custom steps as bogus entries.
-- Debug is emitted before Release, and clangd uses the first match per file.
-- ---------------------------------------------------------------------------

local COMPDB_COMMAND = "ninja -C build -t compdb cc_clang cxx_clang > compile_commands.json"

local ninja_action = premake.action.get("ninja")
if ninja_action then
	local base_on_end = ninja_action.onEnd
	ninja_action.onEnd = function(...)
		if base_on_end then
			base_on_end(...)
		end
		if os.execute(COMPDB_COMMAND) then
			print("Generated compile_commands.json")
		else
			print("Warning: could not generate compile_commands.json for clangd; run `" .. COMPDB_COMMAND .. "` manually")
		end
	end
end

-- ---------------------------------------------------------------------------
-- Target platform
--
-- macOS builds NoGraphicsAPI's Metal 4 backend; every other target uses its
-- Vulkan backend. Decided once at load time, so Vulkan-only paths are never
-- evaluated for a Mac.
-- ---------------------------------------------------------------------------

local METAL = os.istarget("macosx")
local MACOS_VERSION = "26.0" -- the Metal backend requires macOS 26

-- math.hpp needs AVX2 + FMA on x86-64; Apple silicon always has NEON + FMA.
local SIMD_FLAGS = METAL and {} or { "-mavx2", "-mfma" }

-- On Windows, static libraries are archived with `ar`, which is not
-- installed there -- LLVM's drop-in replacement is. macOS has `ar`.
if os.istarget("windows") and premake.tools.clang and premake.tools.clang.tools then
	premake.tools.clang.tools.ar = "llvm-ar"
end

-- ---------------------------------------------------------------------------
-- Vulkan SDK lookup (Vulkan targets only)
--
-- The NoGraphicsAPI backend needs Vulkan headers >= 1.4.357. Prefer the
-- VULKAN_SDK environment variable, but fall back to the newest SDK under
-- C:/VulkanSDK that is recent enough (VULKAN_SDK may point at an older one).
-- ---------------------------------------------------------------------------

local MIN_VK_HEADER_VERSION = 357

local function vk_header_version(sdk)
	local f = io.open(sdk .. "/Include/vulkan/vulkan_core.h", "r")
	if not f then
		return nil
	end
	local text = f:read("*a")
	f:close()
	return tonumber(text:match("#define%s+VK_HEADER_VERSION%s+(%d+)"))
end

local function find_vulkan_sdk()
	local env = os.getenv("VULKAN_SDK")
	if env then
		env = env:gsub("\\", "/")
		if (vk_header_version(env) or 0) >= MIN_VK_HEADER_VERSION then
			return env
		end
	end

	local best, best_version = nil, MIN_VK_HEADER_VERSION - 1
	for _, dir in ipairs(os.matchdirs("C:/VulkanSDK/*")) do
		local version = vk_header_version(dir) or 0
		if version > best_version then
			best, best_version = dir, version
		end
	end
	if best then
		return best
	end

	error("NoGraphicsAPI needs a Vulkan SDK with headers >= 1.4." .. MIN_VK_HEADER_VERSION ..
		" (VULKAN_SDK=" .. tostring(env) .. "); install a newer one from https://vulkan.lunarg.com")
end

local VULKAN_SDK_DIR = not METAL and find_vulkan_sdk() or nil

-- Compile settings for code using the graphics backend: the Vulkan headers on
-- Vulkan targets; on macOS, Objective-C++ (.mm) sources use manual
-- retain/release, as in NoGraphicsAPI's CMake. (Plain `files` is enough for
-- .mm: the ninja action compiles them with clang++, which picks the language
-- from the extension. `compileas "Objective-C++"` would drop them instead.)
local function backend_compile_settings()
	if METAL then
		filter "files:**.mm"
		buildoptions { "-fno-objc-arc" }
		filter {}
	else
		includedirs { VULKAN_SDK_DIR .. "/Include" }
	end
end

-- System libraries an executable using NoGraphicsAPI must link; premake does
-- not pass them on from the static library.
local function backend_links()
	if METAL then
		links { "Metal.framework", "Foundation.framework", "QuartzCore.framework" }
	else
		libdirs { VULKAN_SDK_DIR .. "/Lib" }
		links { "vulkan-1" }
	end
end

-- ---------------------------------------------------------------------------
-- Workspace
-- ---------------------------------------------------------------------------

workspace "AlineEngine"
do
	configurations { "Debug", "Release" }
	location "build" -- generated project/make files live here
	toolset "clang" -- compile with clang++

	-- Metal 4 needs Apple silicon: pin arm64, or a shell running under Rosetta
	-- would build x86_64 (and math.hpp would then demand AVX2). Same
	-- deployment target for every object and the link, or ld warns about
	-- objects built for a newer macOS (systemversion only reaches the compile
	-- flags).
	if METAL then
		architecture "ARM64"
		systemversion(MACOS_VERSION)
		linkoptions { "-mmacosx-version-min=" .. MACOS_VERSION }
	end
end

-- Rendering backend (src/NoGraphicsAPI submodule), built from source as a
-- static library and linked into the engine. Mirrors the submodule's
-- CMakeLists.txt, which is not used. Only the backend for the target (Vulkan,
-- or Metal on macOS) is compiled; examples and tests are left out.
project "NoGraphicsAPI"
do
	kind "StaticLib"
	language "C++"

	targetdir "build/lib/%{cfg.buildcfg}"
	objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

	files {
		METAL and "src/NoGraphicsAPI/src/NoGraphicsAPIMetal.mm" or "src/NoGraphicsAPI/src/NoGraphicsAPI.cpp",
		"src/NoGraphicsAPI/include/**.h",
		"src/NoGraphicsAPI/include/**.hpp",
	}

	includedirs {
		"src/NoGraphicsAPI/include",
	}
	backend_compile_settings()

	-- The backend is written for C++20 with exceptions and RTTI disabled.
	buildoptions {
		"-std=c++20",
		"-fno-exceptions",
		"-fno-rtti",
		"-Wall",
		"-Wextra",
		"-Wpedantic",
		"-Wconversion",
		"-Wshadow",
		"-Wno-missing-field-initializers",
		"-fdiagnostics-absolute-paths",
	}

	filter "configurations:Debug"
	do
		defines { "DEBUG" }
		symbols "On"
		optimize "Off"
	end

	filter "configurations:Release"
	do
		defines { "NDEBUG" }
		optimize "On"
	end

	filter {} -- clear the active filter
end

-- Optional helpers shipped with the backend (allocators, delete/upload queues,
-- texture upload, SIMD math). One static library instead of the CMake
-- allocators/textures/uploads split. On x86-64, anything including math.hpp
-- needs -mavx2 -mfma (SIMD_FLAGS), so the engine passes them too.
project "NoGraphicsAPIUtility"
do
	kind "StaticLib"
	language "C++"

	targetdir "build/lib/%{cfg.buildcfg}"
	objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

	files {
		"src/NoGraphicsAPI/utility/src/**.cpp",
		"src/NoGraphicsAPI/utility/src/**.hpp",
		"src/NoGraphicsAPI/utility/include/**.hpp",
		"src/NoGraphicsAPI/utility/include/**.h",
	}

	includedirs {
		"src/NoGraphicsAPI/utility/include",
		"src/NoGraphicsAPI/include",
	}
	backend_compile_settings()

	buildoptions {
		"-std=c++20",
		"-fno-exceptions",
		"-fno-rtti",
	}
	buildoptions(SIMD_FLAGS)
	buildoptions {
		"-Wall",
		"-Wextra",
		"-Wpedantic",
		"-Wconversion",
		"-Wshadow",
		"-fdiagnostics-absolute-paths",
	}

	links { "NoGraphicsAPI" }

	filter "configurations:Debug"
	do
		defines { "DEBUG" }
		symbols "On"
		optimize "Off"
	end

	filter "configurations:Release"
	do
		defines { "NDEBUG" }
		optimize "On"
	end

	filter {} -- clear the active filter
end

-- Mesh processing library (src/meshoptimizer submodule), built from source as
-- a static library and linked into the engine. Mirrors the submodule's
-- CMakeLists.txt (sources and warning flags), which is not used. Only the
-- library in src/ is compiled; demo/, gltf/ (gltfpack), tools/ and js/ are
-- left out. No -std: the library is plain C++98.
project "meshoptimizer"
do
	kind "StaticLib"
	language "C++"

	targetdir "build/lib/%{cfg.buildcfg}"
	objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

	files {
		"src/meshoptimizer/src/*.cpp",
		"src/meshoptimizer/src/*.h",
	}

	includedirs {
		"src/meshoptimizer/src",
	}

	buildoptions {
		"-fno-exceptions",
		"-fno-rtti",
		"-Wall",
		"-Wextra",
		"-Wshadow",
		"-Wno-missing-field-initializers",
		"-fdiagnostics-absolute-paths",
	}

	filter "configurations:Debug"
	do
		defines { "DEBUG" }
		symbols "On"
		optimize "Off"
	end

	filter "configurations:Release"
	do
		defines { "NDEBUG" }
		optimize "On"
	end

	filter {} -- clear the active filter
end

-- glTF 2.0 loader/saver (src/tinygltf submodule, v3 C API), built from source
-- as a static C library and linked into the engine. The whole runtime is
-- tiny_gltf_v3.c (C11) plus its two headers; the legacy C++ API in attic/,
-- tests/, benchmark/ and web/ are left out.
-- TINYGLTF3_ENABLE_FS enables the built-in stdio file callbacks, so
-- tg3_parse_file() and external .bin/image URIs work without custom
-- callbacks (custom tg3_fs callbacks still take precedence). It only affects
-- the .c file, so the engine does not need it. _CRT_SECURE_NO_WARNINGS
-- silences the MSVC CRT's fopen deprecation warnings in those callbacks.
project "tinygltf"
do
	kind "StaticLib"
	language "C"

	targetdir "build/lib/%{cfg.buildcfg}"
	objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

	files {
		"src/tinygltf/tiny_gltf_v3.c",
		"src/tinygltf/tiny_gltf_v3.h",
		"src/tinygltf/tinygltf_json_c.h",
	}

	includedirs {
		"src/tinygltf",
	}

	defines { "TINYGLTF3_ENABLE_FS", "_CRT_SECURE_NO_WARNINGS" }

	buildoptions {
		"-std=c11",
		"-Wall",
		"-Wextra",
		"-fdiagnostics-absolute-paths",
	}

	filter "configurations:Debug"
	do
		defines { "DEBUG" }
		symbols "On"
		optimize "Off"
	end

	filter "configurations:Release"
	do
		defines { "NDEBUG" }
		optimize "On"
	end

	filter {} -- clear the active filter
end

-- Backend examples (src/NoGraphicsAPI/examples/<dir>), built here as smoke
-- tests for the window + rendering path, with the same shader options as the
-- examples' CMake rules. Vulkan: slangc compiles each stage to SPIR-V, which
-- spirv-val validates. macOS: slangc emits Metal source, which Xcode's metal
-- and metallib turn into a .metallib.
-- Run them with: build/examples/Example<Name>_debug(.exe)
--
-- `opts`: shader (basename of <dir>/<shader>.slang), define_prefix (the
-- *_VERTEX/FRAGMENT_SHADER_PATH define prefix), utility (link the utility lib
-- and use SIMD_FLAGS), defines, extra_files, extra_inputs.
local EXAMPLES = "src/NoGraphicsAPI/examples"
local SHADER_DIR = path.getabsolute("build/shaders")

-- Paths in shader commands. Windows: unquoted, as the ninja action runs them
-- through `cmd /C` and escapes quotes in a way cmd.exe cannot parse (none of
-- the paths contain spaces). macOS: double-quoted, inside the ninja action's
-- `sh -c '...'` wrapper, so a `'` in a path must also close and reopen that.
local function shell_path(p)
	return METAL and ('"' .. p:gsub("'", "'\\''") .. '"') or p
end

-- macOS slangc: SLANGC or PATH. A relative SLANGC is made absolute because
-- ninja runs the commands from inside build/.
local SLANGC_MACOS = os.getenv("SLANGC") or "slangc"
if SLANGC_MACOS:find("/") then
	SLANGC_MACOS = path.getabsolute(SLANGC_MACOS)
end
local SLANGC = METAL and shell_path(SLANGC_MACOS) or path.translate(VULKAN_SDK_DIR .. "/Bin/slangc.exe")

-- On a Mac the shaders need slangc >= 2026.18.2 (the minimum NoGraphicsAPI's
-- CMake enforces) and Xcode's Metal toolchain. Checked when premake runs, so a
-- missing tool is reported up front instead of halfway through the build.
-- Only probed when generating on the Mac itself.
local SLANG_MIN_VERSION = { 2026, 18, 2 }

local function check_shader_tools()
	if not (METAL and os.ishost("macosx")) then
		return
	end
	local quoted = "'" .. SLANGC_MACOS:gsub("'", "'\\''") .. "'" -- run directly by the shell, not via ninja
	-- Some releases print only major.minor (e.g. "2026.21"); a missing patch counts as 0.
	local major, minor, patch = (os.outputof(quoted .. " -version 2>&1") or ""):match("(%d+)%.(%d+)%.?(%d*)")
	local version = major and { major, minor, patch ~= "" and patch or "0" } or {}
	local recent = false
	for i = 1, 3 do
		local part = tonumber(version[i] or -1)
		if part ~= SLANG_MIN_VERSION[i] then
			recent = part > SLANG_MIN_VERSION[i]
			break
		end
		recent = i == 3
	end
	if not recent then
		error("the shaders need slangc >= " .. table.concat(SLANG_MIN_VERSION, ".") ..
			" (on PATH or via SLANGC), found " .. (version[1] and table.concat(version, ".") or "none"))
	end
	if not os.execute("xcrun -sdk macosx metal --version >/dev/null 2>&1") then
		error("the shaders need Xcode's Metal toolchain (install it with `xcodebuild -downloadComponent MetalToolchain`)")
	end
end

check_shader_tools()

-- Commands compiling one Slang entry point for the target backend, with the
-- same options as NoGraphicsAPI's CMake rules. `source`, `include_dirs` and
-- `out` are absolute paths.
local function slang_compile(source, include_dirs, stage, entry, out)
	local slang = SLANGC .. " " .. shell_path(source)
	local common = " -entry " .. entry .. " -stage " .. stage .. " -fvk-use-c-layout -matrix-layout-row-major"
	for _, dir in ipairs(include_dirs) do
		common = common .. " -I " .. shell_path(dir)
	end
	if METAL then
		return {
			slang .. " -target metal -DNOGRAPHICSAPI_METAL" .. common .. " -o " .. shell_path(out .. ".metal"),
			"xcrun -sdk macosx metal -std=metal4.0 -target air64-apple-macos" .. MACOS_VERSION ..
				" -c " .. shell_path(out .. ".metal") .. " -o " .. shell_path(out .. ".air"),
			"xcrun -sdk macosx metallib " .. shell_path(out .. ".air") .. " -o " .. shell_path(out),
		}
	end
	return {
		slang .. " -target spirv -profile spirv_1_5 -emit-spirv-directly -fvk-use-entrypoint-name" .. common ..
			" -capability spvDescriptorHeapEXT -o " .. out,
		path.translate(VULKAN_SDK_DIR .. "/Bin/spirv-val.exe") .. " --target-env vulkan1.4 --scalar-block-layout " .. out,
	}
end

local function example_project(name, dir, opts)
	local shader = opts.shader
	local extension = METAL and ".metallib" or ".spv"
	local vertex_out = SHADER_DIR .. "/" .. shader .. ".vert" .. extension
	local fragment_out = SHADER_DIR .. "/" .. shader .. ".frag" .. extension
	-- Absolute paths: ninja runs the commands from inside build/.
	local root = path.getabsolute(".")
	local src_dir = EXAMPLES .. "/" .. dir

	project("Example" .. name)
	do
		kind "ConsoleApp"
		language "C++"

		targetdir "build/examples"
		objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

		files {
			src_dir .. "/" .. dir .. ".cpp",
			src_dir .. "/" .. shader .. ".slang",
			EXAMPLES .. "/common/example_support.cpp",
			EXAMPLES .. "/common/" .. (METAL and "example_window_macos.mm" or "example_window_win32.cpp"),
		}
		files(opts.extra_files or {})

		includedirs {
			src_dir,
			EXAMPLES .. "/common",
			"src/NoGraphicsAPI/include",
		}
		backend_compile_settings()

		if not METAL then
			defines { "_CRT_SECURE_NO_WARNINGS" }
		end
		defines {
			opts.define_prefix .. '_VERTEX_SHADER_PATH="' .. vertex_out .. '"',
			opts.define_prefix .. '_FRAGMENT_SHADER_PATH="' .. fragment_out .. '"',
		}
		defines(opts.defines or {})

		buildoptions {
			"-std=c++20",
			"-fno-exceptions",
			"-fno-rtti",
			"-Wall",
			"-fdiagnostics-absolute-paths",
		}

		if opts.utility then
			includedirs { "src/NoGraphicsAPI/utility/include" }
			buildoptions(SIMD_FLAGS)
			links { "NoGraphicsAPIUtility" }
		end
		links { "NoGraphicsAPI" }
		backend_links()
		links { METAL and "AppKit.framework" or "user32" } -- the example window

		local function compile_shader(stage, entry, out)
			return slang_compile(root .. "/" .. src_dir .. "/" .. shader .. ".slang", {
				root .. "/" .. src_dir,
				root .. "/src/NoGraphicsAPI/include",
				root .. "/src/NoGraphicsAPI/utility/include",
			}, stage, entry, out)
		end
		-- Custom build rule on the .slang file. (prebuildcommands is avoided: on
		-- Windows the ninja action appends stamp-file commands to it with `\"`
		-- quoting that cmd.exe cannot parse.)
		filter "files:**.slang"
		do
			buildmessage("Compiling " .. shader .. ".slang to " .. (METAL and "metallib" or "SPIR-V"))
			buildcommands(compile_shader("vertex", "vertexMain", vertex_out))
			buildcommands(compile_shader("fragment", "fragmentMain", fragment_out))
			buildoutputs { vertex_out, fragment_out }
			buildinputs {
				root .. "/src/NoGraphicsAPI/include/NoGraphicsAPI/shader.slang",
				root .. "/src/NoGraphicsAPI/include/NoGraphicsAPI/types.h",
				root .. "/src/NoGraphicsAPI/utility/include/NoGraphicsAPIUtility/shader_types.h",
			}
			buildinputs(opts.extra_inputs or {})
		end
		filter {}

		filter "configurations:Debug"
		do
			defines { "DEBUG" }
			symbols "On"
			optimize "Off"
			targetname("Example" .. name .. "_debug")
		end

		filter "configurations:Release"
		do
			defines { "NDEBUG" }
			optimize "On"
			targetname("Example" .. name .. "_release")
		end

		filter {} -- clear the active filter
	end
end

example_project("Triangle", "triangle", {
	shader = "triangle",
	define_prefix = "NOGRAPHICSAPI",
})

example_project("Cube", "cube", {
	shader = "cube",
	define_prefix = "NOGRAPHICSAPI_CUBE",
	utility = true,
	defines = { 'NOGRAPHICSAPI_CUBE_TEXTURE_PATH="' .. path.getabsolute(EXAMPLES .. "/cube/lunarg_logo_256x256.rgba8") .. '"' },
	extra_files = { EXAMPLES .. "/cube/cube_shared.h" },
	extra_inputs = { path.getabsolute(EXAMPLES .. "/cube/cube_shared.h") },
})

project "AlineEngine"
do
	kind "ConsoleApp"
	language "C++"

	targetdir "build"
	objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

	-- Source files ------------------------------------------------------
	-- `**` recurses, so every .cpp/.hpp under src/ is picked up automatically;
	-- just drop new files in and re-run premake. (Headers are listed only so
	-- they appear in IDE projects -- only the .cpp files are compiled.)
	-- The NoGraphicsAPI, meshoptimizer and tinygltf submodules are built by
	-- their own projects above, so they must not be globbed in here (all
	-- contain tests/examples/tools with a main()).
	files {
		"src/**.cpp",
		"src/**.hpp",
		"src/**.h", -- C++/Slang shared headers
	}
	removefiles {
		"src/NoGraphicsAPI/**",
		"src/meshoptimizer/**",
		"src/tinygltf/**",
	}

	-- Repository root, so tests can open the sample models in the submodules.
	defines { 'ALINE_ROOT_DIR="' .. path.getabsolute(".") .. '"' }

	-- Engine shaders (src/gfx/shaders/*.slang): compute entry point
	-- `computeMain`, compiled to build/shaders/<name>.spv (Vulkan) or .metallib
	-- (macOS). Code loads them from ALINE_SHADER_DIR "/<name>" ALINE_SHADER_EXTENSION.
	local shader_extension = METAL and ".metallib" or ".spv"
	files { "src/gfx/shaders/*.slang" }
	defines {
		'ALINE_SHADER_DIR="' .. SHADER_DIR .. '"',
		'ALINE_SHADER_EXTENSION="' .. shader_extension .. '"',
	}
	filter "files:src/gfx/shaders/*.slang"
	do
		local root = path.getabsolute(".")
		local out = SHADER_DIR .. "/%{file.basename}" .. shader_extension
		buildmessage "Compiling %{file.name}"
		buildcommands(slang_compile("%{file.abspath}", {
			root .. "/src",
			root .. "/src/NoGraphicsAPI/include",
			root .. "/src/NoGraphicsAPI/utility/include",
		}, "compute", "computeMain", out))
		buildoutputs { out }
		buildinputs {
			root .. "/src/gfx/scene_shared.h",
			root .. "/src/gfx/cluster_mesh_shared.h",
			root .. "/src/NoGraphicsAPI/include/NoGraphicsAPI/shader.slang",
			root .. "/src/NoGraphicsAPI/include/NoGraphicsAPI/types.h",
			root .. "/src/NoGraphicsAPI/utility/include/NoGraphicsAPIUtility/shader_types.h",
		}
	end
	filter {}

	-- Header search paths -----------------------------------------------
	includedirs {
		"src",
		"src/NoGraphicsAPI/include",
		"src/NoGraphicsAPI/utility/include",
		"src/meshoptimizer/src",
		"src/tinygltf",
	}
	backend_compile_settings()

	-- Libraries ---------------------------------------------------------
	-- NoGraphicsAPI, meshoptimizer and tinygltf are linked statically, plus
	-- the backend's system libraries: on Vulkan targets vulkan-1, the loader's
	-- import library (the loader DLL itself ships with the GPU driver); on
	-- macOS the Metal, Foundation and QuartzCore frameworks.
	links {
		"NoGraphicsAPIUtility",
		"NoGraphicsAPI",
		"meshoptimizer",
		"tinygltf",
	}
	backend_links()

	-- C++26 is requested explicitly so the exact clang flag is used
	-- regardless of the premake version's cppdialect support.
	-- -fdiagnostics-absolute-paths makes clang print absolute file paths in
	-- warnings/errors, so terminal links work even though ninja builds from
	-- inside build/.
	-- SIMD_FLAGS (-mavx2 -mfma on x86-64) are required by NoGraphicsAPIUtility/math.hpp.
	buildoptions { "-std=c++26" }
	buildoptions(SIMD_FLAGS)
	buildoptions { "-Wall", "-fdiagnostics-absolute-paths" }

	-- Configurations ----------------------------------------------------
	filter "configurations:Debug"
	do
		defines { "DEBUG" }
		symbols "On"
		optimize "Off"
		targetname "AlineEngine_debug"
	end

	filter "configurations:Release"
	do
		defines { "NDEBUG" }
		optimize "On"
		targetname "AlineEngine_release"
	end

	filter {} -- clear the active filter
end
