#pragma once
/* logger.h — shared logging for all netease_client translation units.
 *
 * Each .cpp includes this header.
 * g_log_cb / g_log_userdata are defined (once) in logger.cpp.
 * Every call routes to:
 *   1. The registered NeteaseLogCallback (if set) → foobar2000 console
 *   2. OutputDebugStringW                         → DebugView
 */

#include "../include/netease_client.h"  /* NeteaseLogCallback typedef */
#include <windows.h>
#include <string>

namespace netease {

/* Defined in logger.cpp (one definition, extern everywhere else) */
extern NeteaseLogCallback g_log_cb;
extern void*              g_log_userdata;

inline void log(const wchar_t* msg) {
    if (g_log_cb) g_log_cb(msg, g_log_userdata);
    OutputDebugStringW(L"[netease] ");
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\n");
}

inline void log(const std::wstring& msg) { log(msg.c_str()); }

inline void loga(const char* msg) {
    /* UTF-8 → wide so the callback (foobar2000 console) shows Chinese/JP correctly */
    int n = MultiByteToWideChar(CP_UTF8, 0, msg, -1, nullptr, 0);
    if (n > 1) {
        std::wstring w(n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, msg, -1, &w[0], n);
        log(w.c_str());
    } else {
        /* fallback: raw ASCII via OutputDebugString only */
        OutputDebugStringA("[netease] ");
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
    }
}

inline void loga(const std::string& msg) { loga(msg.c_str()); }

} // namespace netease
