/* provider_entry.cpp  —  MusicProviderVTable adapter for qqmusic_client
 *
 * This file bridges the qqmusic_client C API to the generic
 * MusicProviderVTable driver contract expected by music_client (Layer 2).
 *
 * qqmusic_client.dll exports TWO sets of entry points:
 *   1. All qqmusic_* exports        — for direct/legacy callers.
 *   2. music_provider_vtable()      — the generic driver contract used
 *                                     by music_client at runtime.
 */

#define MUSIC_PROVIDER_EXPORTS
#include "music_provider.h"           /* from include/ */

#include "../include/qqmusic_client.h"

/* ── static vtable functions ─────────────────────────── */

static MusicProviderHandle __stdcall qm_create(const wchar_t* cookie) {
    return qqmusic_create(cookie);
}

static void __stdcall qm_destroy(MusicProviderHandle h) {
    qqmusic_destroy(h);
}

static int __stdcall qm_search_song(
    MusicProviderHandle h,
    const wchar_t*      keyword,
    wchar_t*            out_song_id,
    int                 song_id_buf_wchars,
    wchar_t*            out_cover_url,
    int                 cover_url_buf_wchars)
{
    return qqmusic_search_song_with_cover(
        h,
        keyword,
        out_song_id,   song_id_buf_wchars,
        out_cover_url, cover_url_buf_wchars);
}

static int __stdcall qm_get_comments_paged(
    MusicProviderHandle  h,
    const wchar_t*       song_id,
    int                  offset,
    int                  page_limit,
    MusicCommentCallback callback,
    void*                userdata,
    int*                 out_delivered)
{
    /* QQMusicCommentCallback and MusicCommentCallback share an identical
     * signature and calling convention (__stdcall), so a direct cast is safe. */
    return qqmusic_get_comments_by_id_paged(
        h,
        song_id,
        offset,
        page_limit,
        reinterpret_cast<QQMusicCommentCallback>(callback),
        userdata,
        out_delivered);
}

static const wchar_t* __stdcall qm_last_error(MusicProviderHandle h) {
    return qqmusic_last_error(h);
}

static void __stdcall qm_set_log(
    MusicProviderHandle h,
    MusicLogCallback    cb,
    void*               userdata)
{
    /* qqmusic_set_global_log is process-wide rather than per-handle.
     * QQMusicLogCallback and MusicLogCallback share the same signature. */
    (void)h;
    qqmusic_set_global_log(
        reinterpret_cast<QQMusicLogCallback>(cb),
        userdata);
}

/* ── vtable instance ─────────────────────────────────── */

static const MusicProviderVTable kQQMusicVTable = {
    qm_create,
    qm_destroy,
    qm_search_song,
    qm_get_comments_paged,
    qm_last_error,
    qm_set_log
};

/* ── required DLL export ─────────────────────────────── */

MUSIC_PROVIDER_API const MusicProviderVTable* __stdcall music_provider_vtable(void) {
    return &kQQMusicVTable;
}
