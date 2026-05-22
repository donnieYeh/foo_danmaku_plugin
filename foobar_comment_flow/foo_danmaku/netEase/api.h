#ifndef NETEASE_API_H
#define NETEASE_API_H

#include <windows.h>
#include <string>
#include <vector>

struct Comment {
    std::wstring content;
    std::wstring nickname;
    int likeCount;
};

struct SongSearchResult {
    std::wstring songId;
    std::wstring songName;
    std::wstring artistName;
};

class NetEaseAPI {
public:
    NetEaseAPI();
    ~NetEaseAPI();

    bool searchSong(const std::wstring& keyword, std::vector<SongSearchResult>& results);
    bool getComments(const std::wstring& songId, std::vector<Comment>& comments, int limit = 50);
    bool getCommentsByKeyword(const std::wstring& songName, const std::wstring& artistName,
                              std::vector<Comment>& comments, int limit = 50);

private:
    bool initNetwork();
    std::string httpGet(const std::string& url);
    std::string httpPost(const std::string& url, const std::string& postData);
    std::wstring utf8ToUnicode(const std::string& utf8);
    std::string unicodeToUtf8(const std::wstring& unicode);

    bool m_initialized;
};

#endif // NETEASE_API_H