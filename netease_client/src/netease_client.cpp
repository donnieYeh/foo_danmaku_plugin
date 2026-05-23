/* netease_client.cpp — exported C API layer */

#define NETEASE_CLIENT_EXPORTS
#include "../include/netease_client.h"
#include "http.h"
#include "api.h"
#include "json.h"
#include "logger.h"

#include <new>
#include <string>

/* Internal context owned by each handle */
struct NeteaseContext {
    netease::HttpClient  http;
    netease::ApiClient   api;
    std::wstring         last_error;

    NeteaseContext() : api(http) {}
};

/* ── helpers ─────────────────────────────────────────── */

static NeteaseContext* ctx(NeteaseHandle h) {
    return reinterpret_cast<NeteaseContext*>(h);
}

static void set_error(NeteaseContext* c, const std::wstring& msg) {
    if (c) c->last_error = msg;
}

/* ── lifecycle ───────────────────────────────────────── */

extern "C" {

NETEASE_API NeteaseHandle __stdcall netease_create(const wchar_t* cookie) {
    NeteaseContext* c = new (std::nothrow) NeteaseContext();
    if (!c) return nullptr;
    if (cookie && *cookie) {
        c->http.set_cookie(cookie);
    }
    return c;
}

NETEASE_API void __stdcall netease_destroy(NeteaseHandle h) {
    delete ctx(h);
}

/* ── search_song ─────────────────────────────────────── */

NETEASE_API int __stdcall netease_search_song(
    NeteaseHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_id,
    int            buf_wchars)
{
    if (!h || !keyword || !out_song_id || buf_wchars < 2)
        return NETEASE_ERR_PARAM;

    NeteaseContext* c = ctx(h);
    std::wstring err;
    std::wstring id = c->api.search_song(keyword, err);
    if (id.empty()) {
        set_error(c, err);
        return err.find(L"No songs") != std::wstring::npos
               ? NETEASE_ERR_NOTFOUND : NETEASE_ERR_NETWORK;
    }
    wcsncpy_s(out_song_id, buf_wchars, id.c_str(), _TRUNCATE);
    return NETEASE_OK;
}

NETEASE_API int __stdcall netease_search_song_with_cover(
    NeteaseHandle  h,
    const wchar_t* keyword,
    wchar_t*       out_song_id,
    int            song_id_buf_wchars,
    wchar_t*       out_cover_url,
    int            cover_url_buf_wchars)
{
    if (!h || !keyword || !out_song_id || song_id_buf_wchars < 2)
        return NETEASE_ERR_PARAM;

    if (out_cover_url && cover_url_buf_wchars > 0) out_cover_url[0] = 0;

    NeteaseContext* c = ctx(h);
    std::wstring err;
    netease::SongInfo info = c->api.search_song_info(keyword, err);
    if (info.id.empty()) {
        set_error(c, err);
        return err.find(L"No songs") != std::wstring::npos
               ? NETEASE_ERR_NOTFOUND : NETEASE_ERR_NETWORK;
    }
    wcsncpy_s(out_song_id, song_id_buf_wchars, info.id.c_str(), _TRUNCATE);
    if (out_cover_url && cover_url_buf_wchars > 0 && !info.cover_url.empty()) {
        wcsncpy_s(out_cover_url, cover_url_buf_wchars,
                  info.cover_url.c_str(), _TRUNCATE);
    }
    return NETEASE_OK;
}

/* ── get_comments_by_id ──────────────────────────────── */

NETEASE_API int __stdcall netease_get_comments_by_id(
    NeteaseHandle          h,
    const wchar_t*         song_id,
    int                    limit,
    NeteaseCommentCallback callback,
    void*                  userdata)
{
    if (!h || !song_id || !callback || limit <= 0)
        return NETEASE_ERR_PARAM;

    NeteaseContext* c = ctx(h);
    std::wstring err;

    int rc = c->api.get_comments(
        song_id, limit,
        [&](const netease::Comment& cm) -> bool {
            int ret = callback(cm.content.c_str(),
                               cm.nickname.c_str(),
                               cm.like_count,
                               userdata);
            return ret == 0; /* false = stop */
        },
        err);

    if (rc != NETEASE_OK) set_error(c, err);
    return rc;
}

/* ── get_comments_by_id_paged (single-page streaming) ── */

NETEASE_API int __stdcall netease_get_comments_by_id_paged(
    NeteaseHandle          h,
    const wchar_t*         song_id,
    int                    offset,
    int                    page_limit,
    NeteaseCommentCallback callback,
    void*                  userdata,
    int*                   out_delivered)
{
    if (out_delivered) *out_delivered = 0;
    if (!h || !song_id || !callback || page_limit <= 0 || offset < 0)
        return NETEASE_ERR_PARAM;

    NeteaseContext* c = ctx(h);
    std::wstring err;
    int delivered = 0;

    int rc = c->api.get_comments_page(
        song_id, offset, page_limit,
        [&](const netease::Comment& cm) -> bool {
            int ret = callback(cm.content.c_str(),
                               cm.nickname.c_str(),
                               cm.like_count,
                               userdata);
            return ret == 0; /* false = stop */
        },
        delivered, err);

    if (rc != NETEASE_OK) set_error(c, err);
    if (out_delivered) *out_delivered = delivered;
    return rc;
}

/* ── get_comments_by_keyword ─────────────────────────── */

NETEASE_API int __stdcall netease_get_comments_by_keyword(
    NeteaseHandle          h,
    const wchar_t*         song_name,
    const wchar_t*         artist,
    int                    limit,
    NeteaseCommentCallback callback,
    void*                  userdata)
{
    if (!h || !song_name || !callback || limit <= 0)
        return NETEASE_ERR_PARAM;

    NeteaseContext* c = ctx(h);
    std::wstring err;

    /* Build search keyword */
    std::wstring kw = song_name;
    if (artist && *artist) kw += L" " + std::wstring(artist);

    std::wstring song_id = c->api.search_song(kw, err);
    if (song_id.empty()) {
        set_error(c, err);
        return NETEASE_ERR_NOTFOUND;
    }

    return netease_get_comments_by_id(h, song_id.c_str(), limit, callback, userdata);
}

/* ── last_error ──────────────────────────────────────── */

NETEASE_API const wchar_t* __stdcall netease_last_error(NeteaseHandle h) {
    if (!h) return L"(null handle)";
    return ctx(h)->last_error.empty()
           ? L"(no error)"
           : ctx(h)->last_error.c_str();
}

NETEASE_API void __stdcall netease_set_global_log(NeteaseLogCallback cb,
                                                   void*              userdata)
{
    netease::g_log_cb       = cb;
    netease::g_log_userdata = userdata;
}

} /* extern "C" */
