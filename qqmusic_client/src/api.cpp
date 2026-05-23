/* api.cpp — QQ Music search + comments
 *
 * Endpoints (all HTTPS on c.y.qq.com):
 *
 *   Search  : GET /soso/fcgi-bin/client_search_cp
 *             ?w={keyword}&p=1&n=10&format=json&inCharset=utf-8
 *              &outCharset=utf-8&notice=0&platform=yqq.json&needNewCode=0
 *
 *   Comments: GET /base/fcgi-bin/fcg_global_comment_h5.fcg
 *             ?g_tk={g_tk}&loginUin={uin}&...&topid={songmid}
 *              &pagenum={page}&pagesize={size}
 *
 * Song IDs are the alphanumeric "songmid" (e.g. "001OLkXf2nqxZ9"),
 * which is what the comments endpoint requires as "topid".
 */

#include "api.h"
#include "json.h"
#include "logger.h"
#include "../include/qqmusic_client.h"

#include <windows.h>
#include <algorithm>
#include <string>

namespace qqmusic {

/* ── g_tk computation ────────────────────────────────── */
/*
 * g_tk = Jenkins-like hash of the cookie's p_skey (preferred) or skey.
 * For anonymous access (no cookie) the initial value 5381 is used directly.
 */
static int compute_g_tk(const std::wstring& key) {
    unsigned int hash = 5381u;
    for (wchar_t c : key) {
        hash += (hash << 5) + (unsigned int)(unsigned short)c;
    }
    return (int)(hash & 0x7FFFFFFFu);
}

/* Extract a named field from a wide cookie string, e.g.
 * L"uin=o1234567890; p_skey=ABCDEF" → extract("p_skey") → L"ABCDEF"
 */
static std::wstring extract_cookie_field(const std::wstring& cookie,
                                         const std::wstring& name)
{
    std::wstring pat = name + L"=";
    size_t pos = cookie.find(pat);
    if (pos == std::wstring::npos) return {};
    pos += pat.size();
    size_t end = cookie.find(L';', pos);
    std::wstring val = (end == std::wstring::npos)
                       ? cookie.substr(pos)
                       : cookie.substr(pos, end - pos);
    /* trim spaces */
    size_t s = val.find_first_not_of(L' ');
    size_t e = val.find_last_not_of(L' ');
    return (s == std::wstring::npos) ? L"" : val.substr(s, e - s + 1);
}

/* ── constructor ─────────────────────────────────────── */

ApiClient::ApiClient(HttpClient& http) : m_http(http) {}

void ApiClient::set_cookie(const std::wstring& cookie) {
    parse_cookie_fields(cookie);
}

void ApiClient::parse_cookie_fields(const std::wstring& cookie) {
    /* Prefer p_skey over skey for g_tk computation */
    std::wstring key = extract_cookie_field(cookie, L"p_skey");
    if (key.empty()) key = extract_cookie_field(cookie, L"skey");

    m_g_tk = key.empty() ? 5381 : compute_g_tk(key);
    loga("g_tk=" + std::to_string(m_g_tk));

    /* Extract numeric uin (may have leading 'o') */
    std::wstring uin = extract_cookie_field(cookie, L"uin");
    if (!uin.empty() && uin[0] == L'o') uin = uin.substr(1);
    m_login_uin = uin.empty() ? L"0" : uin;
}

/* ── URL encode UTF-8 string ─────────────────────────── */

static std::string url_encode_utf8(const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

/* ── check_api_code: returns true on success ─────────── */

static bool check_api_code(const std::string& resp,
                            std::wstring&      error_msg)
{
    if (resp.empty()) { error_msg = L"Empty response"; return false; }
    long long code = json::num(resp, "code", -1);
    if (code != 0) {
        error_msg = L"QQ Music API error code=" + std::to_wstring(code);
        /* Try to get a message */
        std::string msg = json::str(resp, "message");
        if (msg.empty()) msg = json::str(resp, "msg");
        if (!msg.empty()) error_msg += L": " + json::to_wide(msg);
        log(L"check_api_code: " + error_msg);
        return false;
    }
    return true;
}

/* ── search_song_info ────────────────────────────────── */

SongInfo ApiClient::search_song_info(
    const std::wstring& keyword,
    std::wstring&       error_msg)
{
    SongInfo info;
    std::string kw_utf8    = json::to_utf8(keyword);
    std::string kw_encoded = url_encode_utf8(kw_utf8);
    loga("search_song keyword: " + kw_utf8);

    /* Build path + query string (all pure ASCII after encoding) */
    std::wstring path =
        L"/soso/fcgi-bin/client_search_cp"
        L"?w="         + std::wstring(kw_encoded.begin(), kw_encoded.end()) +
        L"&p=1&n=10"
        L"&format=json&inCharset=utf-8&outCharset=utf-8"
        L"&notice=0&platform=yqq.json&needNewCode=0"
        L"&ct=24&cv=4747474";

    std::string raw;
    if (!m_http.get(path, raw, error_msg)) {
        log(L"search_song HTTP failed: " + error_msg);
        return info;
    }
    if (!check_api_code(raw, error_msg)) return info;

    /* Navigate: data → song → list → [0] */
    std::string data_blk = json::object_raw(raw, "data");
    if (data_blk.empty()) {
        error_msg = L"search: missing 'data' in response";
        log(error_msg);
        return info;
    }

    std::string song_blk = json::object_raw(data_blk, "song");
    if (song_blk.empty()) {
        error_msg = L"search: missing 'song' in data";
        log(error_msg);
        return info;
    }

    std::string list_raw = json::array_raw(song_blk, "list");
    if (list_raw.empty()) {
        error_msg = L"search: 'list' array missing or empty";
        log(error_msg);
        return info;
    }

    auto songs = json::array_items(list_raw);
    if (songs.empty()) {
        error_msg = L"No songs found for: " + keyword;
        loga("search_song: no items in list array");
        return info;
    }

    const std::string& first = songs[0];

    /* Extract numeric songid — this is used as topid in the comments API */
    long long id_num = json::num(first, "songid", 0);
    if (id_num == 0) {
        error_msg = L"Failed to parse numeric songid";
        log(error_msg);
        return info;
    }
    info.id = std::to_wstring(id_num);
    log(L"search_song: songid=" + info.id);

    /* Also extract alphanumeric songmid (for reference / caller info) */
    std::string mid = json::str(first, "songmid");
    info.mid = json::to_wide(mid);
    if (!info.mid.empty())
        log(L"search_song: songmid=" + info.mid);

    /* Extract albummid for cover URL */
    std::string album_mid = json::str(first, "albummid");
    if (album_mid.empty()) {
        /* Try nested: "album":{"mid":"..."} */
        std::string album_blk = json::object_raw(first, "album");
        if (!album_blk.empty())
            album_mid = json::str(album_blk, "mid");
    }
    if (!album_mid.empty()) {
        /* Standard QQ Music CDN pattern: T002R300x300M000{albummid}_1.jpg */
        info.cover_url = L"https://y.gtimg.cn/music/photo_new/T002R300x300M000"
                       + json::to_wide(album_mid)
                       + L"_1.jpg";
        log(L"search_song: cover_url=" + info.cover_url);
    }

    return info;
}

std::wstring ApiClient::search_song(
    const std::wstring& keyword,
    std::wstring&       error_msg)
{
    /* Return numeric songid — this is what the comments API uses as topid */
    return search_song_info(keyword, error_msg).id;
}

/* ── emit_comment_list ───────────────────────────────── */

bool ApiClient::emit_comment_list(
    const std::string& arr_raw,
    int                max_emit,
    int&               fetched,
    CommentVisitor&    visitor)
{
    if (arr_raw.empty()) return false;
    auto items = json::array_items(arr_raw);
    loga("  emit_comment_list: " + std::to_string(items.size()) + " items");

    for (const auto& item : items) {
        Comment c;
        /* fcg_global_comment_h5.fcg uses "rootcommentcontent";
         * fall back to "content" for any other endpoints. */
        std::string body = json::str(item, "rootcommentcontent");
        if (body.empty()) body = json::str(item, "content");
        c.content    = json::to_wide(body);
        c.nickname   = json::to_wide(json::str(item, "nick"));
        c.like_count = (int)json::num(item, "praisenum", 0);

        if (!visitor(c)) return true; /* caller stopped */
        ++fetched;
        if (fetched >= max_emit) return true;
    }
    return false;
}

/* ── get_comments_page ───────────────────────────────── */

int ApiClient::get_comments_page(
    const std::wstring& song_mid,
    int                 offset,
    int                 limit,
    CommentVisitor      visitor,
    int&                out_delivered,
    std::wstring&       error_msg)
{
    out_delivered = 0;
    /* QQ Music's fcg_global_comment_h5 consistently returns 10 comments per
     * page for anonymous access regardless of the 'pagesize' request field.
     * Fix kQQApiPageSize so page_num = offset/10 keeps correct page alignment
     * as the streaming worker advances offset by the delivered count. */
    const int kQQApiPageSize = 10;
    int page_size = std::min(std::max(limit, 1), kQQApiPageSize);
    int page_num  = offset / kQQApiPageSize;

    loga("get_comments_page mid=" + json::to_utf8(song_mid)
       + " offset=" + std::to_string(offset)
       + " pagenum=" + std::to_string(page_num)
       + " pagesize=" + std::to_string(page_size));

    /* Build query path */
    std::wstring path =
        L"/base/fcgi-bin/fcg_global_comment_h5.fcg"
        L"?g_tk="      + std::to_wstring(m_g_tk)    +
        L"&loginUin="  + m_login_uin                  +
        L"&hostUin=0"
        L"&format=json&inCharset=utf-8&outCharset=utf-8"
        L"&notice=0&platform=yqq.json&needNewCode=0"
        L"&cid=205360772&reqtype=2&biztype=1"
        L"&topid="     + song_mid                    +
        L"&cmd=8&needmusiccrit=0"
        L"&pagenum="   + std::to_wstring(page_num)   +
        L"&pagesize="  + std::to_wstring(page_size)  +
        L"&lasttime=0&ct=24&cv=4747474";

    std::string raw;
    if (!m_http.get(path, raw, error_msg)) {
        log(L"get_comments_page HTTP failed: " + error_msg);
        return QQMUSIC_ERR_NETWORK;
    }
    if (!check_api_code(raw, error_msg)) return QQMUSIC_ERR_API;

    /* Log diagnostic flags from the response */
    {
        long long allow_song    = json::num(raw, "allow_song",    -1);
        long long allow_comment = json::num(raw, "allow_comment", -1);
        if (allow_song != -1 || allow_comment != -1) {
            loga("  allow_song="    + std::to_string(allow_song)
               + " allow_comment=" + std::to_string(allow_comment));
        }
    }

    /* Navigate response: data may have commentlist directly
     * OR wrapped under data.comment{} (older API format)
     * OR the comment object is at the root level (fcg_global_comment_h5) */
    std::string data_blk = json::object_raw(raw, "data");
    if (data_blk.empty()) {
        data_blk = raw;  /* fallback: root level */
    }

    /* Try direct commentlist first */
    std::string comments_raw   = json::array_raw(data_blk, "commentlist");
    std::string hot_raw        = json::array_raw(data_blk, "hotcommentlist");

    /* Fallback: nested "comment" wrapper (root-level "comment" object) */
    if (comments_raw.empty()) {
        std::string comment_blk = json::object_raw(data_blk, "comment");
        if (!comment_blk.empty()) {
            comments_raw = json::array_raw(comment_blk, "commentlist");
            hot_raw      = json::array_raw(comment_blk, "hotcommentlist");
            /* Log total comment count for diagnostics */
            long long total = json::num(comment_blk, "commenttotal", -1);
            if (total < 0) total = json::num(comment_blk, "totalcount", -1);
            if (total >= 0)
                loga("  commenttotal=" + std::to_string(total));
        }
    }

    int fetched = 0;

    /* On first page, emit hot/featured comments first */
    if (offset == 0 && !hot_raw.empty()) {
        loga("hotcommentlist:");
        if (emit_comment_list(hot_raw, page_size, fetched, visitor)) {
            out_delivered = fetched;
            return QQMUSIC_OK;
        }
    }

    /* Regular comments */
    if (!comments_raw.empty()) {
        emit_comment_list(comments_raw, page_size * 2, fetched, visitor);
    }

    out_delivered = fetched;
    loga("get_comments_page delivered=" + std::to_string(fetched));
    return QQMUSIC_OK;
}

/* ── get_comments (multi-page loop) ─────────────────── */

int ApiClient::get_comments(
    const std::wstring& song_mid,
    int                 limit,
    CommentVisitor      visitor,
    std::wstring&       error_msg)
{
    int fetched   = 0;
    int offset    = 0;
    const int page_size = std::min(limit, 100);

    while (fetched < limit) {
        int delivered = 0;
        int rc = get_comments_page(song_mid, offset, page_size, visitor,
                                   delivered, error_msg);
        if (rc != QQMUSIC_OK) return rc;

        fetched += delivered;
        if (delivered == 0) break; /* end of stream */

        offset += page_size;

        /* Anti-rate-limit: brief random sleep between pages */
        if (fetched < limit && delivered > 0) {
            BYTE rnd = 0;
            BCryptGenRandom(nullptr, &rnd, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            Sleep(300 + (DWORD)(rnd % 300));
        }
    }

    return QQMUSIC_OK;
}

} // namespace qqmusic
