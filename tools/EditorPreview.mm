#import <AppKit/AppKit.h>
#include "Editor.h"
#include "Parameters.h"
#include <clap/clap.h>
#include <cstring>

using namespace granular;
static const clap_plugin_t* plugin = nullptr;
static const clap_output_events_t output {nullptr, [](const clap_output_events_t*,const clap_event_header_t*) { return true; }};
static void flushChanges(const clap_host_t*) {
    if (!plugin) return;
    const auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin,CLAP_EXT_PARAMS));
    params->flush(plugin,nullptr,&output);
}
static const clap_host_params_t hostParams {[](const clap_host_t*,clap_param_rescan_flags) {},nullptr,flushChanges};
static const clap_host_t host {CLAP_VERSION,nullptr,"Editor Preview","Steve Murr","","1",
    [](const clap_host_t*,const char* id) -> const void* { return !std::strcmp(id,CLAP_EXT_PARAMS) ? &hostParams : nullptr; },
    [](const clap_host_t*) {},flushChanges,[](const clap_host_t*) {}};
int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        auto* menu = [[NSMenu alloc] init];
        auto* appItem = [[NSMenuItem alloc] init]; [menu addItem:appItem];
        auto* appMenu = [[NSMenu alloc] initWithTitle:@"Editor Preview"];
        [appMenu addItemWithTitle:@"Quit Editor Preview" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu;
        auto* editItem = [[NSMenuItem alloc] init]; [menu addItem:editItem];
        auto* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
        [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
        [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        editItem.submenu = editMenu; NSApp.mainMenu = menu;
        clap_entry.init("preview");
        const auto* factory = static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        plugin = factory->create_plugin(factory,&host,"com.stevemurr.granular-delay");
        if (!plugin || !plugin->init(plugin)) return 1;
        const auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin,CLAP_EXT_GUI));
        if (!gui || !gui->create(plugin,CLAP_WINDOW_API_COCOA,false)) return 2;
        auto* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,Editor::width,Editor::height)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
        window.title = @"Granular Delay — Editor Preview";
        clap_window_t parent {}; parent.api = CLAP_WINDOW_API_COCOA; parent.cocoa = (__bridge void*)window.contentView;
        [window center]; gui->set_parent(plugin,&parent); gui->show(plugin);
        [window makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; [NSApp run];
        gui->destroy(plugin); plugin->destroy(plugin); plugin = nullptr; clap_entry.deinit();
    }
}
