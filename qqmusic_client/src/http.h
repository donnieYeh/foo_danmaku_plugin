#pragma once
#include <windows.h>
#include <winhttp.h>
#include <string>

namespace qqmusic {

/**
 * Thin WinHTTP wrapper for HTTPS GET to c.y.qq.com.
 * Not thread-safe; use one instance per thread (or protect externally).
 */
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    /** Optional cookie override (uin=...; skey=...; p_skey=...). */
    void set_cookie(const std::wstring& cookie);

    /** GET https://c.y.qq.com<path>  (path may include ?query) */
    bool get(
        const std::wstring& path,
        std::string&        out_response,
        std::wstring&       error_msg
    );

private:
    HINTERNET    m_session = nullptr;
    HINTERNET    m_connect = nullptr;
    std::wstring m_cookie;

    bool ensure_connect(std::wstring& error_msg);
    void close_connect();
};

} // namespace qqmusic
