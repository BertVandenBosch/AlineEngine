#pragma once

#include "core/core.hpp"

/*
 * Lean, platform-agnostic native window: just enough to give NoGraphicsAPI a
 * surface to present to. One implementation file per platform
 * (Window_macos.mm; Win32 still to come). Single-threaded: create, pump and
 * destroy on the main thread.
 */

struct Window; // opaque

struct WindowDesc
{
    const char* title  = "AlineEngine";
    u32         width  = 1280; // content size in points
    u32         height = 720;
};

[[nodiscard]] Window* window_create(const WindowDesc& desc); // null on failure
void window_destroy(Window* window); // after the gpu device using it is destroyed

// Processes pending OS events and syncs the drawable size with the window.
// Blocks while the window is minimized. False once the user closed the window.
[[nodiscard]] bool window_pump(Window* window);

// Pass to gpu::DeviceDesc::window: CAMetalLayer* on macOS, HWND on Windows.
// The window must outlive the device.
[[nodiscard]] void* window_native_handle(const Window* window);

double platform_time_seconds(); // monotonic
