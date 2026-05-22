#include "api.h"
#include "signature.h"
#include <windows.h>
#include <wininet.h>
#include <string>
#include <cstdlib>

#pragma comment(lib, "wininet.lib")

NetEaseAPI::NetEaseAPI() : m_initialized(false) {
    m_initialized = initNetwork();
}

NetEaseAPI::~NetEaseAPI() {
}

bool NetEaseAPI::initNetwork() {
    HINTERNET hInternet = InternetOpenW(L"FooBarDanmaku/1.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (hInternet) {
        InternetCloseHandle(hInternet);
        return true;
    }
    return false;
}

bool NetEaseAPI::searchSong(const std::wstring& keyword, std::vector<SongSearchResult>& results) {
    // Try Python SDK first for proper encoding handling
    std::string pyCmd = "python fetch_comments.py ";
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, keyword.c_str(), (int)keyword.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8Keyword(utf8Len, 0);
    WideCharToMultiByte(CP_UTF8, 0, keyword.c_str(), (int)keyword.size(), &utf8Keyword[0], utf8Len, nullptr, nullptr);
    pyCmd += utf8Keyword;

    FILE* pipe = _popen(pyCmd.c_str(), "rb");
    if (pipe) {
        std::string resultJson;
        char buffer[4096];
        while (fread(buffer, 1, sizeof(buffer)-1, pipe) > 0) {
            buffer[sizeof(buffer)-1] = '\0';
            resultJson += buffer;
        }
        int rc = _pclose(pipe);

        if (rc == 0 && !resultJson.empty()) {
            const char* json = resultJson.c_str();
            const char* songIdP = strstr(json, "\"songId\":");
            const char* songNameP = strstr(json, "\"songName\":");
            const char* artistNameP = strstr(json, "\"artistName\":");

            if (songIdP && songNameP) {
                SongSearchResult r;

                const char* idStart = strchr(songIdP + 9, '"');
                if (idStart && ++idStart < json + resultJson.length()) {
                    const char* idEnd = strchr(idStart, '"');
                    if (idEnd) {
                        r.songId = utf8ToUnicode(std::string(idStart, idEnd - idStart));
                    }
                }

                const char* nameStart = strchr(songNameP + 11, '"');
                if (nameStart && ++nameStart < json + resultJson.length()) {
                    const char* nameEnd = strchr(nameStart, '"');
                    if (nameEnd) {
                        r.songName = utf8ToUnicode(std::string(nameStart, nameEnd - nameStart));
                    }
                }

                if (artistNameP) {
                    const char* artStart = strchr(artistNameP + 13, '"');
                    if (artStart && ++artStart < json + resultJson.length()) {
                        const char* artEnd = strchr(artStart, '"');
                        if (artEnd) {
                            r.artistName = utf8ToUnicode(std::string(artStart, artEnd - artStart));
                        }
                    }
                }

                if (!r.songId.empty()) {
                    results.push_back(r);
                    return true;
                }
            }
        }
    }

    // Fallback to original HTTP approach
    std::string url = "https://music.163.com/api/search/get?s=";
    url += unicodeToUtf8(keyword);
    url += "&type=1&limit=10&offset=0";

    std::string response = httpGet(url);
    if (response.empty()) return false;

    const char* p = response.c_str();
    const char* end = p + response.length();

    while (results.size() < 10) {
        const char* idP = strstr(p, "\"id\":");
        if (!idP || idP >= end) break;
        idP += 5;

        while (*idP && !isdigit(*idP) && idP < end) idP++;
        if (idP >= end || !*idP) break;

        char idStr[32] = {0};
        int j = 0;
        while (*idP && isdigit(*idP) && j < 31) {
            idStr[j++] = *idP++;
        }

        SongSearchResult result;
        result.songId = utf8ToUnicode(idStr);

        const char* nameP = strstr(idP, "\"name\":\"");
        if (nameP) {
            nameP += 8;
            char name[256] = {0};
            j = 0;
            while (*nameP && *nameP != '"' && j < 255) {
                if (*nameP == '\\') {
                    nameP++;
                    if (*nameP == 'n') name[j++] = '\n';
                    else if (*nameP == '"') name[j++] = '"';
                    else name[j++] = *nameP;
                } else {
                    name[j++] = *nameP;
                }
                nameP++;
            }
            result.songName = utf8ToUnicode(name);
        }

        const char* artistsP = strstr(idP, "\"artists\":[");
        if (artistsP) {
            const char* artistP = strstr(artistsP, "\"name\":\"");
            if (artistP) {
                artistP += 8;
                char artist[256] = {0};
                j = 0;
                while (*artistP && *artistP != '"' && j < 255) {
                    if (*artistP == '\\') {
                        artistP++;
                        if (*artistP == 'n') artist[j++] = '\n';
                        else if (*artistP == '"') artist[j++] = '"';
                        else artist[j++] = *artistP;
                    } else {
                        artist[j++] = *artistP;
                    }
                    artistP++;
                }
                result.artistName = utf8ToUnicode(artist);
            }
        }

        results.push_back(result);
        p = idP;
    }

    return !results.empty();
}

bool NetEaseAPI::getComments(const std::wstring& songId, std::vector<Comment>& comments, int limit) {
    std::string url = "https://music.163.com/api/v1/resource/comments/R_SO_4_";
    std::string songIdAnsi;
    for (wchar_t c : songId) {
        if (isdigit(c)) songIdAnsi += (char)c;
    }
    url += songIdAnsi;
    url += "?limit=" + std::to_string(limit) + "&offset=0";

    std::string response = httpGet(url);
    if (response.empty()) return false;

    const char* p = response.c_str();
    const char* end = p + response.length();

    while ((int)comments.size() < limit) {
        const char* contentP = strstr(p, "\"content\":\"");
        if (!contentP || contentP >= end) break;
        contentP += 11;

        char content[1024] = {0};
        int j = 0;
        while (*contentP && *contentP != '"' && j < 1023) {
            if (*contentP == '\\') {
                contentP++;
                if (*contentP == 'n') content[j++] = '\n';
                else if (*contentP == '"') content[j++] = '"';
                else content[j++] = *contentP;
            } else {
                content[j++] = *contentP;
            }
            contentP++;
        }

        Comment comment;
        comment.content = utf8ToUnicode(content);

        const char* likedP = strstr(contentP, "\"likedCount\":");
        if (likedP) {
            likedP += 13;
            while (*likedP && !isdigit(*likedP)) likedP++;
            if (isdigit(*likedP)) {
                char likedStr[32] = {0};
                j = 0;
                while (*likedP && isdigit(*likedP) && j < 31) {
                    likedStr[j++] = *likedP++;
                }
                comment.likeCount = atoi(likedStr);
            }
        }

        comments.push_back(comment);
        p = contentP;
    }

    return !comments.empty();
}

bool NetEaseAPI::getCommentsByKeyword(const std::wstring& songName, const std::wstring& artistName,
                                      std::vector<Comment>& comments, int limit) {
    std::wstring keyword = songName;
    if (!artistName.empty()) {
        keyword += L" " + artistName;
    }

    std::vector<SongSearchResult> results;
    if (!searchSong(keyword, results)) {
        return false;
    }

    if (results.empty()) return false;

    return getComments(results[0].songId, comments, limit);
}

std::string NetEaseAPI::httpGet(const std::string& url) {
    std::string response;

    HINTERNET hInternet = InternetOpenW(L"FooBarDanmaku/1.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!hInternet) return response;

    HINTERNET hConnect = InternetOpenUrlW(hInternet,
        std::wstring(url.begin(), url.end()).c_str(),
        nullptr, 0,
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_RELOAD,
        0);
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return response;
    }

    char buffer[8192];
    DWORD bytesRead = 0;

    while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
        bytesRead = 0;
    }

    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    return response;
}

std::string NetEaseAPI::httpPost(const std::string& url, const std::string& postData) {
    std::string response;

    HINTERNET hInternet = InternetOpenW(L"FooBarDanmaku/1.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!hInternet) return response;

    HINTERNET hConnect = InternetOpenUrlW(hInternet,
        std::wstring(url.begin(), url.end()).c_str(),
        nullptr, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return response;
    }

    const char* headers = "Content-Type: application/x-www-form-urlencoded";
    HINTERNET hRequest = HttpOpenRequestW(hConnect, L"POST", nullptr, nullptr, nullptr, nullptr, 0, 0);
    if (!hRequest) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return response;
    }

    HttpSendRequestA(hRequest, headers, -1, (LPVOID)postData.c_str(), (DWORD)postData.length());

    char buffer[8192];
    DWORD bytesRead = 0;

    while (InternetReadFile(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    return response;
}

std::wstring NetEaseAPI::utf8ToUnicode(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len == 0) return std::wstring();
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], len);
    return result;
}

std::string NetEaseAPI::unicodeToUtf8(const std::wstring& unicode) {
    if (unicode.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, unicode.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len == 0) return std::string();
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, unicode.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}