#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>

namespace netease {

/**
 * Thin WinHTTP wrapper for HTTPS POST to music.163.com.
 * Not thread-safe; use one instance per thread (or protect externally).
 */
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    /** Optional cookie override (MUSIC_U=…;__csrf=…). */
    void set_cookie(const std::wstring& cookie);

    /**
     * POST to https://music.163.com<path>
     * body is application/x-www-form-urlencoded.
     */
    bool post(
        const std::wstring& path,
        const std::string&  body,
        std::string&        out_response,
        std::wstring&       error_msg
    );

    /** GET https://music.163.com<path>  (path may include ?query) */
    bool get(
        const std::wstring& path,
        std::string&        out_response,
        std::wstring&       error_msg
    );

private:
    HINTERNET   m_session  = nullptr;
    HINTERNET   m_connect  = nullptr;
    std::wstring m_cookie;

    bool ensure_connect(std::wstring& error_msg);
    void close_connect();
};

} // namespace netease
