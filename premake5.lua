-- AlineEngine build configuration
--
-- Generate the build files, then build with ninja:
--   premake5 ninja           generate build/build.ninja
--   ninja -C build Debug     build the debug target   (default)
--   ninja -C build Release   build the release target
--   ninja -C build -t clean  remove build artifacts
--
-- For editor IntelliSense, generate a compilation database:
--   ninja -C build -t compdb > compile_commands.json
--
-- Other generators work too, e.g. `premake5 vs2022` for a Visual Studio
-- solution.
--
-- The rendering backend (src/NoGraphicsAPI submodule, run
-- `git submodule update --init` after cloning) is compiled by the
-- "NoGraphicsAPI" project below as a static library -- no CMake involved. It
-- needs a Vulkan SDK with headers >= 1.4.357, found via VULKAN_SDK or
-- C:/VulkanSDK.
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

-- Static libraries are archived with `ar`, which is not installed here --
-- LLVM's drop-in replacement is.
if premake.tools.clang and premake.tools.clang.tools then
	premake.tools.clang.tools.ar = "llvm-ar"
end

-- ---------------------------------------------------------------------------
-- Vulkan SDK lookup
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

local VULKAN_SDK_DIR = find_vulkan_sdk()

-- ---------------------------------------------------------------------------
-- Workspace
-- ---------------------------------------------------------------------------

workspace "AlineEngine"
do
	configurations { "Debug", "Release" }
	location "build" -- generated project/make files live here
	toolset "clang" -- compile with clang++
end

-- Rendering backend (src/NoGraphicsAPI submodule), built from source as a
-- static library and linked into the engine. Mirrors the submodule's
-- CMakeLists.txt, which is not used. Only the Vulkan implementation is
-- compiled; examples and tests are left out.
project "NoGraphicsAPI"
do
	kind "StaticLib"
	language "C++"

	targetdir "build/lib/%{cfg.buildcfg}"
	objdir "build/obj/%{prj.name}/%{cfg.buildcfg}"

	files {
		"src/NoGraphicsAPI/src/NoGraphicsAPI.cpp",
		"src/NoGraphicsAPI/include/**.h",
		"src/NoGraphicsAPI/include/**.hpp",
	}

	includedirs {
		"src/NoGraphicsAPI/include",
		VULKAN_SDK_DIR .. "/Include",
	}

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
-- texture upload, AVX2 math). One static library instead of the CMake
-- allocators/textures/uploads split. Anything including math.hpp needs
-- -mavx2 -mfma, so the engine passes them too.
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
		VULKAN_SDK_DIR .. "/Include",
	}

	buildoptions {
		"-std=c++20",
		"-fno-exceptions",
		"-fno-rtti",
		"-mavx2",
		"-mfma",
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
	-- The NoGraphicsAPI submodule is built by its own project above, so it
	-- must not be globbed in here (it contains tests/examples with a main()).
	files {
		"src/**.cpp",
		"src/**.hpp",
	}
	removefiles {
		"src/NoGraphicsAPI/**",
	}

	-- Header search paths -----------------------------------------------
	includedirs {
		"src",
		"src/NoGraphicsAPI/include",
		"src/NoGraphicsAPI/utility/include",
		VULKAN_SDK_DIR .. "/Include",
	}

	-- Libraries ---------------------------------------------------------
	-- NoGraphicsAPI is linked statically; vulkan-1 is the Vulkan loader's
	-- import library (the loader DLL itself ships with the GPU driver).
	libdirs {
		VULKAN_SDK_DIR .. "/Lib",
	}
	links {
		"NoGraphicsAPIUtility",
		"NoGraphicsAPI",
		"vulkan-1",
	}

	-- C++26 is requested explicitly so the exact clang flag is used
	-- regardless of the premake version's cppdialect support.
	-- -fdiagnostics-absolute-paths makes clang print absolute file paths in
	-- warnings/errors, so terminal links work even though ninja builds from
	-- inside build/.
	-- -mavx2 -mfma are required by NoGraphicsAPIUtility/math.hpp.
	buildoptions { "-std=c++26", "-mavx2", "-mfma", "-Wall", "-fdiagnostics-absolute-paths" }

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
