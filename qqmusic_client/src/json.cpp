/* json.cpp — minimal JSON helpers (no third-party deps)
 *
 * Handles both compact  {"key":value}
 * and pretty-printed    {"key" : value}  formats.
 */

#include "json.h"
#include <windows.h>
#include <cstring>

namespace qqmusic {
namespace json {

/* ── internal: find a JSON key and return pointer past ':' ── *
 *
 * Searches for  "key"  in [js], then skips optional whitespace
 * on both sides of the ':'.  Returns pointer to the first non-
 * whitespace character of the value, or nullptr if not found.
 *
 * To avoid false matches on values, we require that the character
 * immediately before the opening quote is NOT a letter/digit
 * (i.e., the key is not a substring of another key name).
 */
static const char* find_key(const char* js, const std::string& key) {
    std::string base = "\"" + key + "\"";
    const char* p = js;
    while (*p) {
        const char* found = strstr(p, base.c_str());
        if (!found) return nullptr;

        /* Advance past the key and optional whitespace to ':' */
        const char* after = found + base.size();
        while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')
            ++after;
        if (*after != ':') { p = found + 1; continue; }
        ++after; /* skip ':' */
        while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r')
            ++after;
        return after; /* points at value start */
    }
    return nullptr;
}

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
                /* \uXXXX — simplified: decode to UTF-8 */
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
    const char* val = find_key(js.c_str(), key);
    if (!val) return def;
    if (*val != '"') return def;
    ++val; /* skip opening quote */
    const char* start = val;
    /* scan to closing unescaped quote */
    while (*val && (*val != '"' || (val > start && *(val-1) == '\\'))) {
        if (*val == '\\') { ++val; if (*val) ++val; }
        else                ++val;
    }
    return unescape(start, val);
}

/* ── num ─────────────────────────────────────────────── */

long long num(const std::string& js, const std::string& key, long long def) {
    const char* val = find_key(js.c_str(), key);
    if (!val) return def;
    if (*val == '"') ++val; /* handle quoted numbers */
    if (!(*val == '-' || (*val >= '0' && *val <= '9'))) return def;
    char* endp;
    long long v = strtoll(val, &endp, 10);
    return (endp > val) ? v : def;
}

/* ── array_raw ───────────────────────────────────────── */

std::string array_raw(const std::string& js, const std::string& key) {
    const char* val = find_key(js.c_str(), key);
    if (!val || *val != '[') return "";
    const char* start = val;
    int depth = 0;
    while (*val) {
        if (*val == '[')       ++depth;
        else if (*val == ']') { --depth; if (depth == 0) { ++val; break; } }
        else if (*val == '"') {
            ++val;
            while (*val && *val != '"') {
                if (*val == '\\') { ++val; if (*val) ++val; }
                else ++val;
            }
        }
        ++val;
    }
    return std::string(start, val);
}

/* ── object_raw ──────────────────────────────────────── */

std::string object_raw(const std::string& js, const std::string& key) {
    const char* val = find_key(js.c_str(), key);
    if (!val || *val != '{') return "";
    const char* start = val;
    int depth = 0;
    while (*val) {
        if (*val == '{')       ++depth;
        else if (*val == '}') { --depth; if (depth == 0) { ++val; break; } }
        else if (*val == '"') {
            ++val;
            while (*val && *val != '"') {
                if (*val == '\\') { ++val; if (*val) ++val; }
                else ++val;
            }
        }
        ++val;
    }
    return std::string(start, val);
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
} // namespace qqmusic
