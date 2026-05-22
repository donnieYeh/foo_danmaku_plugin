#pragma once
#include "http.h"
#include <string>
#include <functional>

namespace netease {

struct Comment {
    std::wstring content;
    std::wstring nickname;
    int          like_count = 0;
};

/** Return false to stop iteration early. */
using CommentVisitor = std::function<bool(const Comment&)>;

class ApiClient {
public:
    explicit ApiClient(HttpClient& http);

    /**
     * Search and return the best-matching song ID.
     * Returns empty string on failure and sets error_msg.
     */
    std::wstring search_song(const std::wstring& keyword,
                             std::wstring&       error_msg);

    /**
     * Iterate comments for song_id.
     * Calls visitor for each comment; stops when visitor returns false,
     * limit is reached, or there are no more comments.
     * Returns NETEASE_* error code.
     */
    int get_comments(const std::wstring& song_id,
                     int                 limit,
                     CommentVisitor      visitor,
                     std::wstring&       error_msg);

private:
    HttpClient& m_http;

    /* Build encrypted POST body and send to /weapi/<path>.
     * Returns raw JSON response string, or "" on failure. */
    std::string weapi(const std::wstring& path,
                      const std::string&  payload_json,
                      std::wstring&       error_msg);
};

} // namespace netease
