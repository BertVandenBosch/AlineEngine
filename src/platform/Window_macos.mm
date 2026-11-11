// macOS window, based on NoGraphicsAPI's examples/common/example_window_macos.mm.
// Built with manual retain/release (-fno-objc-arc).

#include "platform/Window.hpp"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <stdlib.h>
#include <time.h>

struct Window
{
    NSWindow*     ns_window;
    CAMetalLayer* layer;
};

static void sync_drawable_size(Window* window)
{
    NSWindow*     ns_window = window->ns_window;
    CAMetalLayer* layer     = window->layer;
    layer.contentsScale     = ns_window.backingScaleFactor;
    const NSSize size       = ns_window.contentView.bounds.size;
    layer.drawableSize      = CGSizeMake(size.width * layer.contentsScale, size.height * layer.contentsScale);
}

Window* window_create(const WindowDesc& desc)
{
    assert(desc.title && desc.width && desc.height);
    @autoreleasepool
    {
        static bool app_initialized = false;
        if (!app_initialized)
        {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp finishLaunching];
            app_initialized = true;
        }

        const NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                        NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;
        NSWindow* ns_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, desc.width, desc.height)
                                                          styleMask:style
                                                            backing:NSBackingStoreBuffered
                                                              defer:NO];
        if (!ns_window)
            return nullptr;
        ns_window.releasedWhenClosed = NO;
        ns_window.title              = [NSString stringWithUTF8String:desc.title];

        // The view retains the layer; it lives as long as the window.
        CAMetalLayer* layer               = [CAMetalLayer layer];
        ns_window.contentView.wantsLayer = YES;
        ns_window.contentView.layer      = layer;

        Window* window = static_cast<Window*>(malloc(sizeof(Window)));
        *window        = {.ns_window = ns_window, .layer = layer};
        sync_drawable_size(window);

        [ns_window center];
        [ns_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return window;
    }
}

void window_destroy(Window* window)
{
    if (!window)
        return;
    @autoreleasepool
    {
        [window->ns_window close];
        [window->ns_window release];
    }
    free(window);
}

// Dispatches queued events; with `wait`, first blocks until at least one arrives.
static void dispatch_events(bool wait)
{
    NSDate* until = wait ? [NSDate distantFuture] : [NSDate distantPast];
    for (;;)
    {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:until
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (!event)
            return;
        [NSApp sendEvent:event];
        until = [NSDate distantPast];
    }
}

bool window_pump(Window* window)
{
    @autoreleasepool
    {
        NSWindow* ns_window = window->ns_window;
        dispatch_events(false);
        // While minimized there is nothing to present: sleep on the event
        // queue instead of spinning (like the Win32 example's WaitMessage).
        while (ns_window.miniaturized)
            dispatch_events(true);
        // Closing hides the window (releasedWhenClosed = NO).
        if (!ns_window.visible)
            return false;
        sync_drawable_size(window);
        return true;
    }
}

void* window_native_handle(const Window* window) { return window->layer; }

double platform_time_seconds()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return double(now.tv_sec) + double(now.tv_nsec) * 1.0e-9;
}
