/* json.cpp — minimal JSON helpers (no third-party deps) */

#include "json.h"
#include <windows.h>
#include <cstring>

namespace netease {
namespace json {

/* ── internal: unescape a raw JSON string value ─────── */

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
                /* \uXXXX — simplified: just keep raw UTF-8 bytes */
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

/* ── str ─────────────────────────────────────────────── */

std::string str(const std::string& js, const std::string& key,
                const std::string& def)
{
    /* Search for  "key":"  or  "key": " */
    std::string pat = "\"" + key + "\":";
    const char* p = js.c_str();
    const char* found = strstr(p, pat.c_str());
    if (!found) return def;
    found += pat.size();
    while (*found == ' ' || *found == '\t') ++found;
    if (*found != '"') return def;
    ++found; /* skip opening quote */
    const char* start = found;
    /* scan to closing unescaped quote */
    while (*found && (*found != '"' || (found > start && *(found-1) == '\\'))) {
        if (*found == '\\') { ++found; if (*found) ++found; }
        else                  ++found;
    }
    return unescape(start, found);
}

/* ── num ─────────────────────────────────────────────── */

long long num(const std::string& js, const std::string& key, long long def) {
    std::string pat = "\"" + key + "\":";
    const char* p = js.c_str();
    const char* found = strstr(p, pat.c_str());
    if (!found) return def;
    found += pat.size();
    while (*found == ' ' || *found == '\t') ++found;
    if (*found == '"') ++found; /* handle quoted numbers */
    if (!(*found == '-' || (*found >= '0' && *found <= '9'))) return def;
    char* endp;
    long long v = strtoll(found, &endp, 10);
    return (endp > found) ? v : def;
}

/* ── object_raw ──────────────────────────────────────── */

std::string object_raw(const std::string& js, const std::string& key) {
    std::string pat = "\"" + key + "\":{";
    const char* p = js.c_str();
    const char* found = strstr(p, pat.c_str());
    if (!found) return "";
    found += pat.size() - 1; /* position at '{' */
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

/* ── array_raw ───────────────────────────────────────── */

std::string array_raw(const std::string& js, const std::string& key) {
    std::string pat = "\"" + key + "\":[";
    const char* p = js.c_str();
    const char* found = strstr(p, pat.c_str());
    if (!found) return "";
    found += pat.size() - 1; /* position at '[' */
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

/* ── array_items ─────────────────────────────────────── */

std::vector<std::string> array_items(const std::string& arr) {
    std::vector<std::string> result;
    const char* p = arr.c_str();
    /* skip leading '[' */
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

/* ── to_wide ─────────────────────────────────────────── */

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], n);
    return out;
}

/* ── to_utf8 ─────────────────────────────────────────── */

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &out[0], n, nullptr, nullptr);
    return out;
}

} // namespace json
} // namespace netease
