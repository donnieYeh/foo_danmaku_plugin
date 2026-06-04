/* api.cpp — NetEase Cloud Music search + comments
 *
 * Search currently uses the legacy unencrypted /api/search/get/web endpoint.
 * The encrypted /weapi/cloudsearch/get/web endpoint can return code 50000005
 * for anonymous requests, so keep it out of the primary resolve path.
 * Comments still use weapi encrypted endpoints (encrypt_weapi from crypto.cpp).
 *
 *   Search  : POST https://music.163.com/api/search/get/web
 *             payload: {"s":"<kw>","type":1,"limit":20,"offset":0,
 *                       "total":true,"csrf_token":""}
 *
 *   Comments: POST https://music.163.com/weapi/v1/resource/comments/R_SO_4_{id}
 *             payload: {"rid":"R_SO_4_<id>","limit":N,"offset":M,
 *                       "total":true,"csrf_token":""}
 */

#include "api.h"
#include "crypto.h"
#include "json.h"
#include "logger.h"
#include "../include/netease_client.h"
#include <windows.h>
#include <string>
#include <algorithm>
#include <cwctype>
#include <vector>
#include <sstream>

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
    /* The top-level "code" field is at the end of many NetEase responses, but
     * search results contain lots of nested "code":0 fields in privileges.
     * json::num() returns the FIRST match, so using it on a tail substring can
     * falsely treat a successful cloudsearch response as "API error 0".
     * Read the LAST "code" occurrence instead; for these endpoints that is the
     * top-level status code we care about. */
    std::string codePat = "\"code\":";
    size_t codePos = resp.rfind(codePat);
    long long code = 200;
    std::string tail;
    if (codePos != std::string::npos) {
        tail = resp.substr(codePos);
        code = json::num(tail, "code", 200);
    } else {
        tail = resp.size() > 512 ? resp.substr(resp.size() - 512) : resp;
    }
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

static long long num_last(const std::string& js, const std::string& key, long long def) {
    std::string pat = "\"" + key + "\":";
    size_t pos = js.rfind(pat);
    if (pos == std::string::npos) return def;
    const char* p = js.c_str() + pos + pat.size();
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '"') ++p; /* handle quoted numbers */
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return def;
    char* endp = nullptr;
    long long v = strtoll(p, &endp, 10);
    return (endp > p) ? v : def;
}

int ApiClient::weapi(
    const std::wstring& path,
    const std::string&  payload_json,
    std::string&        out_resp,
    std::wstring&       error_msg)
{
    out_resp.clear();
    std::string params, enc_sec_key;
    if (!encrypt_weapi(payload_json, params, enc_sec_key, error_msg)) {
        log(L"weapi: encrypt failed: " + error_msg);
        return NETEASE_ERR_CRYPTO;
    }

    std::string body = "params=" + params + "&encSecKey=" + enc_sec_key;
    std::string raw;
    if (!m_http.post(path, body, raw, error_msg)) {
        log(L"weapi HTTP failed: " + error_msg);
        return NETEASE_ERR_NETWORK;
    }
    out_resp = api_call(raw, error_msg);
    return out_resp.empty() ? NETEASE_ERR_API : NETEASE_OK;
}

static std::wstring normalize_str(const std::wstring& src) {
    if (src.empty()) return L"";
    // Convert to simplified Chinese and lowercase using LCMapStringW
    int size = LCMapStringW(LOCALE_USER_DEFAULT, LCMAP_SIMPLIFIED_CHINESE | LCMAP_LOWERCASE, src.c_str(), (int)src.size(), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring dest(size, L'\0');
    LCMapStringW(LOCALE_USER_DEFAULT, LCMAP_SIMPLIFIED_CHINESE | LCMAP_LOWERCASE, src.c_str(), (int)src.size(), &dest[0], size);

    std::wstring filtered;
    filtered.reserve(dest.size());
    for (wchar_t c : dest) {
        if (iswspace(c) || iswpunct(c)) continue;
        if (c == L'（' || c == L'）' || c == L'【' || c == L'】' || c == L'「' || c == L'」' || c == L'～' || c == L'~') continue;
        filtered += c;
    }
    return filtered;
}

static std::vector<std::wstring> split_keyword_to_terms(const std::wstring& keyword) {
    std::vector<std::wstring> terms;
    std::wstringstream ss(keyword);
    std::wstring item;
    while (std::getline(ss, item, L' ')) {
        std::wstring norm = normalize_str(item);
        if (!norm.empty()) {
            terms.push_back(norm);
        }
    }
    return terms;
}

/* ── search_song ─────────────────────────────────────── */

std::wstring ApiClient::search_song(
    const std::wstring& keyword,
    std::wstring&       error_msg)
{
    SongInfo info = search_song_info(keyword, error_msg);
    return info.id;
}

SongInfo ApiClient::search_song_info(
    const std::wstring& keyword,
    std::wstring&       error_msg)
{
    SongInfo info;
    std::string kw_utf8 = json::to_utf8(keyword);
    loga("search_song keyword: " + kw_utf8);

    std::string resp;
    std::string body =
        "s="      + url_encode_utf8(kw_utf8) +
        "&type=1"
        "&limit=20"
        "&offset=0"
        "&total=true";

    if (!m_http.post(L"/api/search/get/web", body, resp, error_msg)) {
        log(L"search_song HTTP failed: " + error_msg);
        return info;
    }
    resp = api_call(resp, error_msg);
    if (resp.empty()) return info;

    /* Parse result.songs[0].id */
    const char* result_start = strstr(resp.c_str(), "\"result\":{");
    if (!result_start) result_start = resp.c_str();

    std::string songs_raw = json::array_raw(std::string(result_start), "songs");
    if (songs_raw.empty()) songs_raw = json::array_raw(resp, "songs");

    auto songs = json::array_items(songs_raw);
    if (songs.empty()) {
        error_msg = L"No songs found for: " + keyword;
        loga("search_song: no songs in response");
        return info;
    }

    // Rank and select the best matching song
    int best_index = 0;
    int max_matches = -1;
    auto query_terms = split_keyword_to_terms(keyword);

    for (int i = 0; i < (int)songs.size(); i++) {
        const auto& item = songs[i];
        std::wstring title = json::to_wide(json::str(item, "name"));
        std::wstring norm_title = normalize_str(title);

        std::string artists_raw = json::array_raw(item, "artists");
        auto artist_items = json::array_items(artists_raw);
        std::vector<std::wstring> artists;
        for (const auto& art : artist_items) {
            artists.push_back(normalize_str(json::to_wide(json::str(art, "name"))));
        }

        std::string album_blk = json::object_raw(item, "album");
        std::wstring album_name = normalize_str(json::to_wide(json::str(album_blk, "name")));

        int matched_count = 0;
        for (const auto& term : query_terms) {
            bool term_found = false;
            if (norm_title.find(term) != std::wstring::npos) {
                term_found = true;
            } else if (album_name.find(term) != std::wstring::npos) {
                term_found = true;
            } else {
                for (const auto& art : artists) {
                    if (art.find(term) != std::wstring::npos) {
                        term_found = true;
                        break;
                    }
                }
            }
            if (term_found) {
                matched_count++;
            }
        }

        if (matched_count > max_matches) {
            max_matches = matched_count;
            best_index = i;
        }
    }

    const auto& best_song = songs[best_index];
    long long id = num_last(best_song, "id", 0);
    if (id == 0) {
        error_msg = L"Failed to parse song ID";
        log(L"best_song=" + json::to_wide(best_song.substr(0, 80)));
        return info;
    }
    log(L"search_song: selected index=" + std::to_wstring(best_index) + L" id=" + std::to_wstring(id) + L" term_matches=" + std::to_wstring(max_matches));
    info.id = std::to_wstring(id);

    /* /api/search/get/web: cover is usually at songs[best_index].album.picUrl.
     * Keep songs[best_index].al.picUrl and songs[best_index].picUrl as fallbacks for forward compat. */
    std::string album_blk = json::object_raw(best_song, "album");
    if (!album_blk.empty())
        info.cover_url = json::to_wide(json::str(album_blk, "picUrl"));
    if (info.cover_url.empty()) {
        std::string al_blk = json::object_raw(best_song, "al");
        if (!al_blk.empty())
            info.cover_url = json::to_wide(json::str(al_blk, "picUrl"));
    }
    if (info.cover_url.empty())
        info.cover_url = json::to_wide(json::str(best_song, "picUrl"));

    /* Fallback: if search response doesn't have a cover URL, fetch song details */
    if (info.cover_url.empty() && !info.id.empty()) {
        log(L"search_song: cover_url empty in search response, trying song/detail fallback...");
        std::string detail_resp;
        std::wstring detail_err;
        std::string detail_payload = "{\"c\":\"[{\\\"id\\\":" + json::to_utf8(info.id) + "}]\",\"ids\":\"[" + json::to_utf8(info.id) + "]\"}";
        int detail_rc = weapi(L"/weapi/v3/song/detail", detail_payload, detail_resp, detail_err);
        if (detail_rc == NETEASE_OK && !detail_resp.empty()) {
            std::string detail_songs_raw = json::array_raw(detail_resp, "songs");
            auto detail_songs = json::array_items(detail_songs_raw);
            if (!detail_songs.empty()) {
                std::string al_blk = json::object_raw(detail_songs[0], "al");
                if (!al_blk.empty()) {
                    info.cover_url = json::to_wide(json::str(al_blk, "picUrl"));
                }
                if (info.cover_url.empty()) {
                    std::string album_blk = json::object_raw(detail_songs[0], "album");
                    if (!album_blk.empty()) {
                        info.cover_url = json::to_wide(json::str(album_blk, "picUrl"));
                    }
                }
                if (info.cover_url.empty()) {
                    info.cover_url = json::to_wide(json::str(detail_songs[0], "picUrl"));
                }
            }
        } else {
            log(L"search_song: song/detail fallback failed, rc=" + std::to_wstring(detail_rc) + L" err=" + detail_err);
        }
    }

    if (!info.cover_url.empty()) {
        log(L"search_song: cover_url=" + info.cover_url);
    }
    return info;
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

int ApiClient::fetch_comments_page(
    const std::wstring& song_id,
    int                 offset,
    int                 page_size,
    std::string&        out_resp,
    std::wstring&       error_msg)
{
    std::string id_utf8 = json::to_utf8(song_id);
    std::string thread_id = "R_SO_4_" + id_utf8;
    std::string jp =
        "{\"rid\":\""    + json::escape(thread_id)       + "\","
        "\"limit\":"    + std::to_string(page_size)      + ","
        "\"offset\":"   + std::to_string(offset)         + ","
        "\"total\":true,"
        "\"csrf_token\":\"\"}";
    std::wstring path = L"/weapi/v1/resource/comments/R_SO_4_" + song_id;

    loga("fetch_comments_page offset=" + std::to_string(offset)
       + " limit=" + std::to_string(page_size));

    return weapi(path, jp, out_resp, error_msg);
}

/* ── get_comments ────────────────────────────────────── */

int ApiClient::get_comments(
    const std::wstring& song_id,
    int                 limit,
    CommentVisitor      visitor,
    std::wstring&       error_msg)
{
    int fetched = 0;
    int offset  = 0;
    const int page_size = std::min(limit, 100);

    /* Page 0: also returns hotComments (top comments) */
    bool first_page = true;

    while (fetched < limit) {
        std::string resp;
        int rc = fetch_comments_page(song_id, offset, page_size, resp, error_msg);
        if (rc != NETEASE_OK) return rc;

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

/* ── get_comments_page (single HTTP request) ──────────── */

int ApiClient::get_comments_page(
    const std::wstring& song_id,
    int                 offset,
    int                 limit,
    CommentVisitor      visitor,
    int&                out_delivered,
    std::wstring&       error_msg)
{
    out_delivered = 0;
    int page_size = std::min(std::max(limit, 1), 100);
    std::string resp;
    int rc = fetch_comments_page(song_id, offset, page_size, resp, error_msg);
    if (rc != NETEASE_OK) return rc;

    int fetched = 0;
    // hotComments only attached to first page in NetEase API
    if (offset == 0) {
        std::string hot_raw = json::array_raw(resp, "hotComments");
        if (!hot_raw.empty()) {
            loga("hotComments:");
            parse_comment_list(hot_raw, page_size, fetched, visitor);
            // fetched now counts hotComments emitted; visitor may have stopped early
        }
    }

    std::string comments_raw = json::array_raw(resp, "comments");
    if (!comments_raw.empty()) {
        parse_comment_list(comments_raw, page_size * 2, fetched, visitor);
    }

    out_delivered = fetched;
    return NETEASE_OK;
}

} // namespace netease
