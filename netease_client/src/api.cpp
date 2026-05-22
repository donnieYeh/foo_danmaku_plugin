/* api.cpp — NetEase Cloud Music search + comments
 *
 * Uses the plain /api/ endpoints (no weapi encryption required).
 *   Search  : POST https://music.163.com/api/cloudsearch/pc
 *   Comments: GET  https://music.163.com/api/v1/resource/comments/R_SO_4_{id}
 */

#include "api.h"
#include "json.h"
#include "logger.h"
#include "../include/netease_client.h"
#include <windows.h>
#include <string>
#include <algorithm>

namespace netease {

ApiClient::ApiClient(HttpClient& http) : m_http(http) {}

/* ── percent-encode a UTF-8 string for form POST body ── */

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

/* ── api_call: check code, log, return body ─────────── */

static std::string api_call(
    const std::string& resp,
    std::wstring&      error_msg)
{
    if (resp.empty()) return "";
    /* The top-level "code" field is always near the END of NetEase responses
     * (after the large "result"/"data"/"hotComments" blocks).
     * Search the last 512 bytes to avoid matching nested privilege.code. */
    size_t tail_start = resp.size() > 512 ? resp.size() - 512 : 0;
    std::string tail  = resp.substr(tail_start);
    long long code    = json::num(tail, "code", 200);
    if (code != 200) {
        std::string msg = json::str(tail, "message");
        if (msg.empty()) msg = json::str(tail, "msg");
        error_msg = L"API error " + std::to_wstring(code);
        if (!msg.empty()) error_msg += L": " + json::to_wide(msg);
        log(L"api_call: " + error_msg);
        return "";
    }
    return resp;
}

/* ── search_song ─────────────────────────────────────── */

std::wstring ApiClient::search_song(
    const std::wstring& keyword,
    std::wstring&       error_msg)
{
    std::string kw_utf8 = json::to_utf8(keyword);
    loga("search_song keyword: " + kw_utf8);

    /* POST /api/cloudsearch/pc  — no encryption */
    std::string body = "s=" + url_encode_utf8(kw_utf8)
                     + "&type=1&limit=20&offset=0&total=true";

    std::string raw;
    if (!m_http.post(L"/api/cloudsearch/pc", body, raw, error_msg))
        return L"";

    std::string resp = api_call(raw, error_msg);
    if (resp.empty()) return L"";

    /* Parse result.songs[0].id */
    const char* result_start = strstr(resp.c_str(), "\"result\":{");
    if (!result_start) result_start = resp.c_str();

    std::string songs_raw = json::array_raw(std::string(result_start), "songs");
    if (songs_raw.empty()) songs_raw = json::array_raw(resp, "songs");

    auto songs = json::array_items(songs_raw);
    if (songs.empty()) {
        error_msg = L"No songs found for: " + keyword;
        loga("search_song: no songs in response");
        return L"";
    }

    long long id = json::num(songs[0], "id", 0);
    if (id == 0) {
        error_msg = L"Failed to parse song ID";
        log(L"songs[0]=" + json::to_wide(songs[0].substr(0, 80)));
        return L"";
    }
    log(L"search_song: found id=" + std::to_wstring(id));
    return std::to_wstring(id);
}

/* ── parse_comment_list: shared for hotComments+comments */

static int parse_comment_list(
    const std::string& arr_raw,
    int                limit,
    int&               fetched,
    CommentVisitor     visitor)
{
    auto items = json::array_items(arr_raw);
    loga("  parse_comment_list: " + std::to_string(items.size()) + " items");
    for (const auto& item : items) {
        Comment c;
        c.content    = json::to_wide(json::str(item, "content"));
        c.like_count = (int)json::num(item, "likedCount", 0);
        const char* up = strstr(item.c_str(), "\"user\":{");
        if (up) c.nickname = json::to_wide(json::str(std::string(up), "nickname"));

        if (!visitor(c)) return 1; /* caller stopped */
        ++fetched;
        if (fetched >= limit) return 1;
    }
    return 0;
}

/* ── get_comments ────────────────────────────────────── */

int ApiClient::get_comments(
    const std::wstring& song_id,
    int                 limit,
    CommentVisitor      visitor,
    std::wstring&       error_msg)
{
    std::string id_utf8 = json::to_utf8(song_id);
    int fetched = 0;
    int offset  = 0;
    const int page_size = std::min(limit, 100);

    /* Page 0: also returns hotComments (top comments) */
    bool first_page = true;

    while (fetched < limit) {
        std::wstring path = L"/api/v1/resource/comments/R_SO_4_"
                          + song_id
                          + L"?limit=" + std::to_wstring(page_size)
                          + L"&offset=" + std::to_wstring(offset)
                          + L"&total=true";

        loga("get_comments offset=" + std::to_string(offset));
        std::string raw;
        if (!m_http.get(path, raw, error_msg)) {
            log(L"get_comments HTTP failed: " + error_msg);
            return NETEASE_ERR_NETWORK;
        }

        std::string resp = api_call(raw, error_msg);
        if (resp.empty()) return NETEASE_ERR_API;

        /* On first page, include hotComments first */
        if (first_page) {
            first_page = false;
            std::string hot_raw = json::array_raw(resp, "hotComments");
            if (!hot_raw.empty()) {
                loga("hotComments:");
                if (parse_comment_list(hot_raw, limit, fetched, visitor))
                    return NETEASE_OK;
            }
        }

        /* Regular comments */
        std::string comments_raw = json::array_raw(resp, "comments");
        if (comments_raw.empty()) break;
        if (parse_comment_list(comments_raw, limit, fetched, visitor))
            return NETEASE_OK;

        /* Check hasMore / more */
        bool more = (json::num(resp, "more", 1) != 0);
        if (!more) break;

        offset += page_size;

        /* Anti-rate-limit */
        if (fetched < limit) {
            BYTE rnd = 0;
            BCryptGenRandom(nullptr, &rnd, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            Sleep(300 + (DWORD)(rnd % 300));
        }
    }

    return NETEASE_OK;
}

} // namespace netease
