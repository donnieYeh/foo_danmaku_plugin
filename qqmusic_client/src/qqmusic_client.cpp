/* qqmusic_client.cpp — exported C API layer
 * QQMUSIC_CLIENT_EXPORTS is defined by build.ps1 for the entire DLL build,
 * so all TUs (including provider_entry.cpp) emit __declspec(dllexport). */
#include "../include/qqmusic_client.h"
#include "http.h"
#include "api.h"
#include "json.h"
#include "logger.h"

#include <new>
#include <string>

/* Internal context owned by each handle */
struct QQMusicContext {
    qqmusic::HttpClient  http;
    qqmusic::ApiClient   api;
    std::wstring         last_error;

    QQMusicContext() : api(http) {}
};

/* ── helpers ─────────────────────────────────────────── */

static QQMusicContext* ctx(QQMusicHandle h) {
    return reinterpret_cast<QQMusicContext*>(h);
}

static void set_error(QQMusicContext* c, const std::wstring& msg) {
    if (c) c->last_error = msg;
}

/* ── lifecycle ───────────────────────────────────────── */

extern "C" {

QQMUSIC_API QQMusicHandle __stdcall qqmusic_create(const wchar_t* cookie) {
    QQMusicContext* c = new (std::nothrow) QQMusicContext();
    if (!c) return nullptr;
    if (cookie && *cookie) {
        c->http.set_cookie(cookie);
        c->api.set_cookie(cookie);
    }
    return c;
}

QQMUSIC_API void __stdcall qqmusic_destroy(QQMusicHandle h) {
    delete ctx(h);
}

/* ── search_song ─────────────────────────────────────── */

QQMUSIC_API int __stdcall qqmusic_search_song(
    QQMusicHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_mid,
    int            buf_wchars)
{
    if (!h || !keyword || !out_song_mid || buf_wchars < 2)
        return QQMUSIC_ERR_PARAM;

    QQMusicContext* c = ctx(h);
    std::wstring err;
    std::wstring mid = c->api.search_song(keyword, err);
    if (mid.empty()) {
        set_error(c, err);
        return err.find(L"No songs") != std::wstring::npos
               ? QQMUSIC_ERR_NOTFOUND : QQMUSIC_ERR_NETWORK;
    }
    wcsncpy_s(out_song_mid, buf_wchars, mid.c_str(), _TRUNCATE);
    return QQMUSIC_OK;
}

QQMUSIC_API int __stdcall qqmusic_search_song_with_cover(
    QQMusicHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_mid,
    int            song_mid_buf_wchars,
    wchar_t*       out_cover_url,
    int            cover_url_buf_wchars)
{
    if (!h || !keyword || !out_song_mid || song_mid_buf_wchars < 2)
        return QQMUSIC_ERR_PARAM;

    if (out_cover_url && cover_url_buf_wchars > 0) out_cover_url[0] = 0;

    QQMusicContext* c = ctx(h);
    std::wstring err;
    qqmusic::SongInfo info = c->api.search_song_info(keyword, err);
    if (info.id.empty()) {
        set_error(c, err);
        return err.find(L"No songs") != std::wstring::npos
               ? QQMUSIC_ERR_NOTFOUND : QQMUSIC_ERR_NETWORK;
    }
    /* out_song_mid receives the numeric songid (used as topid in comments) */
    wcsncpy_s(out_song_mid, song_mid_buf_wchars, info.id.c_str(), _TRUNCATE);
    if (out_cover_url && cover_url_buf_wchars > 0 && !info.cover_url.empty()) {
        wcsncpy_s(out_cover_url, cover_url_buf_wchars,
                  info.cover_url.c_str(), _TRUNCATE);
    }
    return QQMUSIC_OK;
}

/* ── get_comments_by_id ──────────────────────────────── */

QQMUSIC_API int __stdcall qqmusic_get_comments_by_id(
    QQMusicHandle           h,
    const wchar_t*          song_mid,
    int                     limit,
    QQMusicCommentCallback  callback,
    void*                   userdata)
{
    if (!h || !song_mid || !callback || limit <= 0)
        return QQMUSIC_ERR_PARAM;

    QQMusicContext* c = ctx(h);
    std::wstring err;

    int rc = c->api.get_comments(
        song_mid, limit,
        [&](const qqmusic::Comment& cm) -> bool {
            int ret = callback(cm.content.c_str(),
                               cm.nickname.c_str(),
                               cm.like_count,
                               userdata);
            return ret == 0; /* false = stop */
        },
        err);

    if (rc != QQMUSIC_OK) set_error(c, err);
    return rc;
}

/* ── get_comments_by_id_paged ────────────────────────── */

QQMUSIC_API int __stdcall qqmusic_get_comments_by_id_paged(
    QQMusicHandle           h,
    const wchar_t*          song_mid,
    int                     offset,
    int                     page_limit,
    QQMusicCommentCallback  callback,
    void*                   userdata,
    int*                    out_delivered)
{
    if (out_delivered) *out_delivered = 0;
    if (!h || !song_mid || !callback || page_limit <= 0 || offset < 0)
        return QQMUSIC_ERR_PARAM;

    QQMusicContext* c = ctx(h);
    std::wstring err;
    int delivered = 0;

    int rc = c->api.get_comments_page(
        song_mid, offset, page_limit,
        [&](const qqmusic::Comment& cm) -> bool {
            int ret = callback(cm.content.c_str(),
                               cm.nickname.c_str(),
                               cm.like_count,
                               userdata);
            return ret == 0;
        },
        delivered, err);

    if (rc != QQMUSIC_OK) set_error(c, err);
    if (out_delivered) *out_delivered = delivered;
    return rc;
}

/* ── get_comments_by_keyword ─────────────────────────── */

QQMUSIC_API int __stdcall qqmusic_get_comments_by_keyword(
    QQMusicHandle           h,
    const wchar_t*          song_name,
    const wchar_t*          artist,
    int                     limit,
    QQMusicCommentCallback  callback,
    void*                   userdata)
{
    if (!h || !song_name || !callback || limit <= 0)
        return QQMUSIC_ERR_PARAM;

    QQMusicContext* c = ctx(h);
    std::wstring err;

    /* Build search keyword */
    std::wstring kw = song_name;
    if (artist && *artist) kw += L" " + std::wstring(artist);

    std::wstring song_mid = c->api.search_song(kw, err);
    if (song_mid.empty()) {
        set_error(c, err);
        return QQMUSIC_ERR_NOTFOUND;
    }

    return qqmusic_get_comments_by_id(h, song_mid.c_str(), limit, callback, userdata);
}

/* ── last_error ──────────────────────────────────────── */

QQMUSIC_API const wchar_t* __stdcall qqmusic_last_error(QQMusicHandle h) {
    if (!h) return L"(null handle)";
    return ctx(h)->last_error.empty()
           ? L"(no error)"
           : ctx(h)->last_error.c_str();
}

QQMUSIC_API void __stdcall qqmusic_set_global_log(QQMusicLogCallback cb,
                                                   void*              userdata)
{
    qqmusic::g_log_cb       = cb;
    qqmusic::g_log_userdata = userdata;
}

} /* extern "C" */
