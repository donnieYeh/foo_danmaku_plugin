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

/* Optional extension export. Providers may export this function in addition
 * to music_provider_vtable(). Layer 2 probes for it with GetProcAddress and
 * falls back to MusicProviderVTable::search_song when it is absent. */
typedef struct MusicStructuredSearchQuery {
    const wchar_t* title;       /* required */
    const wchar_t* artist;      /* optional */
    const wchar_t* album;       /* optional */
    int            duration_ms; /* optional; 0 if unknown */
} MusicStructuredSearchQuery;

typedef int (__stdcall *music_provider_search_track_fn)(
    MusicProviderHandle              h,
    const MusicStructuredSearchQuery* query,
    wchar_t*                         out_song_id,
    int                              song_id_buf_wchars,
    wchar_t*                         out_cover_url,
    int                              cover_url_buf_wchars);

#define MUSIC_PROVIDER_SEARCH_TRACK_EXPORT "music_provider_search_track"

/* ── driver vtable ───────────────────────────────────── */
typedef struct MusicProviderVTable {

    /* --- lifecycle ---------------------------------------- */

    /** Allocate a provider instance.
     *  @param cookie  Optional raw cookie string, or NULL for anonymous. */
    MusicProviderHandle (__stdcall *create)(const wchar_t* cookie);

    /** Release a provider instance and all associated resources. */
    void (__stdcall *destroy)(MusicProviderHandle h);

    /* --- search ------------------------------------------- */

    /** Search for a song by keyword.
     *
     *  @param out_song_id       Receives the platform-specific song ID string
     *                           that will be passed to get_comments_paged.
     *  @param out_cover_url     Internal Layer-2 handoff only: receives a
     *                           direct HTTPS CDN URL for the album art image
     *                           (JPEG/PNG).  May be NULL if the caller does not
     *                           need cover art.
     *                           Rules for providers:
     *                           - Must be a direct downloadable URL; do NOT
     *                             return a redirect chain or data-URI.
     *                           - Layer-2 (music_client) downloads this URL as
     *                             part of one atomic public operation; Layer 1
     *                             callers never see or pass this URL around.
     *                           - Leave the buffer empty (buf[0]=0) if no
     *                             cover is available; this is not an error.
     *  @return 0 (MUSIC_OK) or a negative MUSIC_ERR_* code. */
    int (__stdcall *search_song)(
        MusicProviderHandle h,
        const wchar_t*      keyword,
        wchar_t*            out_song_id,
        int                 song_id_buf_wchars,
        wchar_t*            out_cover_url,          /* may be NULL */
        int                 cover_url_buf_wchars);

    /* --- comments ----------------------------------------- */

    /** Fetch one page of comments (single HTTP round-trip).
     *
     *  OFFSET SEMANTICS
     *  ----------------
     *  @param offset      Logical zero-based comment index.  The caller
     *                     advances it by the previously delivered count:
     *                       nextOffset += last_delivered
     *                     Providers that use page-number APIs internally
     *                     MUST compute their page number as:
     *                       page_num = offset / actual_page_size
     *                     where actual_page_size is the number the remote
     *                     API actually returns per request — NOT page_limit.
     *                     (These two differ when the API caps page size
     *                     below what the caller requested.)
     *
     *  END-OF-STREAM CONTRACT
     *  ----------------------
     *  - *out_delivered == 0 AND return == MUSIC_OK  →  end of stream.
     *    The caller will stop paging when it sees this.
     *  - *out_delivered < page_limit                 →  NOT end of stream.
     *    Providers are allowed to return fewer items than requested (e.g.
     *    because the remote API caps page size); callers MUST NOT treat a
     *    short page as the end of the comment stream.
     *
     *  OTHER RULES
     *  -----------
     *  - On the first page (offset == 0) emit hot/top comments before the
     *    regular list if the platform provides them.
     *  - Return MUSIC_OK even when 0 comments are delivered.
     *  - If the callback returns non-zero, stop early and return MUSIC_OK
     *    with *out_delivered set to the count emitted so far.
     *
     *  @param page_limit    Maximum comments the caller wants this call to
     *                       deliver.  Providers may deliver fewer (see above).
     *  @param out_delivered Set to the number of comments passed to callback. */
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
