#include "ui/danmaku_preferences.h"
#include "core/provider_manager.h"
#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/SDK/cfg_var.h>
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <vector>
#include <string>
#pragma comment(lib, "comctl32.lib")

// {7C6DB3BB-6C17-46E3-9C3F-10CE511E7F0D}
static constexpr GUID guid_cfg_spawn_interval =
{ 0x7c6db3bb, 0x6c17, 0x46e3, { 0x9c, 0x3f, 0x10, 0xce, 0x51, 0x1e, 0x7f, 0x0d } };

// {84C7676B-1109-4F9A-9F2E-162F27F1D9C2}
static constexpr GUID guid_cfg_track_count =
{ 0x84c7676b, 0x1109, 0x4f9a, { 0x9f, 0x2e, 0x16, 0x2f, 0x27, 0xf1, 0xd9, 0xc2 } };

// {80E1D472-B84E-4CC6-9813-28E0B957B04C}
static constexpr GUID guid_cfg_speed_percent =
{ 0x80e1d472, 0xb84e, 0x4cc6, { 0x98, 0x13, 0x28, 0xe0, 0xb9, 0x57, 0xb0, 0x4c } };

// {C2C18B7E-E550-42AB-93C7-7E5D04CB8E93}
static constexpr GUID guid_cfg_turntable_speed_percent =
{ 0xc2c18b7e, 0xe550, 0x42ab, { 0x93, 0xc7, 0x7e, 0x5d, 0x04, 0xcb, 0x8e, 0x93 } };

// {B43E7AE1-9F3B-4A21-928B-E7DAE977CC41}
static constexpr GUID guid_cfg_bk_aspect_min =
{ 0xb43e7ae1, 0x9f3b, 0x4a21, { 0x92, 0x8b, 0xe7, 0xda, 0xe9, 0x77, 0xcc, 0x41 } };

// {F6D9643A-B1F6-4E92-B5A1-CB2278E2B17D}
static constexpr GUID guid_cfg_bk_aspect_max =
{ 0xf6d9643a, 0xb1f6, 0x4e92, { 0xb5, 0xa1, 0xcb, 0x22, 0x78, 0xe2, 0xb1, 0x7d } };

// {1A0C9E11-74A4-49C7-904A-B2C2BB566F87}
static constexpr GUID guid_pref_page =
{ 0x1a0c9e11, 0x74a4, 0x49c7, { 0x90, 0x4a, 0xb2, 0xc2, 0xbb, 0x56, 0x6f, 0x87 } };

static constexpr int kDefaultSpawnIntervalMs = 5000;
static constexpr int kMinSpawnIntervalMs = 150;
static constexpr int kMaxSpawnIntervalMs = 10000;
static constexpr int kDefaultTrackCount = 4; // fewer lanes than before => wider vertical spacing
static constexpr int kMinTrackCount = 2;
static constexpr int kMaxTrackCount = 10;
static constexpr int kDefaultSpeedPercent = 200; // 2x historical speed
static constexpr int kMinSpeedPercent = 50;
static constexpr int kMaxSpeedPercent = 300;
static constexpr int kDefaultTurntableSpeedPercent = 100;
static constexpr int kMinTurntableSpeedPercent = 20;
static constexpr int kMaxTurntableSpeedPercent = 300;
static constexpr int kDefaultBkAspectMinTenths = 18;
static constexpr int kDefaultBkAspectMaxTenths = 23;
static constexpr int kMinBkAspectTenths = 10;
static constexpr int kMaxBkAspectTenths = 35;

static cfg_int g_cfg_spawn_interval(guid_cfg_spawn_interval, kDefaultSpawnIntervalMs);
static cfg_int g_cfg_track_count(guid_cfg_track_count, kDefaultTrackCount);
static cfg_int g_cfg_speed_percent(guid_cfg_speed_percent, kDefaultSpeedPercent);
static cfg_int g_cfg_turntable_speed_percent(guid_cfg_turntable_speed_percent, kDefaultTurntableSpeedPercent);
static cfg_int g_cfg_bk_aspect_min(guid_cfg_bk_aspect_min, kDefaultBkAspectMinTenths);
static cfg_int g_cfg_bk_aspect_max(guid_cfg_bk_aspect_max, kDefaultBkAspectMaxTenths);

// {4D2A98BC-E2E1-4DAE-9F93-7848F2A832BE}
static constexpr GUID guid_cfg_deepseek_api_key =
{ 0x4d2a98bc, 0xe2e1, 0x4dae, { 0x9f, 0x93, 0x78, 0x48, 0xf2, 0xa8, 0x32, 0xbe } };
static cfg_string g_cfg_deepseek_api_key(guid_cfg_deepseek_api_key, "");

std::string danmaku_get_deepseek_api_key() { return g_cfg_deepseek_api_key.get_ptr(); }
void danmaku_set_deepseek_api_key(const char* val) { g_cfg_deepseek_api_key = val; }


static int clamp_interval(int v) {
    return std::max(kMinSpawnIntervalMs, std::min(kMaxSpawnIntervalMs, v));
}
static int clamp_tracks(int v) {
    return std::max(kMinTrackCount, std::min(kMaxTrackCount, v));
}
static int clamp_speed(int v) {
    return std::max(kMinSpeedPercent, std::min(kMaxSpeedPercent, v));
}
static int clamp_turntable_speed(int v) {
    return std::max(kMinTurntableSpeedPercent, std::min(kMaxTurntableSpeedPercent, v));
}
static int clamp_bk_aspect(int v) {
    return std::max(kMinBkAspectTenths, std::min(kMaxBkAspectTenths, v));
}

int danmaku_default_spawn_interval_ms() { return kDefaultSpawnIntervalMs; }
int danmaku_get_spawn_interval_ms() { return clamp_interval((int)g_cfg_spawn_interval.get()); }
void danmaku_set_spawn_interval_ms(int valueMs) { g_cfg_spawn_interval = clamp_interval(valueMs); }
int danmaku_default_track_count() { return kDefaultTrackCount; }
int danmaku_get_track_count() { return clamp_tracks((int)g_cfg_track_count.get()); }
void danmaku_set_track_count(int value) { g_cfg_track_count = clamp_tracks(value); }
int danmaku_default_speed_percent() { return kDefaultSpeedPercent; }
int danmaku_get_speed_percent() { return clamp_speed((int)g_cfg_speed_percent.get()); }
void danmaku_set_speed_percent(int value) { g_cfg_speed_percent = clamp_speed(value); }
float danmaku_get_base_speed() {
    // Historical base was 2px/frame at ~60fps = 120px/s.
    return 120.0f * ((float)danmaku_get_speed_percent() / 100.0f);
}
int danmaku_default_turntable_speed_percent() { return kDefaultTurntableSpeedPercent; }
int danmaku_get_turntable_speed_percent() { return clamp_turntable_speed((int)g_cfg_turntable_speed_percent.get()); }
void danmaku_set_turntable_speed_percent(int value) { g_cfg_turntable_speed_percent = clamp_turntable_speed(value); }
float danmaku_get_turntable_speed() {
    return 0.42f * ((float)danmaku_get_turntable_speed_percent() / 100.0f);
}
int danmaku_default_bk_aspect_min_tenths() { return kDefaultBkAspectMinTenths; }
int danmaku_default_bk_aspect_max_tenths() { return kDefaultBkAspectMaxTenths; }
int danmaku_get_bk_aspect_min_tenths() {
    int mn = clamp_bk_aspect((int)g_cfg_bk_aspect_min.get());
    int mx = clamp_bk_aspect((int)g_cfg_bk_aspect_max.get());
    return std::min(mn, mx);
}
int danmaku_get_bk_aspect_max_tenths() {
    int mn = clamp_bk_aspect((int)g_cfg_bk_aspect_min.get());
    int mx = clamp_bk_aspect((int)g_cfg_bk_aspect_max.get());
    return std::max(mn, mx);
}
void danmaku_set_bk_aspect_min_tenths(int value) { g_cfg_bk_aspect_min = clamp_bk_aspect(value); }
void danmaku_set_bk_aspect_max_tenths(int value) { g_cfg_bk_aspect_max = clamp_bk_aspect(value); }
float danmaku_get_bk_aspect_min() { return (float)danmaku_get_bk_aspect_min_tenths() / 10.0f; }
float danmaku_get_bk_aspect_max() { return (float)danmaku_get_bk_aspect_max_tenths() / 10.0f; }

class DanmakuPreferencesInstance : public preferences_page_instance {
public:
    DanmakuPreferencesInstance(HWND parent, preferences_page_callback::ptr cb)
        : m_parent(parent), m_callback(cb), m_wnd(nullptr),
          m_intervalEdit(nullptr), m_tracksEdit(nullptr), m_speedEdit(nullptr),
          m_turntableSpeedEdit(nullptr), m_bkAspectMinEdit(nullptr), m_bkAspectMaxEdit(nullptr),
          m_providerList(nullptr), m_dragSrc(-1), m_dragDst(-1), m_dragging(false) {
        createWindow();
    }

    ~DanmakuPreferencesInstance() {
        if (m_wnd) DestroyWindow(m_wnd);
    }

    t_uint32 get_state() override {
        t_uint32 state = preferences_state::resettable;
        if (hasChanged()) state |= preferences_state::changed;
        return state;
    }

    fb2k::hwnd_t get_wnd() override { return m_wnd; }

    void apply() override {
        danmaku_set_spawn_interval_ms(readInt(m_intervalEdit, kDefaultSpawnIntervalMs, clamp_interval));
        danmaku_set_track_count(readInt(m_tracksEdit, kDefaultTrackCount, clamp_tracks));
        danmaku_set_speed_percent(readInt(m_speedEdit, kDefaultSpeedPercent, clamp_speed));
        danmaku_set_turntable_speed_percent(readInt(m_turntableSpeedEdit, kDefaultTurntableSpeedPercent, clamp_turntable_speed));
        danmaku_set_bk_aspect_min_tenths(readInt(m_bkAspectMinEdit, kDefaultBkAspectMinTenths, clamp_bk_aspect));
        danmaku_set_bk_aspect_max_tenths(readInt(m_bkAspectMaxEdit, kDefaultBkAspectMaxTenths, clamp_bk_aspect));
        wchar_t keyBuf[512] = {};
        GetWindowTextW(m_apiKeyEdit, keyBuf, 512);
        pfc::stringcvt::string_utf8_from_wide utf8_key(keyBuf);
        danmaku_set_deepseek_api_key(utf8_key.get_ptr());
        if (g_music) {
            music_client_set_deepseek_api_key(g_music, utf8_key.get_ptr());
        }

        writeEdit(danmaku_get_spawn_interval_ms());
        writeTracks(danmaku_get_track_count());
        writeSpeed(danmaku_get_speed_percent());
        writeTurntableSpeed(danmaku_get_turntable_speed_percent());
        writeBkAspectMin(danmaku_get_bk_aspect_min_tenths());
        writeBkAspectMax(danmaku_get_bk_aspect_max_tenths());
        applyProviderOrder();
        if (m_callback.is_valid()) m_callback->on_state_changed();
    }

    void reset() override {
        writeEdit(kDefaultSpawnIntervalMs);
        writeTracks(kDefaultTrackCount);
        writeSpeed(kDefaultSpeedPercent);
        writeTurntableSpeed(kDefaultTurntableSpeedPercent);
        writeBkAspectMin(kDefaultBkAspectMinTenths);
        writeBkAspectMax(kDefaultBkAspectMaxTenths);
        SetWindowTextW(m_apiKeyEdit, L"");
        if (m_callback.is_valid()) m_callback->on_state_changed();
    }

private:
    enum { IDC_INTERVAL = 1001, IDC_TRACKS = 1002, IDC_SPEED = 1003, IDC_TURNTABLE_SPEED = 1004, IDC_BK_ASPECT_MIN = 1005, IDC_BK_ASPECT_MAX = 1006, IDC_PROVIDER_LIST = 1010, IDC_API_KEY = 1011, IDC_CLEAR_CACHE = 1012 };

    HWND m_parent;
    preferences_page_callback::ptr m_callback;
    HWND m_wnd;
    HWND m_intervalEdit;
    HWND m_tracksEdit;
    HWND m_speedEdit;
    HWND m_turntableSpeedEdit;
    HWND m_bkAspectMinEdit;
    HWND m_bkAspectMaxEdit;
    HWND m_providerList;
    HWND m_apiKeyEdit;
    int  m_dragSrc;
    int  m_dragDst;
    bool m_dragging;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<DanmakuPreferencesInstance*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<DanmakuPreferencesInstance*>(cs->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

        if (msg == WM_COMMAND &&
            (LOWORD(wp) == IDC_INTERVAL || LOWORD(wp) == IDC_TRACKS || LOWORD(wp) == IDC_SPEED ||
             LOWORD(wp) == IDC_TURNTABLE_SPEED || LOWORD(wp) == IDC_BK_ASPECT_MIN ||
             LOWORD(wp) == IDC_BK_ASPECT_MAX || LOWORD(wp) == IDC_API_KEY) &&
            HIWORD(wp) == EN_CHANGE) {
            if (self->m_callback.is_valid()) self->m_callback->on_state_changed();
            return 0;
        }

        if (msg == WM_COMMAND && LOWORD(wp) == IDC_CLEAR_CACHE && HIWORD(wp) == BN_CLICKED) {
            if (g_music) {
                music_client_clear_deepseek_cache(g_music);
                MessageBoxW(hwnd, L"DeepSeek cache has been successfully cleared.", L"FooBar Danmaku", MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }

        /* ── ListView drag-to-reorder ── */
        if (msg == WM_NOTIFY) {
            auto* nm = reinterpret_cast<NMHDR*>(lp);
            if (nm->hwndFrom == self->m_providerList && nm->code == LVN_BEGINDRAG) {
                auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lp);
                self->m_dragSrc = nmlv->iItem;
                self->m_dragDst = -1;   /* gap not yet determined */
                self->m_dragging = true;
                SetCapture(hwnd);
                return 0;
            }
        }
        if (msg == WM_MOUSEMOVE && self->m_dragging) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            MapWindowPoints(hwnd, self->m_providerList, &pt, 1);
            int total = ListView_GetItemCount(self->m_providerList);
            /* Compute gap: 0 = before item 0, N = after item N-1 */
            int gap = total;
            for (int i = 0; i < total; i++) {
                RECT rc = {};
                ListView_GetItemRect(self->m_providerList, i, &rc, LVIR_BOUNDS);
                if (pt.y < (rc.top + rc.bottom) / 2) { gap = i; break; }
            }
            if (gap != self->m_dragDst) {
                self->m_dragDst = gap;
                if (gap == 0) {
                    LVINSERTMARK lim = { sizeof(LVINSERTMARK), 0, 0, 0 };
                    ListView_SetInsertMark(self->m_providerList, &lim);
                } else {
                    /* Show mark AFTER item (gap-1) */
                    LVINSERTMARK lim = { sizeof(LVINSERTMARK), 0x00000001 /*LVIMF_AFTER*/, gap - 1, 0 };
                    ListView_SetInsertMark(self->m_providerList, &lim);
                }
            }
            return 0;
        }
        if (msg == WM_LBUTTONUP && self->m_dragging) {
            self->m_dragging = false;
            ReleaseCapture();
            /* Safe clear — never pass nullptr; LVIMF_INVALID is the portable way */
            LVINSERTMARK clr = { sizeof(LVINSERTMARK), 0xFFFFFFFF /*LVIMF_INVALID*/, -1, 0 };
            ListView_SetInsertMark(self->m_providerList, &clr);
            int src = self->m_dragSrc;
            int gap = self->m_dragDst; /* gap index: 0..N */
            /* gap==src means 'before src' (no-op), gap==src+1 means 'after src' (no-op) */
            if (gap >= 0 && gap != src && gap != src + 1) {
                /* When src < gap, deleting src shifts remaining items down by 1 */
                int dst = (src < gap) ? gap - 1 : gap;
                self->moveListItem(src, dst);
                if (self->m_callback.is_valid()) self->m_callback->on_state_changed();
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void createWindow() {
        HINSTANCE inst = (HINSTANCE)GetModuleHandleW(L"foo_danmaku.dll");
        if (!inst) inst = GetModuleHandleW(nullptr);

        const wchar_t* cls = L"FooBarDanmakuPrefsPage";
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);

        m_wnd = CreateWindowExW(0, cls, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 100, 100, m_parent, nullptr, inst, this);

        CreateWindowExW(0, L"STATIC",
            L"Danmaku sparsity / spawn interval (milliseconds):",
            WS_CHILD | WS_VISIBLE,
            12, 14, 320, 20, m_wnd, nullptr, inst, nullptr);

        m_intervalEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            12, 40, 90, 24, m_wnd, (HMENU)(INT_PTR)IDC_INTERVAL, inst, nullptr);

        CreateWindowExW(0, L"STATIC",
            L"Larger = sparser. Default: 5000 ms. Suggested range: 1000鈥?000 ms.",
            WS_CHILD | WS_VISIBLE,
            112, 43, 480, 20, m_wnd, nullptr, inst, nullptr);

        CreateWindowExW(0, L"STATIC",
            L"Line spacing / lane count:",
            WS_CHILD | WS_VISIBLE,
            12, 78, 220, 20, m_wnd, nullptr, inst, nullptr);

        m_tracksEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            12, 104, 90, 24, m_wnd, (HMENU)(INT_PTR)IDC_TRACKS, inst, nullptr);

        CreateWindowExW(0, L"STATIC",
            L"Fewer lanes = wider line spacing. Default: 4. Suggested range: 3鈥?.",
            WS_CHILD | WS_VISIBLE,
            112, 107, 520, 20, m_wnd, nullptr, inst, nullptr);

        CreateWindowExW(0, L"STATIC",
            L"Playback speed (%):",
            WS_CHILD | WS_VISIBLE,
            12, 142, 220, 20, m_wnd, nullptr, inst, nullptr);

        m_speedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            12, 168, 90, 24, m_wnd, (HMENU)(INT_PTR)IDC_SPEED, inst, nullptr);

        CreateWindowExW(0, L"STATIC",
            L"100 = old speed; 200 = 2x. Default: 200.",
            WS_CHILD | WS_VISIBLE,
            112, 171, 520, 20, m_wnd, nullptr, inst, nullptr);

        CreateWindowExW(0, L"STATIC",
            L"Turntable rotation speed (%):",
            WS_CHILD | WS_VISIBLE,
            12, 206, 560, 20, m_wnd, nullptr, inst, nullptr);

        m_turntableSpeedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
            12, 232, 90, 24, m_wnd, (HMENU)(INT_PTR)IDC_TURNTABLE_SPEED, inst, nullptr);

        CreateWindowExW(0, L"STATIC",
            L"Default: 100. Smaller = slower vinyl rotation. Suggested range: 60-150.",
            WS_CHILD | WS_VISIBLE,
            112, 235, 560, 20, m_wnd, nullptr, inst, nullptr);

        CreateWindowExW(0, L"STATIC",
            L"BK spread aspect ratio range (x10): min / max. Default: 18 - 23 = 1.8 - 2.3.",
            WS_CHILD | WS_VISIBLE,
            12, 270, 650, 20, m_wnd, nullptr, inst, nullptr);

        SendMessageW(m_intervalEdit, EM_SETLIMITTEXT, 5, 0);
        SendMessageW(m_tracksEdit, EM_SETLIMITTEXT, 2, 0);
        SendMessageW(m_speedEdit, EM_SETLIMITTEXT, 3, 0);
        SendMessageW(m_turntableSpeedEdit, EM_SETLIMITTEXT, 3, 0);
        m_bkAspectMinEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 12, 296, 60, 24, m_wnd, (HMENU)(INT_PTR)IDC_BK_ASPECT_MIN, inst, nullptr);
        m_bkAspectMaxEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 82, 296, 60, 24, m_wnd, (HMENU)(INT_PTR)IDC_BK_ASPECT_MAX, inst, nullptr);
        SendMessageW(m_bkAspectMinEdit, EM_SETLIMITTEXT, 2, 0);
        SendMessageW(m_bkAspectMaxEdit, EM_SETLIMITTEXT, 2, 0);
        writeEdit(danmaku_get_spawn_interval_ms());
        writeTracks(danmaku_get_track_count());
        writeSpeed(danmaku_get_speed_percent());
        writeTurntableSpeed(danmaku_get_turntable_speed_percent());
        writeBkAspectMin(danmaku_get_bk_aspect_min_tenths());
        writeBkAspectMax(danmaku_get_bk_aspect_max_tenths());

        /* ── Provider priority section ── */
        CreateWindowExW(0, L"STATIC",
            L"Music Provider Priority (drag rows to reorder; top = first tried):",
            WS_CHILD | WS_VISIBLE,
            12, 338, 600, 20, m_wnd, nullptr, inst, nullptr);

        m_providerList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
            12, 362, 380, 110, m_wnd,
            (HMENU)(INT_PTR)IDC_PROVIDER_LIST, inst, nullptr);

        ListView_SetExtendedListViewStyle(m_providerList,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW col = {};
        col.mask = LVCF_WIDTH;
        col.cx   = 356;
        ListView_InsertColumn(m_providerList, 0, &col);

        populateProviderList();

        /* ── DeepSeek API Key section ── */
        CreateWindowExW(0, L"STATIC",
            L"DeepSeek API Key (for LLM song matching fallback):",
            WS_CHILD | WS_VISIBLE,
            12, 485, 380, 20, m_wnd, nullptr, inst, nullptr);

        m_apiKeyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
            12, 505, 380, 24, m_wnd, (HMENU)(INT_PTR)IDC_API_KEY, inst, nullptr);

        SendMessageW(m_apiKeyEdit, EM_SETLIMITTEXT, 511, 0);

        std::string saved_key = danmaku_get_deepseek_api_key();
        pfc::stringcvt::string_wide_from_utf8 wkey(saved_key.c_str());
        SetWindowTextW(m_apiKeyEdit, wkey.get_ptr());

        CreateWindowExW(0, L"BUTTON", L"Clear Cache",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            400, 505, 100, 24, m_wnd, (HMENU)(INT_PTR)IDC_CLEAR_CACHE, inst, nullptr);
    }

    int readInt(HWND edit, int defaultValue, int (*clampFn)(int)) const {
        wchar_t buf[32] = {};
        GetWindowTextW(edit, buf, 32);
        int v = _wtoi(buf);
        if (v <= 0) v = defaultValue;
        return clampFn(v);
    }

    void writeEdit(int v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", clamp_interval(v));
        SetWindowTextW(m_intervalEdit, buf);
    }
    void writeTracks(int v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", clamp_tracks(v));
        SetWindowTextW(m_tracksEdit, buf);
    }
    void writeSpeed(int v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", clamp_speed(v));
        SetWindowTextW(m_speedEdit, buf);
    }
    void writeTurntableSpeed(int v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", clamp_turntable_speed(v));
        SetWindowTextW(m_turntableSpeedEdit, buf);
    }
    void writeBkAspectMin(int v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", clamp_bk_aspect(v));
        SetWindowTextW(m_bkAspectMinEdit, buf);
    }
    void writeBkAspectMax(int v) {
        wchar_t buf[32];
        swprintf_s(buf, L"%d", clamp_bk_aspect(v));
        SetWindowTextW(m_bkAspectMaxEdit, buf);
    }

    bool hasChanged() const {
        if (readInt(m_intervalEdit, kDefaultSpawnIntervalMs, clamp_interval) != danmaku_get_spawn_interval_ms() ||
            readInt(m_tracksEdit, kDefaultTrackCount, clamp_tracks)           != danmaku_get_track_count()        ||
            readInt(m_speedEdit, kDefaultSpeedPercent, clamp_speed)           != danmaku_get_speed_percent()      ||
            readInt(m_turntableSpeedEdit, kDefaultTurntableSpeedPercent, clamp_turntable_speed)
                != danmaku_get_turntable_speed_percent() ||
            readInt(m_bkAspectMinEdit, kDefaultBkAspectMinTenths, clamp_bk_aspect)
                != danmaku_get_bk_aspect_min_tenths() ||
            readInt(m_bkAspectMaxEdit, kDefaultBkAspectMaxTenths, clamp_bk_aspect)
                != danmaku_get_bk_aspect_max_tenths())
            return true;
        {
            wchar_t keyBuf[512] = {};
            GetWindowTextW(m_apiKeyEdit, keyBuf, 512);
            pfc::stringcvt::string_utf8_from_wide utf8_key(keyBuf);
            std::string saved_key = danmaku_get_deepseek_api_key();
            if (strcmp(utf8_key.get_ptr(), saved_key.c_str()) != 0) return true;
        }
        /* Check whether the provider order in the ListView differs from g_music. */
        if (!g_music || !m_providerList) return false;
        int n = music_client_get_provider_count(g_music);
        if (ListView_GetItemCount(m_providerList) != n) return true;
        for (int i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH] = {};
            LVITEMW li = {};
            li.mask = LVIF_TEXT; li.iItem = i; li.pszText = buf; li.cchTextMax = MAX_PATH;
            ListView_GetItem(m_providerList, &li);
            const wchar_t* cur = music_client_get_provider_path(g_music, i);
            if (!cur) return true;
            const wchar_t* fn = wcsrchr(cur, L'\\');
            if (fn) fn++; else fn = cur;
            if (_wcsicmp(buf, fn) != 0) return true;
        }
        return false;
    }

    /* ── provider list helpers ── */

    void populateProviderList() {
        if (!m_providerList) return;
        ListView_DeleteAllItems(m_providerList);
        if (!g_music) return;
        int n = music_client_get_provider_count(g_music);
        for (int i = 0; i < n; i++) {
            const wchar_t* path = music_client_get_provider_path(g_music, i);
            if (!path) continue;
            const wchar_t* fn = wcsrchr(path, L'\\');
            const wchar_t* name = fn ? fn + 1 : path;
            LVITEMW li = {};
            li.mask    = LVIF_TEXT;
            li.iItem   = i;
            li.pszText = const_cast<wchar_t*>(name);
            ListView_InsertItem(m_providerList, &li);
        }
    }

    void moveListItem(int src, int dst) {
        if (!m_providerList || src == dst) return;
        wchar_t buf[MAX_PATH] = {};
        LVITEMW li = {};
        li.mask = LVIF_TEXT; li.iItem = src; li.pszText = buf; li.cchTextMax = MAX_PATH;
        ListView_GetItem(m_providerList, &li);
        std::wstring text = buf;
        ListView_DeleteItem(m_providerList, src);
        LVITEMW ins = {};
        ins.mask    = LVIF_TEXT;
        ins.iItem   = dst;
        ins.pszText = const_cast<wchar_t*>(text.c_str());
        ListView_InsertItem(m_providerList, &ins);
        ListView_SetItemState(m_providerList, dst,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    void applyProviderOrder() {
        if (!g_music || !m_providerList) return;
        int n     = ListView_GetItemCount(m_providerList);
        int total = music_client_get_provider_count(g_music);
        if (n != total) return;
        std::vector<std::wstring> ordered;
        for (int i = 0; i < n; i++) {
            wchar_t buf[MAX_PATH] = {};
            LVITEMW li = {};
            li.mask = LVIF_TEXT; li.iItem = i; li.pszText = buf; li.cchTextMax = MAX_PATH;
            ListView_GetItem(m_providerList, &li);
            for (int j = 0; j < total; j++) {
                const wchar_t* path = music_client_get_provider_path(g_music, j);
                if (!path) continue;
                const wchar_t* fn = wcsrchr(path, L'\\');
                const wchar_t* name = fn ? fn + 1 : path;
                if (_wcsicmp(buf, name) == 0) { ordered.push_back(path); break; }
            }
        }
        save_provider_order(ordered);
    }
};

class DanmakuPreferencesPage : public preferences_page_v3 {
public:
    const char* get_name() override { return "FooBar Danmaku"; }
    GUID get_guid() override { return guid_pref_page; }
    GUID get_parent_guid() override { return preferences_page::guid_tools; }
    preferences_page_instance::ptr instantiate(fb2k::hwnd_t parent, preferences_page_callback::ptr callback) override {
        return new service_impl_t<DanmakuPreferencesInstance>((HWND)parent, callback);
    }
};

static preferences_page_factory_t<DanmakuPreferencesPage> g_danmaku_preferences_page_factory;


