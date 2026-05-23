#pragma once
#include "http.h"
#include <string>
#include <functional>

namespace qqmusic {

struct Comment {
    std::wstring content;
    std::wstring nickname;
    int          like_count = 0;
};

struct SongInfo {
    std::wstring id;         /* numeric songid string, e.g. "97869"   → used as topid in comments */
    std::wstring mid;        /* alphanumeric songmid, e.g. "001OLkXf2nqxZ9" (kept for reference) */
    std::wstring cover_url;  /* HTTPS URL of album art (300x300 JPEG)                             */
};

/** Return false to stop iteration early. */
using CommentVisitor = std::function<bool(const Comment&)>;

class ApiClient {
public:
    explicit ApiClient(HttpClient& http);

    /**
     * Update credentials derived from cookie (g_tk and loginUin).
     * Called whenever the handle's cookie changes.
     */
    void set_cookie(const std::wstring& cookie);

    /**
     * Search and return the best-matching songmid.
     * Returns empty SongInfo on failure; sets error_msg.
     */
    SongInfo search_song_info(const std::wstring& keyword,
                              std::wstring&       error_msg);

    /** Convenience: return only the mid. */
    std::wstring search_song(const std::wstring& keyword,
                             std::wstring&       error_msg);

    /**
     * Iterate comments for song_mid.
     * Calls visitor for each comment; stops when visitor returns false,
     * limit is reached, or there are no more comments.
     * Returns QQMUSIC_* error code.
     */
    int get_comments(const std::wstring& song_mid,
                     int                 limit,
                     CommentVisitor      visitor,
                     std::wstring&       error_msg);

    /**
     * Fetch exactly one page of comments (one HTTP request).
     *
     * @param offset      0-based absolute offset; pagenum = offset / limit.
     * @param limit       Page size (clamped to 1..100).
     * @param out_delivered  Actual number of comments handed to visitor.
     *
     * Hot comments are emitted before the regular list on the first page
     * (offset == 0).  Returns QQMUSIC_OK on success (even if 0 delivered).
     */
    int get_comments_page(const std::wstring& song_mid,
                          int                 offset,
                          int                 limit,
                          CommentVisitor      visitor,
                          int&                out_delivered,
                          std::wstring&       error_msg);

private:
    HttpClient& m_http;
    int         m_g_tk      = 5381;    /* 5381 = anonymous (empty skey) */
    std::wstring m_login_uin = L"0";   /* numeric uin extracted from cookie */

    /** Parse skey/p_skey from cookie and compute g_tk + loginUin. */
    void parse_cookie_fields(const std::wstring& cookie);

    /** Emit items from a JSON comment array to visitor; returns true if caller stopped. */
    bool emit_comment_list(const std::string& arr_raw,
                           int                max_emit,
                           int&               fetched,
                           CommentVisitor&    visitor);
};

} // namespace qqmusic
