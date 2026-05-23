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
 *   wchar_t song_id[64], cover_url[1024];
 *   music_client_search_song(h, L"晴天 周杰伦", song_id, 64, cover_url, 1024);
 *
 *   music_client_get_comments_paged(h, song_id, 0, 100, cb, userdata, &n);
 *
 *   void* img; int img_sz;
 *   music_client_download_bytes(h, cover_url, &img, &img_sz);
 *   // … decode img with GDI+ …
 *   music_client_free(img);
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

/* ── search ──────────────────────────────────────────── */

/** Search for a song and return its platform ID plus optional cover-art URL.
 *  @return MUSIC_OK, MUSIC_ERR_NOTFOUND, or another MUSIC_ERR_* code. */
int music_client_search_song(
    MusicClientHandle h,
    const wchar_t*    keyword,
    wchar_t*          out_song_id,
    int               song_id_buf_wchars,
    wchar_t*          out_cover_url,      /* may be NULL */
    int               cover_url_buf_wchars);

/* ── comments ────────────────────────────────────────── */

/** Fetch one page of comments (one HTTP round-trip).
 *  See MusicProviderVTable::get_comments_paged for semantics.
 *  @return MUSIC_OK on success (even if out_delivered == 0). */
int music_client_get_comments_paged(
    MusicClientHandle            h,
    const wchar_t*               song_id,
    int                          offset,
    int                          page_limit,
    MusicClientCommentCallback   callback,
    void*                        userdata,
    int*                         out_delivered);

/* ── cover download ──────────────────────────────────── */

/** Download raw image bytes from a URL (e.g., a cover-art URL returned by
 *  music_client_search_song).
 *
 *  On success *out_data is a heap-allocated buffer of *out_size bytes.
 *  The caller must free it with music_client_free().
 *
 *  @return MUSIC_OK or MUSIC_ERR_NETWORK. */
int  music_client_download_bytes(
    MusicClientHandle h,
    const wchar_t*    url,
    void**            out_data,
    int*              out_size);

/** Free a buffer returned by music_client_download_bytes(). */
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

#ifdef __cplusplus
}
#endif
#endif /* MUSIC_CLIENT_H */
