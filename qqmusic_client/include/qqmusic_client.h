#ifndef QQMUSIC_CLIENT_H
#define QQMUSIC_CLIENT_H

/* qqmusic_client.h — public C API (ABI-stable, no C++ types on boundary)
 *
 * Caller pattern:
 *   QQMusicHandle h = qqmusic_create(NULL);
 *   qqmusic_get_comments_by_keyword(h, L"晴天", L"周杰伦", 50, cb, userdata);
 *   qqmusic_destroy(h);
 *
 * Song IDs returned by qqmusic_search_song* are NUMERIC strings (e.g. "97773").
 * This is the numeric songid used as "topid" in the comments API.
 * Do NOT confuse with the alphanumeric songmid (e.g. "001OLkXf2nqxZ9").
 *
 * Anonymous access (cookie=NULL) works for most public songs.
 * For authenticated access, pass the raw QQ Music cookie string:
 *   L"uin=o1234567890; p_skey=XXXXX; skey=YYYYY; ..."
 */

#ifdef QQMUSIC_CLIENT_EXPORTS
#  define QQMUSIC_API __declspec(dllexport)
#else
#  define QQMUSIC_API __declspec(dllimport)
#endif

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── error codes ─────────────────────────────────────── */
#define QQMUSIC_OK             0
#define QQMUSIC_ERR_PARAM     -1   /* bad argument              */
#define QQMUSIC_ERR_NETWORK   -2   /* HTTP / connection failure */
#define QQMUSIC_ERR_NOTFOUND  -4   /* song not found            */
#define QQMUSIC_ERR_API       -5   /* server returned error     */
#define QQMUSIC_ERR_INTERNAL  -9   /* unexpected internal error */

/* ── opaque handle ───────────────────────────────────── */
typedef void* QQMusicHandle;

/* ── comment callback ────────────────────────────────── */
/* Return 0 to continue, non-zero to stop early. */
typedef int (__stdcall *QQMusicCommentCallback)(
    const wchar_t* content,
    const wchar_t* nickname,
    int            like_count,
    void*          userdata
);

/* ── lifecycle ───────────────────────────────────────── */

/**
 * Create a client instance.
 * @param cookie  Optional raw QQ Music cookie string.
 *                Pass NULL for anonymous access (works for most songs).
 * @return Non-NULL handle on success, NULL on allocation failure.
 */
QQMUSIC_API QQMusicHandle __stdcall qqmusic_create(const wchar_t* cookie);

/** Destroy handle and release all resources. */
QQMUSIC_API void __stdcall qqmusic_destroy(QQMusicHandle h);

/* ── search ──────────────────────────────────────────── */

/**
 * Search a song, write its songmid into out_song_mid.
 * @param out_song_mid  Caller-allocated buffer (recommend 32 wchars).
 * @param buf_wchars    Buffer size in wchar_t units.
 * @return QQMUSIC_OK, QQMUSIC_ERR_NOTFOUND, or another QQMUSIC_ERR_* code.
 */
QQMUSIC_API int __stdcall qqmusic_search_song(
    QQMusicHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_mid,
    int            buf_wchars
);

/**
 * Search a song and also return its cover art URL.
 * All output buffers are caller-allocated. out_cover_url may be NULL.
 * @param out_song_mid        Alphanumeric song MID, e.g. "001OLkXf2nqxZ9"
 * @param out_cover_url       Full HTTPS URL of the album art (300x300 JPEG).
 */
QQMUSIC_API int __stdcall qqmusic_search_song_with_cover(
    QQMusicHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_mid,
    int            song_mid_buf_wchars,
    wchar_t*       out_cover_url,
    int            cover_url_buf_wchars
);

/* ── comments ────────────────────────────────────────── */

/**
 * Fetch up to 'limit' comments for the given songmid.
 * Internally paginates; hot/featured comments are emitted first.
 */
QQMUSIC_API int __stdcall qqmusic_get_comments_by_id(
    QQMusicHandle           h,
    const wchar_t*          song_mid,
    int                     limit,
    QQMusicCommentCallback  callback,
    void*                   userdata
);

/**
 * Fetch exactly one page of comments (single HTTP request).
 *
 * @param offset          0-based starting offset (must be a multiple of page_limit).
 * @param page_limit      Page size, 1..100 (clamped server-side).
 * @param out_delivered   Number of comments actually delivered to the callback.
 *
 * Returns QQMUSIC_OK on success (even if out_delivered == 0 = end of stream).
 * On the first page (offset == 0) hot comments are emitted before the regular list.
 */
QQMUSIC_API int __stdcall qqmusic_get_comments_by_id_paged(
    QQMusicHandle           h,
    const wchar_t*          song_mid,
    int                     offset,
    int                     page_limit,
    QQMusicCommentCallback  callback,
    void*                   userdata,
    int*                    out_delivered
);

/**
 * Search a song by name + artist, then fetch comments.
 * Calls callback once per comment until limit or end.
 */
QQMUSIC_API int __stdcall qqmusic_get_comments_by_keyword(
    QQMusicHandle           h,
    const wchar_t*          song_name,
    const wchar_t*          artist,       /* may be NULL */
    int                     limit,
    QQMusicCommentCallback  callback,
    void*                   userdata
);

/* ── diagnostics ─────────────────────────────────────── */

/**
 * Return last error description (valid until next call on this handle).
 * Never returns NULL.
 */
QQMUSIC_API const wchar_t* __stdcall qqmusic_last_error(QQMusicHandle h);

/**
 * Set a global log sink. All internal log lines are forwarded here
 * IN ADDITION to OutputDebugString.
 * Pass cb=NULL to clear.  msg is UTF-16, no trailing newline.
 * Safe to call before qqmusic_create().
 */
typedef void (__stdcall *QQMusicLogCallback)(const wchar_t* msg, void* userdata);
QQMUSIC_API void __stdcall qqmusic_set_global_log(QQMusicLogCallback cb,
                                                   void*              userdata);

#ifdef __cplusplus
}
#endif
#endif /* QQMUSIC_CLIENT_H */
