/* provider_entry.cpp  —  MusicProviderVTable adapter for netease_client
 *
 * This file bridges the existing netease_client C API to the generic
 * MusicProviderVTable driver contract expected by music_client (Layer 2).
 *
 * The netease_client.dll now exports TWO entry points:
 *   1. All original netease_* exports  — unchanged, for direct/legacy callers.
 *   2. music_provider_vtable()         — the generic driver contract entry point
 *                                        used by music_client at runtime.
 */

/* Must be defined before including music_provider.h so the export macro
   emits __declspec(dllexport) rather than the empty fallback. */
#define MUSIC_PROVIDER_EXPORTS
#include "music_provider.h"       /* copied to include/ by build.ps1 */

#include "../include/netease_client.h"

/* ── static vtable functions ─────────────────────────── */

static MusicProviderHandle __stdcall nc_create(const wchar_t* cookie) {
    return netease_create(cookie);
}

static void __stdcall nc_destroy(MusicProviderHandle h) {
    netease_destroy(h);
}

static int __stdcall nc_search_song(
    MusicProviderHandle h,
    const wchar_t*      keyword,
    wchar_t*            out_song_id,
    int                 song_id_buf_wchars,
    wchar_t*            out_cover_url,
    int                 cover_url_buf_wchars)
{
    return netease_search_song_with_cover(
        h,
        keyword,
        out_song_id,  song_id_buf_wchars,
        out_cover_url, cover_url_buf_wchars);
}

static int __stdcall nc_get_comments_paged(
    MusicProviderHandle  h,
    const wchar_t*       song_id,
    int                  offset,
    int                  page_limit,
    MusicCommentCallback callback,
    void*                userdata,
    int*                 out_delivered)
{
    /* NeteaseCommentCallback and MusicCommentCallback share an identical
     * signature and calling convention (__stdcall), so a direct cast is safe. */
    return netease_get_comments_by_id_paged(
        h,
        song_id,
        offset,
        page_limit,
        reinterpret_cast<NeteaseCommentCallback>(callback),
        userdata,
        out_delivered);
}

static const wchar_t* __stdcall nc_last_error(MusicProviderHandle h) {
    return netease_last_error(h);
}

static void __stdcall nc_set_log(
    MusicProviderHandle h,
    MusicLogCallback    cb,
    void*               userdata)
{
    /* netease_set_global_log is process-wide rather than per-handle.
     * NeteaseLogCallback and MusicLogCallback share the same signature. */
    (void)h;
    netease_set_global_log(
        reinterpret_cast<NeteaseLogCallback>(cb),
        userdata);
}

/* ── vtable instance ─────────────────────────────────── */

static const MusicProviderVTable kNeteaseVTable = {
    nc_create,
    nc_destroy,
    nc_search_song,
    nc_get_comments_paged,
    nc_last_error,
    nc_set_log
};

/* ── required DLL export ─────────────────────────────── */

MUSIC_PROVIDER_API const MusicProviderVTable* __stdcall music_provider_vtable(void) {
    return &kNeteaseVTable;
}
