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

static std::string strip_nested(const std::string& js) {
    std::string out;
    out.reserve(js.size());
    int depth = 0;
    bool in_quote = false;
    for (size_t i = 0; i < js.size(); ++i) {
        char c = js[i];
        if (c == '"' && (i == 0 || js[i-1] != '\\')) {
            in_quote = !in_quote;
        }
        if (!in_quote) {
            if (c == '{' || c == '[') {
                depth++;
                continue;
            }
            if (c == '}' || c == ']') {
                depth--;
                continue;
            }
        }
        if (depth == 1) {
            out += c;
        }
    }
    return out;
}

static bool is_cjk(const std::wstring& s) {
    for (wchar_t c : s) {
        if (c >= 0x2E80) return true;
    }
    return false;
}

static bool has_match(const std::wstring& target, const std::wstring& term) {
    if (term.empty() || target.empty()) return false;
    if (is_cjk(term)) {
        return target.find(term) != std::wstring::npos;
    }
    size_t pos = 0;
    while ((pos = target.find(term, pos)) != std::wstring::npos) {
        bool before_ok = true;
        if (pos > 0) {
            wchar_t prev = target[pos - 1];
            if ((prev >= L'a' && prev <= L'z') || (prev >= L'A' && prev <= L'Z') || (prev >= L'0' && prev <= L'9')) {
                before_ok = false;
            }
        }
        bool after_ok = true;
        if (pos + term.size() < target.size()) {
            wchar_t next = target[pos + term.size()];
            if ((next >= L'a' && next <= L'z') || (next >= L'A' && next <= L'Z') || (next >= L'0' && next <= L'9')) {
                after_ok = false;
            }
        }
        if (before_ok && after_ok) {
            return true;
        }
        pos += 1;
    }
    return false;
}

static bool contains_unrequested_noise(const std::wstring& target, const std::vector<std::wstring>& query_terms) {
    static const std::vector<std::wstring> noise_words = {
        L"instrumental", L"instrument", L"inst", L"karaoke", L"伴奏", L"伴奏版", L"伴奏型",
        L"guide", L"melody", L"backing", L"offvocal", L"off vocal", L"piano", L"acoustic",
        L"remix", L"cover", L"翻唱", L"tribute", L"orchestra", L"orgel", L"八音盒"
    };
    for (const auto& noise : noise_words) {
        if (target.find(noise) != std::wstring::npos) {
            bool requested = false;
            for (const auto& term : query_terms) {
                if (term.find(noise) != std::wstring::npos || noise.find(term) != std::wstring::npos) {
                    requested = true;
                    break;
                }
            }
            if (!requested) {
                return true;
            }
        }
    }
    return false;
}

/* Fix-2: file-scope list shared by check_word_coverage and search_song_info.
 * Words in this list are treated as "metadata" — they do not count toward
 * the match threshold and are not considered "core" query terms. */
static const std::vector<std::wstring> k_allowed_extra = {
    L"original", L"mix", L"remix", L"edit", L"version", L"ver", L"single", L"album",
    L"ost", L"theme", L"song", L"op", L"ed", L"tv", L"size", L"karaoke", L"instrumental",
    L"inst", L"live", L"acoustic", L"cover", L"tribute", L"orchestra", L"piano", L"cv",
    L"bonus", L"track", L"cd", L"the", L"a", L"of", L"in", L"and", L"to", L"for", L"with",
    L"by", L"feat", L"ft"
};

static bool check_word_coverage(const std::wstring& norm_title, const std::vector<std::wstring>& query_terms) {
    bool has_cjk_term = false;
    for (const auto& term : query_terms) {
        if (is_cjk(term)) {
            has_cjk_term = true;
            break;
        }
    }
    if (has_cjk_term) return true;

    std::vector<std::wstring> candidate_words;
    std::wstringstream ss(norm_title);
    std::wstring word;
    while (ss >> word) {
        candidate_words.push_back(word);
    }
    if (candidate_words.empty()) return false;

    int core_words = 0;
    int matched_core = 0;
    for (const auto& w : candidate_words) {
        bool is_metadata = (std::find(k_allowed_extra.begin(), k_allowed_extra.end(), w) != k_allowed_extra.end());
        if (!is_metadata) {
            core_words++;
            for (const auto& term : query_terms) {
                if (w == term) {
                    matched_core++;
                    break;
                }
            }
        }
    }
    if (core_words == 0) return true;
    double coverage = (double)matched_core / core_words;
    return coverage >= 0.7;
}

static std::wstring normalize_str(const std::wstring& src) {
    if (src.empty()) return L"";
    int size = LCMapStringW(LOCALE_USER_DEFAULT, LCMAP_SIMPLIFIED_CHINESE | LCMAP_LOWERCASE, src.c_str(), (int)src.size(), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring dest(size, L'\0');
    LCMapStringW(LOCALE_USER_DEFAULT, LCMAP_SIMPLIFIED_CHINESE | LCMAP_LOWERCASE, src.c_str(), (int)src.size(), &dest[0], size);

    std::wstring filtered;
    filtered.reserve(dest.size());
    for (wchar_t c : dest) {
        if (iswspace(c) || iswpunct(c) || c == L'（' || c == L'）' || c == L'【' || c == L'】' || c == L'「' || c == L'」' || c == L'～' || c == L'~' || c == L'・') {
            if (filtered.empty() || filtered.back() != L' ') {
                filtered += L' ';
            }
        } else {
            filtered += c;
        }
    }
    if (!filtered.empty() && filtered.back() == L' ') {
        filtered.pop_back();
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
            std::wstringstream ss2(norm);
            std::wstring subitem;
            while (ss2 >> subitem) {
                if (std::find(terms.begin(), terms.end(), subitem) == terms.end()) {
                    terms.push_back(subitem);
                }
            }
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
    SearchQuery q;
    q.title = keyword;
    return search_song_info(q, error_msg);
}

static bool any_artist_matches(const std::vector<std::wstring>& artists, const std::wstring& term) {
    for (const auto& art : artists) {
        if (has_match(art, term)) return true;
    }
    return false;
}

static int duration_score(int query_ms, int candidate_ms) {
    if (query_ms <= 0 || candidate_ms <= 0) return 0;
    int diff = std::abs(query_ms - candidate_ms);
    if (diff <= 2000) return 18;
    if (diff <= 5000) return 12;
    if (diff <= 10000) return 5;
    if (diff >= 30000) return -18;
    return -6;
}

SongInfo ApiClient::search_song_info(
    const SearchQuery& query,
    std::wstring&      error_msg)
{
    SongInfo info;
    std::wstring keyword = query.title;
    if (!query.artist.empty()) keyword += L" " + query.artist;
    if (!query.album.empty()) keyword += L" " + query.album;
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

    int best_index = 0;
    int best_score = -100000;
    int best_matches = 0;
    auto query_terms = split_keyword_to_terms(keyword);
    auto title_terms = split_keyword_to_terms(query.title);
    auto artist_terms = split_keyword_to_terms(query.artist);
    auto album_terms = split_keyword_to_terms(query.album);
    std::wstring norm_query_title = normalize_str(query.title);
    std::wstring norm_query_artist = normalize_str(query.artist);
    const bool structured_terms = !artist_terms.empty() || !album_terms.empty();

    for (int i = 0; i < (int)songs.size(); i++) {
        const auto& item = songs[i];
        std::string stripped = strip_nested(item);
        std::wstring title = json::to_wide(json::str(stripped, "name"));
        std::wstring norm_title = normalize_str(title);

        std::string alias_raw = json::array_raw(item, "alias");
        std::wstring norm_alias = normalize_str(json::to_wide(alias_raw));

        std::string trans_raw = json::array_raw(item, "transNames");
        std::wstring norm_trans = normalize_str(json::to_wide(trans_raw));

        std::string artists_raw = json::array_raw(item, "artists");
        auto artist_items = json::array_items(artists_raw);
        std::vector<std::wstring> artists;
        for (const auto& art : artist_items) {
            artists.push_back(normalize_str(json::to_wide(json::str(art, "name"))));
        }

        std::string album_blk = json::object_raw(item, "album");
        std::wstring album_name = normalize_str(json::to_wide(json::str(album_blk, "name")));
        int candidate_duration = (int)json::num(stripped, "duration", 0);
        if (candidate_duration <= 0) candidate_duration = (int)json::num(stripped, "dt", 0);

        int score = 0;
        bool title_matched = false;
        for (const auto& term : title_terms.empty() ? query_terms : title_terms) {
            if (has_match(norm_title, term) ||
                has_match(norm_alias, term) ||
                has_match(norm_trans, term)) {
                title_matched = true;
                break;
            }
        }

        if (title_matched && contains_unrequested_noise(norm_title, query_terms)) {
            title_matched = false;
        }

        if (title_matched && !check_word_coverage(norm_title, query_terms)) {
            title_matched = false;
        }

        int matched_count = 0;
        if (title_matched) {
            score += 45;
            if (!norm_query_title.empty() &&
                (norm_title == norm_query_title || norm_alias.find(norm_query_title) != std::wstring::npos ||
                 norm_trans.find(norm_query_title) != std::wstring::npos)) {
                score += 35;
            }
            for (const auto& term : title_terms.empty() ? query_terms : title_terms) {
                bool term_found = has_match(norm_title, term) ||
                                  has_match(norm_alias, term) ||
                                  has_match(norm_trans, term);
                if (!structured_terms && !term_found) {
                    term_found = has_match(album_name, term) ||
                                 any_artist_matches(artists, term);
                }
                if (term_found) {
                    matched_count++;
                    score += 10;
                }
            }
        } else {
            score -= 60;
        }

        bool artist_matched = false;
        for (const auto& term : artist_terms) {
            if (any_artist_matches(artists, term)) {
                artist_matched = true;
                matched_count++;
                score += 18;
            }
        }
        if (!artist_terms.empty() && !artist_matched) score -= 18;
        if (!norm_query_artist.empty()) {
            for (const auto& art : artists) {
                if (art == norm_query_artist) {
                    score += 22;
                    break;
                }
            }
        }

        for (const auto& term : album_terms) {
            if (has_match(album_name, term)) {
                matched_count++;
                score += 4;
            }
        }

        score += duration_score(query.duration_ms, candidate_duration);

        if (score > best_score) {
            best_score = score;
            best_matches = matched_count;
            best_index = i;
        }
    }

    /* Fix-2: require at least ceil(n_core / 2) core-term matches, where core
     * terms are query tokens that are NOT in the allowed_extra metadata list.
     * This prevents a single coincidental word match (e.g. "complete" in an
     * unrelated song) from being accepted when the query has multiple meaningful
     * terms (e.g. "ichigo", "complete", "mashimaro"). */
    int n_core_terms = 0;
    for (const auto& t : query_terms) {
        if (std::find(k_allowed_extra.begin(), k_allowed_extra.end(), t) == k_allowed_extra.end())
            n_core_terms++;
    }
    int match_threshold = (n_core_terms > 0) ? (n_core_terms + 1) / 2 : 0;

    if (best_matches < match_threshold || best_score <= 0) {
        error_msg = L"No matching song terms found for: " + keyword;
        loga("search_song: matches=" + std::to_string(best_matches)
           + " < threshold=" + std::to_string(match_threshold) + ", returning empty");
        return info;
    }


    const auto& best_song = songs[best_index];
    long long id = json::num(strip_nested(best_song), "id", 0);
    if (id == 0) {
        error_msg = L"Failed to parse song ID";
        log(L"best_song=" + json::to_wide(best_song.substr(0, 80)));
        return info;
    }
    log(L"search_song: selected index=" + std::to_wstring(best_index)
        + L" id=" + std::to_wstring(id)
        + L" score=" + std::to_wstring(best_score)
        + L" term_matches=" + std::to_wstring(best_matches));
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
