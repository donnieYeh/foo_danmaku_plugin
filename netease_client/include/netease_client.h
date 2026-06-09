#ifndef NETEASE_CLIENT_H
#define NETEASE_CLIENT_H

/* netease_client.h — public C API (ABI-stable, no C++ types on boundary)
 *
 * Caller pattern:
 *   NeteaseHandle h = netease_create(NULL);
 *   netease_get_comments_by_keyword(h, L"晴天", L"周杰伦", 50, cb, userdata);
 *   netease_destroy(h);
 */

#ifdef NETEASE_CLIENT_EXPORTS
#  define NETEASE_API __declspec(dllexport)
#else
#  define NETEASE_API __declspec(dllimport)
#endif

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── error codes ─────────────────────────────────────── */
#define NETEASE_OK             0
#define NETEASE_ERR_PARAM     -1   /* bad argument             */
#define NETEASE_ERR_NETWORK   -2   /* HTTP/connection failure  */
#define NETEASE_ERR_CRYPTO    -3   /* encryption failure       */
#define NETEASE_ERR_NOTFOUND  -4   /* song not found           */
#define NETEASE_ERR_API       -5   /* server returned error    */
#define NETEASE_ERR_INTERNAL  -9   /* unexpected internal error*/

/* ── opaque handle ───────────────────────────────────── */
typedef void* NeteaseHandle;

/* ── comment callback ────────────────────────────────── */
/* Return 0 to continue, non-zero to stop early. */
typedef int (__stdcall *NeteaseCommentCallback)(
    const wchar_t* content,
    const wchar_t* nickname,
    int            like_count,
    void*          userdata
);

/* ── lifecycle ───────────────────────────────────────── */

/**
 * Create a client instance.
 * @param cookie  Optional raw cookie string (L"MUSIC_U=…;__csrf=…").
 *                Pass NULL for anonymous access (works for most songs).
 * @return Non-NULL handle on success, NULL on failure.
 */
NETEASE_API NeteaseHandle __stdcall netease_create(const wchar_t* cookie);

/** Destroy handle and release all resources. */
NETEASE_API void __stdcall netease_destroy(NeteaseHandle h);

/* ── core functions ──────────────────────────────────── */

/**
 * Search song by name + artist, then fetch comments.
 * Calls callback once per comment until limit or end.
 */
NETEASE_API int __stdcall netease_get_comments_by_keyword(
    NeteaseHandle          h,
    const wchar_t*         song_name,
    const wchar_t*         artist,       /* may be NULL */
    int                    limit,
    NeteaseCommentCallback callback,
    void*                  userdata
);

/**
 * Fetch comments directly by numeric song ID string.
 */
NETEASE_API int __stdcall netease_get_comments_by_id(
    NeteaseHandle          h,
    const wchar_t*         song_id,
    int                    limit,
    NeteaseCommentCallback callback,
    void*                  userdata
);

/**
 * Fetch exactly one page of comments (single HTTP request).
 * Use this for streaming/pagination from a caller-managed offset.
 *
 * @param offset          0-based starting offset, in comments.
 * @param page_limit      Page size, 1..100 (will be clamped).
 * @param out_delivered   Number of comments actually delivered to the callback
 *                        for this page (may be 0 = end of stream).
 *
 * Returns NETEASE_OK on success (even if 0 comments). Returns NETEASE_ERR_* on
 * failure. On the first page (offset==0) hotComments are emitted before the
 * regular list.
 */
NETEASE_API int __stdcall netease_get_comments_by_id_paged(
    NeteaseHandle          h,
    const wchar_t*         song_id,
    int                    offset,
    int                    page_limit,
    NeteaseCommentCallback callback,
    void*                  userdata,
    int*                   out_delivered
);

/**
 * Search a song, write its ID into out_song_id.
 * @param out_song_id  Caller-allocated buffer (recommend 32 wchars).
 * @param buf_wchars   Buffer size in wchar_t units.
 */
NETEASE_API int __stdcall netease_search_song(
    NeteaseHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_id,
    int            buf_wchars
);

/**
 * Search a song and also return its NetEase album cover URL when available.
 * All output buffers are caller-allocated. out_cover_url may be NULL.
 */
NETEASE_API int __stdcall netease_search_song_with_cover(
    NeteaseHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_id,
    int            song_id_buf_wchars,
    wchar_t*       out_cover_url,
    int            cover_url_buf_wchars
);

/**
 * Structured search variant for callers that know field boundaries.
 * duration_ms may be 0 when unknown. ABI-compatible optional export.
 */
NETEASE_API int __stdcall netease_search_track_with_cover(
    NeteaseHandle  h,
    const wchar_t* title,
    const wchar_t* artist,
    const wchar_t* album,
    int            duration_ms,
    wchar_t*       out_song_id,
    int            song_id_buf_wchars,
    wchar_t*       out_cover_url,
    int            cover_url_buf_wchars
);

/* ── diagnostics ─────────────────────────────────────── */

/**
 * Return last error description (valid until next call on this handle).
 * Never returns NULL.
 */
NETEASE_API const wchar_t* __stdcall netease_last_error(NeteaseHandle h);

/**
 * Set a global log sink. All internal log lines are forwarded here
 * IN ADDITION to OutputDebugString.
 * Pass cb=NULL to clear.  msg is UTF-16, no trailing newline.
 * Safe to call before netease_create().
 */
typedef void (__stdcall *NeteaseLogCallback)(const wchar_t* msg, void* userdata);
NETEASE_API void __stdcall netease_set_global_log(NeteaseLogCallback cb,
                                                   void*              userdata);

#ifdef __cplusplus
}
#endif
#endif /* NETEASE_CLIENT_H */
