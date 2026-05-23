/* test_qqmusic.cpp — standalone console test for qqmusic_client.dll
 *
 * Usage: test_qqmusic.exe [title] [artist]
 * Default: "晴天" "周杰伦"
 *
 * Build: see test_build.ps1
 * The DLL is loaded at runtime (no .lib needed at link time).
 */

#include <windows.h>
#include <stdio.h>
#include <string>

/* ── function pointer types (must match qqmusic_client.h exactly) ── */
typedef void*          (__stdcall *FnCreate)(const wchar_t*);
typedef void           (__stdcall *FnDestroy)(void*);
typedef void           (__stdcall *FnSetLog)(
                            void (__stdcall*)(const wchar_t*, void*), void*);
typedef int            (__stdcall *FnGetByKw)(
                            void*, const wchar_t*, const wchar_t*, int,
                            int (__stdcall*)(const wchar_t*, const wchar_t*, int, void*),
                            void*);
typedef int            (__stdcall *FnSearchWithCover)(
                            void*, const wchar_t*,
                            wchar_t*, int, wchar_t*, int);
typedef const wchar_t* (__stdcall *FnLastError)(void*);

/* ── callbacks ───────────────────────────────────────── */

static void __stdcall on_log(const wchar_t* msg, void*) {
    wprintf(L"  [SDK] %s\n", msg);
}

static int g_count = 0;
static int __stdcall on_comment(const wchar_t* content, const wchar_t* nick,
                                int likes, void*) {
    ++g_count;
    wprintf(L"  #%02d [%4d\u2764] %s\n        \u2014 %s\n",
            g_count, likes,
            content ? content : L"(null)",
            nick    ? nick    : L"(null)");
    return 0; /* continue */
}

/* ── main ────────────────────────────────────────────── */

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    const wchar_t* title  = (argc > 1) ? argv[1] : L"\u6674\u5929";   /* 晴天 */
    const wchar_t* artist = (argc > 2) ? argv[2] : L"\u5468\u6770\u4f26"; /* 周杰伦 */

    wprintf(L"=== qqmusic_client test ===\n");
    wprintf(L"Song  : %s\n", title);
    wprintf(L"Artist: %s\n\n", artist);

    /* ── load DLL ────────────────────────────────────── */
    HMODULE dll = LoadLibraryW(L"qqmusic_client.dll");
    if (!dll) {
        DWORD e = GetLastError();
        wprintf(L"LoadLibrary failed: err=%lu\n", e);
        /* Try alongside the exe */
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) { *(slash+1) = 0; wcscat_s(exePath, L"qqmusic_client.dll"); }
        dll = LoadLibraryW(exePath);
        if (!dll) { wprintf(L"Still failed. Aborting.\n"); return 1; }
    }
    wprintf(L"DLL loaded OK\n");

#define GETPROC(name, type) \
    auto fn_##name = (type)GetProcAddress(dll, #name); \
    if (!fn_##name) { wprintf(L"GetProcAddress(" #name ") failed\n"); \
                      FreeLibrary(dll); return 1; }

    GETPROC(qqmusic_create,                  FnCreate)
    GETPROC(qqmusic_destroy,                 FnDestroy)
    GETPROC(qqmusic_set_global_log,          FnSetLog)
    GETPROC(qqmusic_get_comments_by_keyword, FnGetByKw)
    GETPROC(qqmusic_search_song_with_cover,  FnSearchWithCover)
    GETPROC(qqmusic_last_error,              FnLastError)
#undef GETPROC

    /* ── create handle + register log ───────────────── */
    void* h = fn_qqmusic_create(nullptr);
    if (!h) { wprintf(L"qqmusic_create returned NULL\n"); FreeLibrary(dll); return 1; }
    fn_qqmusic_set_global_log(on_log, nullptr);
    wprintf(L"Handle created, log callback set.\n\n");

    /* ── search for song + cover ─────────────────────── */
    wchar_t song_mid[64]    = {};
    wchar_t cover_url[1024] = {};
    std::wstring kw = std::wstring(title) + L" " + artist;
    int src = fn_qqmusic_search_song_with_cover(
        h, kw.c_str(),
        song_mid,  (int)(sizeof(song_mid)  / sizeof(wchar_t)),
        cover_url, (int)(sizeof(cover_url) / sizeof(wchar_t)));

    if (src == 0) {
        wprintf(L"Song MID  : %s\n", song_mid);
        wprintf(L"Cover URL : %s\n\n", cover_url[0] ? cover_url : L"(none)");
    } else {
        wprintf(L"Search failed (rc=%d): %s\n\n",
                src, fn_qqmusic_last_error(h));
    }

    /* ── fetch comments ──────────────────────────────── */
    wprintf(L"--- fetching up to 20 comments ---\n");
    int rc = fn_qqmusic_get_comments_by_keyword(
        h, title, artist, 20, on_comment, nullptr);

    wprintf(L"\n--- result ---\n");
    wprintf(L"rc = %d  (%s)\n", rc, rc == 0 ? L"OK" : L"ERROR");
    if (rc != 0)
        wprintf(L"last_error: %s\n", fn_qqmusic_last_error(h));
    wprintf(L"total comments received: %d\n", g_count);

    fn_qqmusic_destroy(h);
    FreeLibrary(dll);
    return rc;
}
