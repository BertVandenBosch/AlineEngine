// Win32 window, based on NoGraphicsAPI's examples/common/example_window_win32.cpp.
// Window* is the HWND itself; the Vulkan swapchain follows the client size on its own.

#include "platform/Window.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    // Hide rather than destroy: the swapchain presents to this HWND until the device is destroyed.
    if (message == WM_CLOSE)
    {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

Window* window_create(const WindowDesc& desc)
{
    assert(desc.title && desc.width && desc.height);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // points -> native pixels, like macOS
    const HINSTANCE instance = GetModuleHandleA(nullptr);
    const WNDCLASSA window_class = {
        .lpfnWndProc = window_proc, .hInstance = instance, .hCursor = LoadCursorA(nullptr, IDC_ARROW), .lpszClassName = "AlineEngine"};
    if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;

    const UINT dpi  = GetDpiForSystem();
    RECT       rect = {.right = MulDiv(desc.width, dpi, 96), .bottom = MulDiv(desc.height, dpi, 96)};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    return reinterpret_cast<Window*>(CreateWindowExA(0, "AlineEngine", desc.title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                                     rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr));
}

void window_destroy(Window* window)
{
    assert(window);
    DestroyWindow(reinterpret_cast<HWND>(window));
}

bool window_pump(Window* window)
{
    const HWND hwnd = reinterpret_cast<HWND>(window);
    for (;;)
    {
        MSG message;
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (!IsWindowVisible(hwnd)) // closed, possibly while minimized
            return false;
        if (!IsIconic(hwnd))
            return true;
        WaitMessage(); // minimized: nothing to present, sleep until the next message
    }
}

void* window_native_handle(const Window* window) { return const_cast<Window*>(window); }

double platform_time_seconds()
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return double(counter.QuadPart) / double(frequency.QuadPart);
}
