/* test_netease.cpp — standalone console test for netease_client.dll
 *
 * Usage: test_netease.exe [title] [artist]
 * Default: "晴天" "周杰伦"
 *
 * Build: see test_build.ps1
 */

#include <windows.h>
#include <stdio.h>
#include <string>

/* Load DLL at runtime so we don't need the .lib */
typedef void*    (__stdcall *FnCreate)(const wchar_t*);
typedef void     (__stdcall *FnDestroy)(void*);
typedef void     (__stdcall *FnSetLog)(void (__stdcall*)(const wchar_t*, void*), void*);
typedef int      (__stdcall *FnGetByKw)(void*, const wchar_t*, const wchar_t*,
                                        int,
                                        int (__stdcall*)(const wchar_t*, const wchar_t*, int, void*),
                                        void*);
typedef const wchar_t* (__stdcall *FnLastError)(void*);

/* Log callback: print to stdout */
static void __stdcall on_log(const wchar_t* msg, void*) {
    wprintf(L"  [SDK] %s\n", msg);
}

/* Comment callback */
static int g_count = 0;
static int __stdcall on_comment(const wchar_t* content, const wchar_t* nick,
                                int likes, void*) {
    ++g_count;
    wprintf(L"  #%02d [%4d❤] %s\n        — %s\n", g_count, likes,
            content ? content : L"(null)",
            nick    ? nick    : L"(null)");
    return 0; /* continue */
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    const wchar_t* title  = (argc > 1) ? argv[1] : L"晴天";
    const wchar_t* artist = (argc > 2) ? argv[2] : L"周杰伦";

    wprintf(L"=== netease_client test ===\n");
    wprintf(L"Song  : %s\n", title);
    wprintf(L"Artist: %s\n\n", artist);

    /* Load DLL */
    HMODULE dll = LoadLibraryW(L"netease_client.dll");
    if (!dll) {
        DWORD e = GetLastError();
        wprintf(L"LoadLibrary failed: err=%lu\n", e);
        /* Try full path next to exe */
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) { *(slash+1) = 0; wcscat_s(exePath, L"netease_client.dll"); }
        dll = LoadLibraryW(exePath);
        if (!dll) { wprintf(L"Still failed. Aborting.\n"); return 1; }
    }
    wprintf(L"DLL loaded OK\n");

#define GETPROC(name, type) \
    auto fn_##name = (type)GetProcAddress(dll, #name); \
    if (!fn_##name) { wprintf(L"GetProcAddress(" #name ") failed\n"); return 1; }

    GETPROC(netease_create,                   FnCreate)
    GETPROC(netease_destroy,                  FnDestroy)
    GETPROC(netease_set_global_log,           FnSetLog)
    GETPROC(netease_get_comments_by_keyword,  FnGetByKw)
    GETPROC(netease_last_error,               FnLastError)
#undef GETPROC

    /* Create handle and register log sink */
    void* h = fn_netease_create(nullptr);
    if (!h) { wprintf(L"netease_create returned NULL\n"); return 1; }
    fn_netease_set_global_log(on_log, nullptr);
    wprintf(L"Handle created, log callback set.\n\n");

    /* Fetch comments */
    wprintf(L"--- fetching up to 20 comments ---\n");
    int rc = fn_netease_get_comments_by_keyword(
        h, title, artist, 20, on_comment, nullptr);

    wprintf(L"\n--- result ---\n");
    wprintf(L"rc = %d  (%s)\n", rc, rc == 0 ? L"OK" : L"ERROR");
    if (rc != 0)
        wprintf(L"last_error: %s\n", fn_netease_last_error(h));
    wprintf(L"total comments received: %d\n", g_count);

    fn_netease_destroy(h);
    FreeLibrary(dll);
    return rc;
}
