// foo_danmaku.cpp - main entry point for foobar2000 danmaku plugin
#include "foo_danmaku.h"
#include "ui/danmaku_ui.h"
#include "core/danmaku_engine.h"
#include "core/playback_monitor.h"
#include <windows.h>

DECLARE_COMPONENT_VERSION(
    "FooBar Danmaku",
    "2.1.0",
    "Fetches NetEase Cloud Music comments for the currently playing song and displays them as scrolling danmaku."
);

VALIDATE_COMPONENT_FILENAME("foo_danmaku.dll");

static service_factory_single_t<DanmakuUI> g_danmaku_ui_factory;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
    }
    return TRUE;
}


