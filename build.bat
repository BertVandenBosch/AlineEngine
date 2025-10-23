@echo off
setlocal enabledelayedexpansion

:: Check if the vulkan sdk, glm, vma and volk ared detected

if "%VULKAN_SDK%"=="" (
    echo Error: Vulkan sdk path not found in env %VULKAN_SDK%
    exit /b 1
)
set VULKAN_INC=%VULKAN_SDK%\Include

echo Vulkan SDK: %VULKAN_SDK%
echo Vulkan Include: %VULKAN_INC%

:: Configuration - Customize these paths as needed
set INCLUDE_DIRS=./src %VULKAN_INC% ..\third-party\glfw3.4\include
set SRC_DIRS=./src ./src/core

:: Check if build type argument is provided
@REM if "%~1"=="" (
@REM     echo Error: Please specify build type (debug or release)
@REM     echo Usage: %~nx0 [debug^|release]
@REM     exit /b 1
@REM )

:: Set build type from first argument
set BUILD_TYPE=%~1
set BUILD_TYPE=%BUILD_TYPE: =%

if	"%~2"=="--autorun" (
	set AUTO_RUN=true
)

set C_FLAGS=-std=c++20
set BUILD_DB=false
if "%~2"=="intellisense" (
    set C_FLAGS=!C_FLAGS! -MJ main.json
    set BUILD_DB=true
    echo Set to build intellisense
)

:: Validate build type
if /i not "%BUILD_TYPE%"=="debug" if /i not "%BUILD_TYPE%"=="release" (
    echo Error: Invalid build type. Use 'debug' or 'release'
    exit /b 1
)

:: Set compiler flags based on build type
if /i "%BUILD_TYPE%"=="debug" (
    set COMPILER_FLAGS=-g -O0 -MD -Wall -Wextra
    set OUTPUT_NAME=AlineEngine_debug.exe
    echo Building DEBUG version...
) else (
    set COMPILER_FLAGS=-O2 -MD -Wall -Wextra
    set OUTPUT_NAME=AlineEngine_release.exe
    echo Building RELEASE version...
)

:: Set linker flags
set LINKER_FLAGS= -lglfw3 -lvulkan

:: Build include paths
set INCLUDE_FLAGS=
for %%d in (%INCLUDE_DIRS%) do (
    @REM if exist "%%d" (
        set INCLUDE_FLAGS=!INCLUDE_FLAGS! -I"%%d"
    @REM ) else (
    @REM     echo Warning: Include directory "%%d" does not exist
    @REM )
)

:: Display compilation info
echo Include directories: %INCLUDE_DIRS%
echo Compiling with flags: %COMPILER_FLAGS%

if errorlevel 1 (
    echo Error: Failed to setup MSVC environment
    exit /b 1
)

:: Create build folder
if not exist build mkdir build
pushd build

::/Fe:%OUTPUT_NAME%
:: Compile all .cpp files
clang++ %C_FLAGS% %COMPILER_FLAGS% %INCLUDE_FLAGS%   ..\src\main.cpp -L"C:\Users\bert\Desktop\AlineEngine\third-party\glfw3.4\lib-vc2022" -lglfw3dll -lgdi32  -l %VULKAN_SDK%\Lib\vulkan-1.lib -o %OUTPUT_NAME%

if errorlevel 1 (
    echo.
    echo Compilation failed!
    exit /b 1
) else (
    echo.
    echo Successfully built: %OUTPUT_NAME%
    echo Build type: %BUILD_TYPE%
)

if /i %BUILD_DB%==true (
    echo [ > compile_commands.json
    type main.json >> compile_commands.json

    :: fix for trailing comma error
    :: echo {} >> compile_commands.json

    echo ] >> compile_commands.json

    :: cleanup
    del main.json

    echo Created compile_commands.json
)

:: out of build dir
popd

:: auto run
if	defined AUTO_RUN (
	if /i "%BUILD_TYPE%"=="debug" (
		raddbg --project:raddb_proj.txt --auto_run
	) else (
		.\build\%OUTPUT_NAME%
	)
)

endlocal
