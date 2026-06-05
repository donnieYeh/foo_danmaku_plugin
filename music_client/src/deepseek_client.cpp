#include "deepseek_client.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

static std::string g_api_key;
static std::mutex g_api_key_mutex;

void deepseek_set_api_key(const char* api_key) {
    std::lock_guard<std::mutex> lock(g_api_key_mutex);
    if (api_key) {
        g_api_key = api_key;
    } else {
        g_api_key.clear();
    }
}

bool deepseek_has_api_key() {
    std::lock_guard<std::mutex> lock(g_api_key_mutex);
    return !g_api_key.empty();
}


static std::string get_api_key() {
    std::lock_guard<std::mutex> lock(g_api_key_mutex);
    return g_api_key;
}

// ── simple local logging ──────────────────────────────────
static void ds_log(const std::wstring& msg) {
    std::wstring out = L"[DeepSeek] " + msg + L"\n";
    OutputDebugStringW(out.c_str());
}

// ── minimal JSON helpers ──────────────────────────────────
namespace json {

static std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

static std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], n);
    return out;
}

static std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            static const char kHex[] = "0123456789ABCDEF";
            out += "\\u00";
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        } else {
            out += (char)c;
        }
    }
    return out;
}

static std::string unescape(const char* p, const char* end) {
    std::string out;
    out.reserve(end - p);
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            ++p;
            switch (*p) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                if (p + 4 < end) {
                    char hex[5] = {p[1],p[2],p[3],p[4],0};
                    unsigned cp = (unsigned)strtol(hex, nullptr, 16);
                    p += 4;
                    if (cp < 0x80) {
                        out += (char)cp;
                    } else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            }
            default: out += *p; break;
            }
        } else {
            out += *p;
        }
        ++p;
    }
    return out;
}

static std::string str(const std::string& js, const std::string& key, const std::string& def = "") {
    std::string pat = "\"" + key + "\":";
    const char* p = js.c_str();
    const char* found = strstr(p, pat.c_str());
    if (!found) return def;
    found += pat.size();
    while (*found == ' ' || *found == '\t') ++found;
    if (*found != '"') return def;
    ++found;
    const char* start = found;
    while (*found && (*found != '"' || (found > start && *(found-1) == '\\'))) {
        if (*found == '\\') { ++found; if (*found) ++found; }
        else                  ++found;
    }
    return unescape(start, found);
}

static std::string object_raw(const std::string& js, const std::string& key) {
    std::string pat = "\"" + key + "\":{";
    const char* p = js.c_str();
    const char* found = strstr(p, pat.c_str());
    if (!found) return "";
    found += pat.size() - 1;
    const char* start = found;
    int depth = 0;
    while (*found) {
        if (*found == '{')       ++depth;
        else if (*found == '}') { --depth; if (depth == 0) { ++found; break; } }
        else if (*found == '"') {
            ++found;
            while (*found && *found != '"') {
                if (*found == '\\') { ++found; if (*found) ++found; }
                else ++found;
            }
        }
        ++found;
    }
    return std::string(start, found);
}

static std::string array_raw(const std::string& js, const std::string& key) {
    std::string pat = "\"" + key + "\":[";
    const char* p = js.c_str();
    const char* found = strstr(p, pat.c_str());
    if (!found) return "";
    found += pat.size() - 1;
    const char* start = found;
    int depth = 0;
    while (*found) {
        if (*found == '[')       ++depth;
        else if (*found == ']') { --depth; if (depth == 0) { ++found; break; } }
        else if (*found == '"') {
            ++found;
            while (*found && *found != '"') {
                if (*found == '\\') { ++found; if (*found) ++found; }
                else ++found;
            }
        }
        ++found;
    }
    return std::string(start, found);
}

static std::vector<std::string> array_items(const std::string& arr) {
    std::vector<std::string> result;
    const char* p = arr.c_str();
    while (*p && *p != '[') ++p;
    if (*p == '[') ++p;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') ++p;
        if (*p == ']' || *p == '\0') break;
        if (*p != '{') { ++p; continue; }

        const char* start = p;
        int depth = 0;
        while (*p) {
            if (*p == '{')       ++depth;
            else if (*p == '}') { --depth; if (depth == 0) { ++p; break; } }
            else if (*p == '"') {
                ++p;
                while (*p && *p != '"') {
                    if (*p == '\\') { ++p; if (*p) ++p; }
                    else ++p;
                }
            }
            ++p;
        }
        result.push_back(std::string(start, p));
    }
    return result;
}

} // namespace json

// ── Cache File Management ─────────────────────────────────
struct CacheVariant {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
};

struct CacheEntry {
    std::wstring q_title;
    std::wstring q_artist;
    std::wstring q_album;
    std::vector<CacheVariant> variants;
};

static std::vector<CacheEntry> g_cache;
static std::mutex g_cache_mutex;
static bool g_cache_loaded = false;

static std::wstring get_cache_file_path() {
    wchar_t buf[MAX_PATH] = {};
    HMODULE self = GetModuleHandleW(L"foo_danmaku.dll");
    if (!self || GetModuleFileNameW(self, buf, MAX_PATH) == 0) {
        return L"deepseek_cache.json";
    }
    wchar_t* sep = wcsrchr(buf, L'\\');
    if (sep) {
        *sep = L'\0';
        return std::wstring(buf) + L"\\deepseek_cache.json";
    }
    return L"deepseek_cache.json";
}

static void load_cache_file() {
    if (g_cache_loaded) return;
    g_cache_loaded = true;

    std::wstring path = get_cache_file_path();
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    std::string items_raw = json::array_raw(content, "items");
    auto items = json::array_items(items_raw);
    for (const auto& item : items) {
        CacheEntry e;
        e.q_title  = json::to_wide(json::str(item, "qt"));
        e.q_artist = json::to_wide(json::str(item, "qar"));
        e.q_album  = json::to_wide(json::str(item, "qal"));
        
        std::string vars_raw = json::array_raw(item, "vars");
        if (!vars_raw.empty()) {
            auto var_items = json::array_items(vars_raw);
            for (const auto& v : var_items) {
                CacheVariant cv;
                cv.title = json::to_wide(json::str(v, "st"));
                cv.artist = json::to_wide(json::str(v, "sar"));
                cv.album = json::to_wide(json::str(v, "sal"));
                e.variants.push_back(cv);
            }
        } else {
            // Migrating old flat cache format
            std::wstring st = json::to_wide(json::str(item, "st"));
            if (!st.empty()) {
                CacheVariant cv;
                cv.title = st;
                cv.artist = json::to_wide(json::str(item, "sar"));
                cv.album = json::to_wide(json::str(item, "sal"));
                e.variants.push_back(cv);
            }
        }
        g_cache.push_back(e);
    }
    ds_log(L"Loaded " + std::to_wstring(g_cache.size()) + L" entries from cache file.");
}

static void save_cache_file() {
    std::wstring path = get_cache_file_path();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return;

    std::string out = "{\"items\":[\n";
    for (size_t i = 0; i < g_cache.size(); ++i) {
        const auto& e = g_cache[i];
        out += "  {";
        out += "\"qt\":\"" + json::escape(json::to_utf8(e.q_title)) + "\",";
        out += "\"qar\":\"" + json::escape(json::to_utf8(e.q_artist)) + "\",";
        out += "\"qal\":\"" + json::escape(json::to_utf8(e.q_album)) + "\",";
        out += "\"vars\":[\n";
        for (size_t j = 0; j < e.variants.size(); ++j) {
            const auto& v = e.variants[j];
            out += "    {";
            out += "\"st\":\"" + json::escape(json::to_utf8(v.title)) + "\",";
            out += "\"sar\":\"" + json::escape(json::to_utf8(v.artist)) + "\",";
            out += "\"sal\":\"" + json::escape(json::to_utf8(v.album)) + "\"";
            out += "}";
            if (j + 1 < e.variants.size()) out += ",";
            out += "\n";
        }
        out += "  ]}";
        if (i + 1 < g_cache.size()) out += ",";
        out += "\n";
    }
    out += "]}";

    f.write(out.data(), out.size());
    f.close();
}

static bool check_cache(
    const std::wstring& q_title,
    const std::wstring& q_artist,
    const std::wstring& q_album,
    DeepSeekCleanResult& out_result)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    load_cache_file();

    for (const auto& e : g_cache) {
        if (e.q_title == q_title && e.q_artist == q_artist && e.q_album == q_album) {
            out_result.variants.clear();
            for (const auto& v : e.variants) {
                DeepSeekSongVariant dv;
                dv.title = v.title;
                dv.artist = v.artist;
                dv.album = v.album;
                out_result.variants.push_back(dv);
            }
            return true;
        }
    }
    return false;
}

static void write_cache(
    const std::wstring& q_title,
    const std::wstring& q_artist,
    const std::wstring& q_album,
    const DeepSeekCleanResult& res)
{
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    load_cache_file();

    // Check duplication
    for (const auto& e : g_cache) {
        if (e.q_title == q_title && e.q_artist == q_artist && e.q_album == q_album) {
            return;
        }
    }

    CacheEntry e;
    e.q_title  = q_title;
    e.q_artist = q_artist;
    e.q_album  = q_album;
    for (const auto& v : res.variants) {
        CacheVariant cv;
        cv.title = v.title;
        cv.artist = v.artist;
        cv.album = v.album;
        e.variants.push_back(cv);
    }

    g_cache.push_back(e);
    save_cache_file();
}

void deepseek_clear_cache() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache.clear();
    std::wstring path = get_cache_file_path();
    DeleteFileW(path.c_str());
    ds_log(L"DeepSeek cache cleared (memory + file deleted).");
}

// ── DeepSeek API Client Call ──────────────────────────────
bool deepseek_clean_metadata(
    const std::wstring& q_title,
    const std::wstring& q_artist,
    const std::wstring& q_album,
    DeepSeekCleanResult& out_result,
    std::wstring&       error_msg)
{
    // 1. Check local cache
    if (check_cache(q_title, q_artist, q_album, out_result)) {
        ds_log(L"Cache hit for: " + q_title + L" / " + q_artist);
        return true;
    }

    std::string api_key = get_api_key();
    if (api_key.empty()) {
        error_msg = L"DeepSeek API key is not configured.";
        ds_log(error_msg);
        return false;
    }

    ds_log(L"Cache miss. Contacting DeepSeek API for: " + q_title);

    // 2. Build JSON Request Body
    std::string prompt = "Analyze and clean the following song:\n"
                         "Song Name: " + json::to_utf8(q_title) + "\n"
                         "Artist: " + json::to_utf8(q_artist) + "\n"
                         "Album: " + json::to_utf8(q_album);

    std::string system_prompt = "You are a music metadata cleaning assistant. You must analyze the given song information (which may contain noise like live version, format details, CV names, track numbers, etc.) and extract/standardize it into clean song name, artist name, and album name. Always respond in JSON format with a \"variants\" key containing an array of objects (each with keys \"title\", \"artist\", and \"album\"). If the song is of Japanese/anime origin, you MUST provide exactly two variants: the first with the English/Romaji title (e.g. \"Ichigo Complete\"), and the second with the original Japanese title (e.g. \"いちごコンプリート\"). Otherwise, return a single variant.";

    std::string body = "{\n"
                       "  \"model\": \"deepseek-v4-flash\",\n"
                       "  \"messages\": [\n"
                       "    {\"role\": \"system\", \"content\": \"" + json::escape(system_prompt) + "\"},\n"
                       "    {\"role\": \"user\", \"content\": \"" + json::escape(prompt) + "\"}\n"
                       "  ],\n"
                       "  \"response_format\": {\n"
                       "    \"type\": \"json_object\"\n"
                       "  },\n"
                       "  \"thinking\": {\n"
                       "    \"type\": \"disabled\"\n"
                       "  }\n"
                       "}";

    // 3. Make WinHTTP request to DeepSeek
    HINTERNET hSession = WinHttpOpen(L"foo_danmaku/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        error_msg = L"WinHttpOpen failed (err=" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.deepseek.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        error_msg = L"WinHttpConnect failed (err=" + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", L"/chat/completions", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) {
        error_msg = L"WinHttpOpenRequest failed (err=" + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring extra_headers = L"Content-Type: application/json\r\n";
    extra_headers += L"Authorization: Bearer " + json::to_wide(api_key) + L"\r\n";
    WinHttpAddRequestHeaders(hReq, extra_headers.c_str(), (DWORD)extra_headers.size(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);

    if (!ok) {
        error_msg = L"HTTP request failed (err=" + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0, scLen = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &scLen, nullptr);
    if (statusCode != 200) {
        error_msg = L"DeepSeek HTTP status " + std::to_wstring(statusCode);
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::string response;
    char buf[4096];
    DWORD bytesRead = 0;
    for (;;) {
        if (!WinHttpReadData(hReq, buf, sizeof(buf), &bytesRead)) break;
        if (bytesRead == 0) break;
        response.append(buf, bytesRead);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // 4. Parse the OpenAI JSON output
    // response.choices[0].message.content
    std::string choices_raw = json::array_raw(response, "choices");
    auto choices = json::array_items(choices_raw);
    if (choices.empty()) {
        error_msg = L"DeepSeek returned empty choices";
        return false;
    }

    std::string message_blk = json::object_raw(choices[0], "message");
    std::string content_str = json::str(message_blk, "content");
    if (content_str.empty()) {
        error_msg = L"DeepSeek response message has no content";
        return false;
    }

    // Parse variants array from content_str
    std::string vars_raw = json::array_raw(content_str, "variants");
    if (!vars_raw.empty()) {
        auto var_items = json::array_items(vars_raw);
        for (const auto& v : var_items) {
            DeepSeekSongVariant dv;
            dv.title  = json::to_wide(json::str(v, "title"));
            dv.artist = json::to_wide(json::str(v, "artist"));
            dv.album  = json::to_wide(json::str(v, "album"));
            if (!dv.title.empty()) {
                out_result.variants.push_back(dv);
            }
        }
    } else {
        // Fallback to flat structure
        std::string s_title  = json::str(content_str, "title");
        std::string s_artist = json::str(content_str, "artist");
        std::string s_album  = json::str(content_str, "album");
        if (!s_title.empty()) {
            DeepSeekSongVariant dv;
            dv.title  = json::to_wide(s_title);
            dv.artist = json::to_wide(s_artist);
            dv.album  = json::to_wide(s_album);
            out_result.variants.push_back(dv);
        }
    }

    if (out_result.variants.empty()) {
        error_msg = L"DeepSeek failed to extract any song variants";
        return false;
    }

    ds_log(L"Successfully retrieved " + std::to_wstring(out_result.variants.size()) + L" clean variant(s) from DeepSeek. First: " + out_result.variants[0].title + L" / " + out_result.variants[0].artist);

    // Write to persistent cache
    write_cache(q_title, q_artist, q_album, out_result);

    return true;
}
