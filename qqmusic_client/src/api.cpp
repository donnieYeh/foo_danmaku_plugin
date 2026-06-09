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
#include <cwctype>
#include <vector>
#include <sstream>

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
    /* QQ Music soso search API historically returned code:0 for success,
     * but has since migrated to HTTP-style code:200.  Accept both. */
    if (code != 0 && code != 200) {
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

    static const std::vector<std::wstring> allowed_extra = {
        L"original", L"mix", L"remix", L"edit", L"version", L"ver", L"single", L"album",
        L"ost", L"theme", L"song", L"op", L"ed", L"tv", L"size", L"karaoke", L"instrumental",
        L"inst", L"live", L"acoustic", L"cover", L"tribute", L"orchestra", L"piano", L"cv",
        L"bonus", L"track", L"cd", L"the", L"a", L"of", L"in", L"and", L"to", L"for", L"with",
        L"by", L"feat", L"ft"
    };

    int core_words = 0;
    int matched_core = 0;
    for (const auto& w : candidate_words) {
        bool is_metadata = (std::find(allowed_extra.begin(), allowed_extra.end(), w) != allowed_extra.end());
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

/* ── search_song_info ────────────────────────────────── */

SongInfo ApiClient::search_song_info(
    const std::wstring& keyword,
    std::wstring&       error_msg)
{
    SearchQuery q;
    q.title = keyword;
    return search_song_info(q, error_msg);
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
    std::string kw_utf8    = json::to_utf8(keyword);
    std::string kw_encoded = url_encode_utf8(kw_utf8);
    loga("search_song keyword: " + kw_utf8);

    /* ── Step 1: search via smartbox_new (soso/client_search_cp is dead) ───── */
    std::wstring path =
        L"/splcloud/fcgi-bin/smartbox_new.fcg"
        L"?is_xml=0"
        L"&key="    + std::wstring(kw_encoded.begin(), kw_encoded.end()) +
        L"&g_tk=5381&loginUin=0&hostUin=0"
        L"&format=json&inCharset=utf-8&outCharset=utf-8"
        L"&notice=0&platform=yqq.json&needNewCode=1";

    std::string raw;
    if (!m_http.get(path, raw, error_msg)) {
        log(L"search_song HTTP failed: " + error_msg);
        return info;
    }
    if (!check_api_code(raw, error_msg)) return info;

    /* Navigate: data → song → itemlist → [0]
     * smartbox response: {"code":0,"data":{"song":{"count":N,"itemlist":[...]},...}} */
    std::string data_blk = json::object_raw(raw, "data");
    if (data_blk.empty()) {
        error_msg = L"search: missing 'data' in smartbox response";
        log(error_msg);
        return info;
    }

    std::string song_blk = json::object_raw(data_blk, "song");
    if (song_blk.empty()) {
        error_msg = L"No songs found for: " + keyword;
        log(error_msg);
        return info;
    }

    long long song_count = json::num(song_blk, "count", -1);
    if (song_count == 0) {
        error_msg = L"No songs found for: " + keyword;
        loga("search_song: smartbox count=0");
        return info;
    }

    std::string list_raw = json::array_raw(song_blk, "itemlist");
    auto songs = json::array_items(list_raw);
    if (songs.empty()) {
        error_msg = L"No songs found for: " + keyword;
        loga("search_song: smartbox itemlist empty");
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
        std::wstring title = json::to_wide(json::str(item, "name"));
        std::wstring norm_title = normalize_str(title);

        std::wstring artist = json::to_wide(json::str(item, "singer"));
        std::wstring norm_artist = normalize_str(artist);
        std::wstring album = json::to_wide(json::str(item, "album"));
        std::wstring norm_album = normalize_str(album);
        int candidate_duration = (int)json::num(item, "interval", 0);
        if (candidate_duration > 0 && candidate_duration < 10000) candidate_duration *= 1000;

        int score = 0;
        bool title_matched = false;
        for (const auto& term : title_terms.empty() ? query_terms : title_terms) {
            if (has_match(norm_title, term)) {
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
            if (!norm_query_title.empty() && norm_title == norm_query_title) {
                score += 35;
            }
            for (const auto& term : title_terms.empty() ? query_terms : title_terms) {
                bool term_found = has_match(norm_title, term);
                if (!structured_terms && !term_found) {
                    term_found = has_match(norm_artist, term) ||
                                 has_match(norm_album, term);
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
            if (has_match(norm_artist, term)) {
                artist_matched = true;
                matched_count++;
                score += 18;
            }
        }
        if (!artist_terms.empty() && !artist_matched) score -= 18;
        if (!norm_query_artist.empty() && norm_artist == norm_query_artist) {
            score += 22;
        }

        for (const auto& term : album_terms) {
            if (has_match(norm_album, term)) {
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

    if (best_matches <= 0 || best_score <= 0) {
        error_msg = L"No matching song terms found for: " + keyword;
        loga("search_song: no positive structured score, returning empty");
        return info;
    }


    const std::string& best_song = songs[best_index];

    /* smartbox returns id as a JSON string ("id":"97773"), not a number.
     * Try json::num first (handles both); fall back to str+stoll. */
    long long id_num = json::num(best_song, "id", 0);
    if (id_num == 0) {
        std::string id_str = json::str(best_song, "id");
        if (!id_str.empty()) {
            try { id_num = std::stoll(id_str); } catch (...) {}
        }
    }
    if (id_num == 0) {
        error_msg = L"Failed to parse numeric song id from smartbox";
        log(error_msg);
        return info;
    }
    info.id = std::to_wstring(id_num);
    log(L"search_song: selected index=" + std::to_wstring(best_index)
        + L" id=" + info.id
        + L" score=" + std::to_wstring(best_score)
        + L" term_matches=" + std::to_wstring(best_matches));

    std::string mid_str = json::str(best_song, "mid");
    info.mid = json::to_wide(mid_str);
    if (!info.mid.empty())
        log(L"search_song: mid=" + info.mid);

    /* ── Step 2: fetch song detail to get album mid for cover URL (non-fatal) */
    if (!mid_str.empty()) {
        std::wstring detail_path =
            L"/v8/fcg-bin/fcg_play_single_song.fcg?songmid="
            + json::to_wide(mid_str)
            + L"&tmeAppID=qqmusic&format=json&inCharset=utf-8&outCharset=utf-8"
            L"&notice=0&platform=yqq.json&needNewCode=0";

        bool detail_ok = false;
        std::string detail_raw;
        std::wstring detail_err;
        detail_ok = m_http.get(detail_path, detail_raw, detail_err);
        if (detail_ok) {
            /* response: {"code":0,"data":[{"album":{"mid":"..."},...},...]} */
            std::string darr = json::array_raw(detail_raw, "data");
            if (!darr.empty()) {
                auto ditems = json::array_items(darr);
                if (!ditems.empty()) {
                    std::string album_blk = json::object_raw(ditems[0], "album");
                    if (!album_blk.empty()) {
                        std::string album_mid = json::str(album_blk, "mid");
                        if (!album_mid.empty()) {
                            info.cover_url =
                                L"https://y.gtimg.cn/music/photo_new/T002R300x300M000"
                                + json::to_wide(album_mid)
                                + L"_1.jpg";
                            log(L"search_song: cover_url=" + info.cover_url);
                        }
                    }
                }
            }
        } else {
            loga("search_song: detail fetch failed (non-fatal): "
                 + json::to_utf8(detail_err));
        }
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
