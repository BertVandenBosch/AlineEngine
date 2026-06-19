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
-- The clang++ toolchain, C++26 and the original include/library layout from
-- build.bat are all preserved below. To add a source folder, a dependency or a
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
-- Dependencies
-- ---------------------------------------------------------------------------

local vulkan_sdk = os.getenv("VULKAN_SDK")
if not vulkan_sdk then
	error("VULKAN_SDK environment variable is not set -- install the Vulkan SDK")
end
print("Vulkan SDK: " .. vulkan_sdk)

local glfw = "third-party/glfw3.4"

-- ---------------------------------------------------------------------------
-- Workspace
-- ---------------------------------------------------------------------------

workspace "AlineEngine"
do
	configurations { "Debug", "Release" }
	location "build" -- generated project/make files live here
	toolset "clang" -- compile with clang++
end

project "AlineEngine"
do
	kind "ConsoleApp"
	language "C++"

	targetdir "build"
	objdir "build/obj/%{cfg.buildcfg}"

	-- Source files ------------------------------------------------------
	-- `**` recurses, so every .cpp/.hpp under src/ is picked up automatically;
	-- just drop new files in and re-run premake. (Headers are listed only so
	-- they appear in IDE projects -- only the .cpp files are compiled.)
	files {
		"src/**.cpp",
		"src/**.hpp",
	}

	-- Header search paths -----------------------------------------------
	includedirs {
		"src",
		vulkan_sdk .. "/Include",
		glfw .. "/include",
	}

	-- Libraries ---------------------------------------------------------
	libdirs {
		glfw .. "/lib-vc2022",
		vulkan_sdk .. "/Lib",
	}
	links {
		"glfw3dll",
		"gdi32",
		"vulkan-1",
	}

	-- C++26 is requested explicitly so the exact clang flag is used
	-- regardless of the premake version's cppdialect support.
	-- -fdiagnostics-absolute-paths makes clang print absolute file paths in
	-- warnings/errors, so terminal links work even though ninja builds from
	-- inside build/.
	buildoptions { "-std=c++26", "-Wall", "-fdiagnostics-absolute-paths" }

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
