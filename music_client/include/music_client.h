#ifndef MUSIC_CLIENT_H
#define MUSIC_CLIENT_H

/* music_client.h  —  Layer-2 static-library public API
 *
 * foo_danmaku.dll (Layer 1) statically links music_client.lib and calls only
 * the functions declared here.  It never includes provider-specific headers.
 *
 * music_client in turn loads platform provider DLLs (Layer 3) at runtime via
 * LoadLibraryW and dispatches through their MusicProviderVTable.
 *
 * Usage:
 *   MusicClientHandle h = music_client_create();
 *   music_client_load_provider(h, L"C:\\...\\netease_client.dll", NULL);
 *   music_client_set_log(h, my_log_fn, NULL);
 *
 *   MusicTrackQuery q = { L"晴天", L"周杰伦", NULL, 0 };
 *   MusicTrackSessionHandle s = NULL;
 *   music_client_open_track_session(h, &q, &s);
 *   music_client_track_next_comments(s, 100, cb, userdata, &n);
 *   music_client_close_track_session(s);
 *
 *   music_client_destroy(h);
 */

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── error codes ─────────────────────────────────────── */
#define MUSIC_OK              0
#define MUSIC_ERR_PARAM      -1   /* bad argument              */
#define MUSIC_ERR_NETWORK    -2   /* HTTP / connection failure */
#define MUSIC_ERR_CRYPTO     -3   /* encryption failure        */
#define MUSIC_ERR_NOTFOUND   -4   /* song not found            */
#define MUSIC_ERR_API        -5   /* server returned error     */
#define MUSIC_ERR_INTERNAL   -9   /* unexpected internal error */
#define MUSIC_ERR_NO_PROVIDER -10 /* no provider loaded        */

/* ── opaque handle ───────────────────────────────────── */
typedef void* MusicClientHandle;
typedef void* MusicTrackSessionHandle;

/** Raw metadata for one currently-playing track.
 *  Layer 1 provides only this platform-neutral information; Layer 2 resolves
 *  and caches provider-specific song IDs for the lifetime of a track session. */
typedef struct MusicTrackQuery {
    const wchar_t* title;       /* required */
    const wchar_t* artist;      /* optional */
    const wchar_t* album;       /* optional */
    int            duration_ms; /* optional; 0 if unknown */
} MusicTrackQuery;

/* ── callbacks ───────────────────────────────────────── */

/** Comment delivery.  Return 0 to continue, non-zero to stop early.
 *  Matches the __stdcall convention of the underlying provider contract so
 *  that existing __stdcall comment functions can be passed directly. */
typedef int (__stdcall *MusicClientCommentCallback)(
    const wchar_t* content,
    const wchar_t* nickname,
    int            like_count,
    void*          userdata);

/** Log sink.  Receives all internal log messages from music_client and the
 *  active provider(s).  msg is UTF-16, no trailing newline. */
typedef void (*MusicClientLogCallback)(
    const wchar_t* msg,
    void*          userdata);

/* ── lifecycle ───────────────────────────────────────── */

/** Create a client instance.  Returns NULL on allocation failure. */
MusicClientHandle music_client_create(void);

/** Destroy a client instance, unload all providers, free all resources. */
void              music_client_destroy(MusicClientHandle h);

/* ── provider loading ────────────────────────────────── */

/** Load and instantiate a provider DLL.
 *
 *  @param dll_path  Full path to the provider DLL (e.g., L"C:\\...\\netease_client.dll").
 *                   If only a filename is supplied, standard DLL search order applies.
 *  @param cookie    Optional cookie string passed to the provider's create().
 *                   Pass NULL for anonymous access.
 *  @return MUSIC_OK on success, MUSIC_ERR_INTERNAL on failure.
 *          Call music_client_last_error() for a human-readable reason. */
int music_client_load_provider(
    MusicClientHandle h,
    const wchar_t*    dll_path,
    const wchar_t*    cookie);

/* ── provider introspection ─────────────────────────── */

/** Number of providers currently loaded (0 if none). */
int music_client_get_provider_count(MusicClientHandle h);

/** Full DLL path of the provider at position @p index.
 *  Returns NULL if @p index is out of range.
 *  Pointer valid until the provider list is modified. */
const wchar_t* music_client_get_provider_path(MusicClientHandle h, int index);

/** Reorder loaded providers.
 *  @p new_order[i] = current index of the provider to place at position i.
 *  @p count must equal music_client_get_provider_count().
 *  After reordering, search falls back through providers in the new order.
 *  Returns MUSIC_OK or MUSIC_ERR_PARAM. */
int music_client_reorder_providers(
    MusicClientHandle h,
    const int*        new_order,
    int               count);

/* ── track session ───────────────────────────────────── */

/** Open an ephemeral Layer-2 session for one currently-playing track.
 *
 *  The session copies @p query and maintains non-persistent mappings from the
 *  track's raw metadata to provider-specific song IDs.  Close it when playback
 *  moves to another track.
 */
int music_client_open_track_session(
    MusicClientHandle        h,
    const MusicTrackQuery*   query,
    MusicTrackSessionHandle* out_session);

/** Release a track session and its in-memory provider mappings. */
void music_client_close_track_session(MusicTrackSessionHandle session);

/** Fetch the next page of comments for a track session.
 *
 *  Layer 1 does not pass song IDs or offsets.  Layer 2 resolves provider IDs,
 *  caches them in the session, advances offsets internally, and can fall back
 *  to another provider if the initial provider cannot deliver the first page.
 *
 *  End-of-stream: MUSIC_OK with *out_delivered == 0.
 */
int music_client_track_next_comments(
    MusicTrackSessionHandle     session,
    int                         page_limit,
    MusicClientCommentCallback  callback,
    void*                       userdata,
    int*                        out_delivered);

/** Fetch cover art for a track session.
 *
 *  Reuses the same per-track provider resolution cache as comments.  If a
 *  provider was already resolved for comments, this does not issue another
 *  provider search just to discover its cover URL.
 */
int music_client_track_fetch_cover(
    MusicTrackSessionHandle session,
    void**                  out_data,
    int*                    out_size);

/* ── search ──────────────────────────────────────────── */

/** Search for a song and return its platform ID.
 *
 *  If @p out_cover_data is non-NULL the cover-art image is fetched from the
 *  provider's CDN and returned as raw bytes (JPEG/PNG).  The caller must free
 *  the buffer with music_client_free().  Cover download failure is non-fatal:
 *  MUSIC_OK is still returned when the song was found; *out_cover_data will
 *  simply be NULL.  Pass out_cover_data=NULL to skip the cover download.
 *
 *  @return MUSIC_OK, MUSIC_ERR_NOTFOUND, or another MUSIC_ERR_* code. */
int music_client_search_song(
    MusicClientHandle h,
    const wchar_t*    keyword,
    wchar_t*          out_song_id,
    int               song_id_buf_wchars,
    void**            out_cover_data,   /* may be NULL; free with music_client_free */
    int*              out_cover_size);  /* may be NULL */

/* ── comments ────────────────────────────────────────── */

/** Fetch one page of comments (one HTTP round-trip).
 *
 *  End-of-stream: MUSIC_OK with *out_delivered == 0.
 *  A short page (*out_delivered < page_limit) is NOT end-of-stream;
 *  callers must keep paging until they receive 0 delivered.
 *
 *  @return MUSIC_OK on success (even if *out_delivered == 0). */
int music_client_get_comments_paged(
    MusicClientHandle            h,
    const wchar_t*               song_id,
    int                          offset,
    int                          page_limit,
    MusicClientCommentCallback   callback,
    void*                        userdata,
    int*                         out_delivered);

/* ── cover fetch ─────────────────────────────────────── */

/** Atomically fetch cover art for a song keyword.
 *
 *  This is the cover-only variant of music_client_search_song(): callers pass
 *  a music keyword and receive raw image bytes.  The intermediate provider
 *  metadata and CDN URL are an implementation detail and are never exposed as
 *  part of the public contract.
 *
 *  On success *out_data is a heap-allocated buffer of *out_size bytes.
 *  The caller must free it with music_client_free().
 *
 *  @return MUSIC_OK, MUSIC_ERR_NOTFOUND, or another MUSIC_ERR_* code. */
int  music_client_fetch_cover(
    MusicClientHandle h,
    const wchar_t*    keyword,
    void**            out_data,
    int*              out_size);

/** Low-level byte downloader.
 *
 *  Deprecated for cover-art use: prefer music_client_fetch_cover() or the
 *  out_cover_data parameter of music_client_search_song(), both of which make
 *  cover fetching an atomic operation from the caller's point of view.
 */
int  music_client_download_bytes(
    MusicClientHandle h,
    const wchar_t*    url,
    void**            out_data,
    int*              out_size);

/** Free a buffer returned by music_client_search_song()/music_client_fetch_cover(). */
void music_client_free(void* ptr);

/* ── diagnostics ─────────────────────────────────────── */

/** Last error description (valid until the next call on this handle).
 *  Never returns NULL. */
const wchar_t* music_client_last_error(MusicClientHandle h);

/** Set a log sink for internal diagnostics.  Pass cb=NULL to clear.
 *  Safe to call before music_client_load_provider(). */
void music_client_set_log(
    MusicClientHandle     h,
    MusicClientLogCallback cb,
    void*                 userdata);

/** Set the API key used for DeepSeek song search fallback. */
void music_client_set_deepseek_api_key(
    MusicClientHandle h,
    const char*       api_key);

/** Clear the local DeepSeek cache (both in-memory and deepseek_cache.json). */
void music_client_clear_deepseek_cache(MusicClientHandle h);


#ifdef __cplusplus
}
#endif
#endif /* MUSIC_CLIENT_H */
