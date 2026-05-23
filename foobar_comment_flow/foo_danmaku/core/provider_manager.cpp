/* provider_manager.cpp
 *
 * Implements global g_music lifecycle, provider discovery, and the
 * foobar2000 initquit hook that wires everything together at startup.
 */

#include "core/provider_manager.h"
#include "music_client.h"
#include "music_provider.h"

#include <foobar2000/SDK/foobar2000.h>
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>

/* ── global handle ──────────────────────────────────── */

MusicClientHandle g_music = nullptr;

/* ── cfg: provider load order ──────────────────────── */
// Stored as comma-separated DLL filenames (not full paths), e.g.:
//   "netease_client.dll,qqmusic_client.dll"
// Empty string means "load all discovered providers alphabetically".

// {A3F82D51-9C14-4B7E-A031-CC5F68B2D4F0}
static constexpr GUID guid_cfg_provider_order =
{ 0xa3f82d51, 0x9c14, 0x4b7e, { 0xa0, 0x31, 0xcc, 0x5f, 0x68, 0xb2, 0xd4, 0xf0 } };

static cfg_string g_cfg_provider_order(guid_cfg_provider_order, "");

/* ── helpers ──────────────────────────────────────────── */

static std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

static std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}

// Extract filename (no path, no extension) from a full path
static std::wstring filename_only(const std::wstring& path) {
    auto sep = path.rfind(L'\\');
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

// Return the directory that contains foo_danmaku.dll
static std::wstring get_components_dir() {
    wchar_t buf[MAX_PATH] = {};
    HMODULE self = GetModuleHandleW(L"foo_danmaku.dll");
    if (!self || GetModuleFileNameW(self, buf, MAX_PATH) == 0) return {};
    wchar_t* sep = wcsrchr(buf, L'\\');
    if (sep) *sep = L'\0';
    return buf;
}

/* ── provider discovery ──────────────────────────────── */

std::vector<std::wstring> discover_providers(const std::wstring& dir) {
    std::vector<std::wstring> result;
    if (dir.empty()) return result;

    std::wstring pattern = dir + L"\\*.dll";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return result;

    do {
        if (wcscmp(fd.cFileName, L"foo_danmaku.dll") == 0) continue;

        std::wstring full = dir + L"\\" + fd.cFileName;
        HMODULE dll = LoadLibraryW(full.c_str());
        if (!dll) continue;
        bool ok = (GetProcAddress(dll, MUSIC_PROVIDER_VTABLE_EXPORT) != nullptr);
        FreeLibrary(dll);
        if (ok) result.push_back(full);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    std::sort(result.begin(), result.end(),
        [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(filename_only(a).c_str(), filename_only(b).c_str()) < 0;
        });
    return result;
}

/* ── reload / save ───────────────────────────────────── */

void reload_providers() {
    if (!g_music) return;

    const std::wstring dir = get_components_dir();
    auto discovered = discover_providers(dir);
    if (discovered.empty()) return;

    // Parse the stored order (filenames, comma-separated)
    std::vector<std::wstring> ordered;
    {
        std::string stored = g_cfg_provider_order.get_ptr();
        std::istringstream ss(stored);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token.empty()) continue;
            // Find full path in discovered list
            std::wstring wtoken = utf8_to_wide(token.c_str());
            for (auto& p : discovered)
                if (_wcsicmp(filename_only(p).c_str(), wtoken.c_str()) == 0)
                    { ordered.push_back(p); break; }
        }
    }
    // Append any discovered providers not yet in ordered list
    for (auto& p : discovered) {
        bool found = false;
        for (auto& o : ordered)
            if (_wcsicmp(o.c_str(), p.c_str()) == 0) { found = true; break; }
        if (!found) ordered.push_back(p);
    }

    // Load in order (skip duplicates)
    for (auto& path : ordered)
        music_client_load_provider(g_music, path.c_str(), nullptr);
}

void save_provider_order(const std::vector<std::wstring>& ordered_paths) {
    // Persist filenames to config
    std::string stored;
    for (auto& p : ordered_paths) {
        if (!stored.empty()) stored += ',';
        stored += wide_to_utf8(filename_only(p).c_str());
    }
    g_cfg_provider_order = stored.c_str();

    // Build new_order[] mapping current indices to desired positions
    if (!g_music) return;
    int n = music_client_get_provider_count(g_music);
    if (n <= 0) return;

    std::vector<int> new_order;
    new_order.reserve(n);
    for (auto& desired : ordered_paths) {
        for (int i = 0; i < n; i++) {
            const wchar_t* cur = music_client_get_provider_path(g_music, i);
            if (cur && _wcsicmp(filename_only(desired).c_str(),
                                filename_only(cur).c_str()) == 0) {
                new_order.push_back(i);
                break;
            }
        }
    }
    if ((int)new_order.size() == n)
        music_client_reorder_providers(g_music, new_order.data(), n);
}

/* ── foobar2000 initquit ─────────────────────────────── */

class DanmakuProviderInit : public initquit {
public:
    void on_init() override {
        g_music = music_client_create();
        if (!g_music) return;

        music_client_set_log(g_music, [](const wchar_t* msg, void*) {
            int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                std::string s(n - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, msg, -1, &s[0], n, nullptr, nullptr);
                console::print(s.c_str());
            }
        }, nullptr);

        reload_providers();
    }

    void on_quit() override {
        if (g_music) {
            music_client_destroy(g_music);
            g_music = nullptr;
        }
    }
};

static initquit_factory_t<DanmakuProviderInit> g_provider_init_factory;
