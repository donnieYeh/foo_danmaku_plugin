/* http.cpp — WinHTTP-based HTTPS client for music.163.com */

#include "http.h"
#include "logger.h"
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace netease {

static const wchar_t* kHost     = L"music.163.com";
static const DWORD    kPort     = INTERNET_DEFAULT_HTTPS_PORT;
static const wchar_t* kUA       =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    L"AppleWebKit/537.36 (KHTML, like Gecko) "
    L"Chrome/124.0.0.0 Safari/537.36";

static const wchar_t* kRefererHeaders =
    L"Accept: */*\r\n"
    L"Accept-Language: zh-CN,zh;q=0.9\r\n"
    L"Referer: https://music.163.com/\r\n"
    L"Origin: https://music.163.com\r\n";

/* ── lifecycle ───────────────────────────────────────── */

HttpClient::HttpClient() {
    m_session = WinHttpOpen(
        kUA,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    /* Generate a random anonymous NMTID cookie */
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring nmtid;
    nmtid.reserve(32);
    BYTE rnd[16];
    BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    for (int i = 0; i < 16; ++i) {
        nmtid += kHex[(rnd[i] >> 4) & 0xF];
        nmtid += kHex[ rnd[i]       & 0xF];
    }
    /* Default anonymous cookie */
    m_cookie = L"NMTID=" + nmtid + L"; os=pc; appver=2.9.7";
}

HttpClient::~HttpClient() {
    close_connect();
    if (m_session) { WinHttpCloseHandle(m_session); m_session = nullptr; }
}

void HttpClient::set_cookie(const std::wstring& cookie) {
    m_cookie = cookie;
    close_connect(); /* force reconnect with new cookie */
}

void HttpClient::close_connect() {
    if (m_connect) { WinHttpCloseHandle(m_connect); m_connect = nullptr; }
}

bool HttpClient::ensure_connect(std::wstring& error_msg) {
    if (m_connect) return true;
    if (!m_session) { error_msg = L"WinHTTP session not initialized"; return false; }

    m_connect = WinHttpConnect(m_session, kHost, kPort, 0);
    if (!m_connect) {
        error_msg = L"WinHttpConnect failed (err=" +
                    std::to_wstring(GetLastError()) + L")";
        return false;
    }
    return true;
}

/* ── shared request helper ───────────────────────────── */

static bool do_request(
    HINTERNET           hConnect,
    const wchar_t*      verb,          /* L"GET" or L"POST" */
    const std::wstring& path,
    const std::wstring& extra_headers, /* e.g. Content-Type */
    const std::string&  body,          /* empty for GET */
    const std::wstring& cookie,
    std::string&        out_response,
    std::wstring&       error_msg)
{
    HINTERNET hReq = WinHttpOpenRequest(
        hConnect, verb, path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (!hReq) {
        error_msg = L"WinHttpOpenRequest failed (err=" +
                    std::to_wstring(GetLastError()) + L")";
        log(error_msg);
        return false;
    }
    log(std::wstring(verb) + L" " + path);

    /* Merge headers */
    std::wstring hdrs = kRefererHeaders + extra_headers;
    hdrs += L"Cookie: " + cookie + L"\r\n";
    WinHttpAddRequestHeaders(hReq, hdrs.c_str(), (DWORD)hdrs.size(),
        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    /* Auto-decompress gzip/deflate from server */
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(hReq, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));

    const void* bodyPtr  = body.empty() ? nullptr : (const void*)body.data();
    DWORD       bodySize = (DWORD)body.size();

    BOOL ok = WinHttpSendRequest(hReq,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)bodyPtr, bodySize, bodySize, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);

    if (!ok) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hReq);
        error_msg = L"Send/Receive failed (err=" + std::to_wstring(err) + L")";
        log(error_msg);
        return false;
    }

    /* Check status */
    DWORD statusCode = 0, scLen = sizeof(statusCode);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &statusCode, &scLen, nullptr);
    log(L"HTTP " + std::to_wstring(statusCode));

    if (statusCode != 200) {
        WinHttpCloseHandle(hReq);
        error_msg = L"HTTP " + std::to_wstring(statusCode);
        log(error_msg);
        return false;
    }

    /* Read body */
    out_response.clear();
    char buf[8192];
    DWORD bytesRead = 0;
    for (;;) {
        if (!WinHttpReadData(hReq, buf, sizeof(buf) - 1, &bytesRead)) {
            log(L"WinHttpReadData err=" + std::to_wstring(GetLastError()));
            break;
        }
        if (bytesRead == 0) break;
        out_response.append(buf, bytesRead);
        bytesRead = 0;
    }

    WinHttpCloseHandle(hReq);
    loga("response bytes=" + std::to_string(out_response.size())
         + "  [0:200]=" + out_response.substr(0, 200));
    return true;
}

/* ── POST ────────────────────────────────────────────── */

bool HttpClient::post(
    const std::wstring& path,
    const std::string&  body,
    std::string&        out_response,
    std::wstring&       error_msg)
{
    if (!ensure_connect(error_msg)) return false;
    return do_request(m_connect, L"POST", path,
        L"Content-Type: application/x-www-form-urlencoded\r\n",
        body, m_cookie, out_response, error_msg);
}

/* ── GET ─────────────────────────────────────────────── */

bool HttpClient::get(
    const std::wstring& path,
    std::string&        out_response,
    std::wstring&       error_msg)
{
    if (!ensure_connect(error_msg)) return false;
    return do_request(m_connect, L"GET", path,
        L"", {}, m_cookie, out_response, error_msg);
}

} // namespace netease
