// Renders the editor to a PNG without a plugin host.
//   editor_snapshot out.png [character=1 voices=5 interval=150 warp=60 warptype=2 sync=1 ...]
// Keys are parameter names, lowercased, spaces removed ("sync" = Tempo Sync).
#import <AppKit/AppKit.h>
#include "Editor.h"
#include "Parameters.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace granular;
static Values values = defaults();
static const float demoMeters[3] {.34f, .5f, .72f};

static std::string key(const char* name) {
    std::string out;
    for (const char* c = name; *c; ++c) if (*c != ' ') out += static_cast<char>(std::tolower(*c));
    return out;
}
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s out.png [param=value ...]\n", argv[0]); return 2; }
    for (int i = 2; i < argc; ++i) {
        const char* eq = std::strchr(argv[i], '=');
        if (!eq) { std::fprintf(stderr, "bad override %s\n", argv[i]); return 2; }
        const std::string name(static_cast<const char*>(argv[i]), static_cast<size_t>(eq-argv[i]));
        bool found = false;
        for (uint32_t id = 0; id < ParamCount; ++id)
            if (name == key(parameters[id].name) || (id == Sync && name == "sync")) {
                values[id] = sanitize(id, std::atof(eq+1)); found = true;
            }
        if (!found) { std::fprintf(stderr, "unknown parameter %s\n", name.c_str()); return 2; }
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        EditorCallbacks callbacks;
        callbacks.get = [](void*, uint32_t id) { return values[id]; };
        callbacks.edit = [](void*, uint32_t id, double value, int kind) { if (kind == 0) values[id] = value; return true; };
        callbacks.meter = [](void*, unsigned index) { return demoMeters[index%3]; };
        callbacks.tempo = [](void*) { return 120.; };
        auto* editor = Editor::create(callbacks);
        NSView* view = (__bridge NSView*)editor->nativeView();
        auto* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, Editor::width, Editor::height)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        window.contentView = view;
        [view layoutSubtreeIfNeeded]; [view displayIfNeeded];
        const CGFloat scale = 2;
        auto* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:Editor::width*scale pixelsHigh:Editor::height*scale
            bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        rep.size = NSMakeSize(Editor::width, Editor::height);
        [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (![png writeToFile:[NSString stringWithUTF8String:argv[1]] atomically:YES]) { std::fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
        delete editor;
    }
    std::printf("wrote %s (%ux%u @%gx)\n", argv[1], Editor::width, Editor::height, 2.0);
    return 0;
}
