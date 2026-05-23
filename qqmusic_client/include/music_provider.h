#ifndef MUSIC_PROVIDER_H
#define MUSIC_PROVIDER_H

/* music_provider.h  —  Layer-3 driver contract
 *
 * Every platform provider DLL (netease_client.dll, qqmusic_client.dll, …)
 * must export exactly ONE C function:
 *
 *   MUSIC_PROVIDER_API const MusicProviderVTable* __stdcall music_provider_vtable(void);
 *
 * music_client (Layer 2) loads the DLL at runtime with LoadLibraryW /
 * GetProcAddress, calls this function once, and uses the returned
 * function-pointer table for all subsequent operations.
 *
 * Error codes returned by vtable functions are a subset of the MUSIC_ERR_*
 * values defined in music_client.h (same numeric values).
 */

#ifdef MUSIC_PROVIDER_EXPORTS
#  define MUSIC_PROVIDER_API __declspec(dllexport)
#else
#  define MUSIC_PROVIDER_API  /* caller uses GetProcAddress, not import lib */
#endif

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── opaque per-instance handle ──────────────────────── */
typedef void* MusicProviderHandle;

/* ── callbacks (cross DLL boundary → must be __stdcall) ─ */

/** Comment delivery callback.  Return 0 to continue, non-zero to stop. */
typedef int (__stdcall *MusicCommentCallback)(
    const wchar_t* content,
    const wchar_t* nickname,
    int            like_count,
    void*          userdata);

/** Log sink callback. */
typedef void (__stdcall *MusicLogCallback)(
    const wchar_t* msg,
    void*          userdata);

/* ── driver vtable ───────────────────────────────────── */
typedef struct MusicProviderVTable {

    /* --- lifecycle ---------------------------------------- */

    /** Allocate a provider instance.
     *  @param cookie  Optional raw cookie string, or NULL for anonymous. */
    MusicProviderHandle (__stdcall *create)(const wchar_t* cookie);

    /** Release a provider instance and all associated resources. */
    void (__stdcall *destroy)(MusicProviderHandle h);

    /* --- search ------------------------------------------- */

    /** Search for a song by keyword and return its platform ID plus an
     *  optional cover-art URL.
     *
     *  @param out_cover_url  May be NULL if the caller does not need it.
     *  @return 0 (MUSIC_OK) or a negative MUSIC_ERR_* code. */
    int (__stdcall *search_song)(
        MusicProviderHandle h,
        const wchar_t*      keyword,
        wchar_t*            out_song_id,
        int                 song_id_buf_wchars,
        wchar_t*            out_cover_url,          /* may be NULL */
        int                 cover_url_buf_wchars);

    /* --- comments ----------------------------------------- */

    /** Fetch one page of comments (single HTTP request).
     *
     *  On the first page (offset == 0) hot/top comments are emitted before
     *  the regular list.  Returns MUSIC_OK even when 0 comments are delivered
     *  (signals end-of-stream).
     *
     *  @param out_delivered  Set to the number of comments passed to callback. */
    int (__stdcall *get_comments_paged)(
        MusicProviderHandle  h,
        const wchar_t*       song_id,
        int                  offset,
        int                  page_limit,
        MusicCommentCallback callback,
        void*                userdata,
        int*                 out_delivered);

    /* --- diagnostics -------------------------------------- */

    /** Last error description; valid until the next call on this handle.
     *  Never returns NULL. */
    const wchar_t* (__stdcall *last_error)(MusicProviderHandle h);

    /** Set a per-instance log sink.  Pass NULL to clear.
     *  May be NULL if the provider does not support per-instance logging
     *  (music_client will fall back to the global log if available). */
    void (__stdcall *set_log)(
        MusicProviderHandle h,
        MusicLogCallback    cb,
        void*               userdata);

} MusicProviderVTable;

/* ── required DLL export ─────────────────────────────── */

/** typedef for GetProcAddress cast in music_client */
typedef const MusicProviderVTable* (__stdcall *music_provider_vtable_fn)(void);

/** The symbol name music_client looks for with GetProcAddress. */
#define MUSIC_PROVIDER_VTABLE_EXPORT "music_provider_vtable"

/** Every provider DLL must implement and export this. */
MUSIC_PROVIDER_API const MusicProviderVTable* __stdcall music_provider_vtable(void);

#ifdef __cplusplus
}
#endif
#endif /* MUSIC_PROVIDER_H */
