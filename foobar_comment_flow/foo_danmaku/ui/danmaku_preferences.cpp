#include "ui/danmaku_preferences.h"
#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/SDK/cfg_var.h>
#include <windows.h>
#include <algorithm>

// {7C6DB3BB-6C17-46E3-9C3F-10CE511E7F0D}
static constexpr GUID guid_cfg_spawn_interval =
{ 0x7c6db3bb, 0x6c17, 0x46e3, { 0x9c, 0x3f, 0x10, 0xce, 0x51, 0x1e, 0x7f, 0x0d } };

// {84C7676B-1109-4F9A-9F2E-162F27F1D9C2}
static constexpr GUID guid_cfg_track_count =
{ 0x84c7676b, 0x1109, 0x4f9a, { 0x9f, 0x2e, 0x16, 0x2f, 0x27, 0xf1, 0xd9, 0xc2 } };

// {80E1D472-B84E-4CC6-9813-28E0B957B04C}
static constexpr GUID guid_cfg_speed_percent =
{ 0x80e1d472, 0xb84e, 0x4cc6, { 0x98, 0x13, 0x28, 0xe0, 0xb9, 0x57, 0xb0, 0x4c } };

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

static cfg_int g_cfg_spawn_interval(guid_cfg_spawn_interval, kDefaultSpawnIntervalMs);
static cfg_int g_cfg_track_count(guid_cfg_track_count, kDefaultTrackCount);
static cfg_int g_cfg_speed_percent(guid_cfg_speed_percent, kDefaultSpeedPercent);

static int clamp_interval(int v) {
    return std::max(kMinSpawnIntervalMs, std::min(kMaxSpawnIntervalMs, v));
}
static int clamp_tracks(int v) {
    return std::max(kMinTrackCount, std::min(kMaxTrackCount, v));
}
static int clamp_speed(int v) {
    return std::max(kMinSpeedPercent, std::min(kMaxSpeedPercent, v));
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

class DanmakuPreferencesInstance : public preferences_page_instance {
public:
    DanmakuPreferencesInstance(HWND parent, preferences_page_callback::ptr cb)
        : m_parent(parent), m_callback(cb), m_wnd(nullptr),
          m_intervalEdit(nullptr), m_tracksEdit(nullptr), m_speedEdit(nullptr) {
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
        writeEdit(danmaku_get_spawn_interval_ms());
        writeTracks(danmaku_get_track_count());
        writeSpeed(danmaku_get_speed_percent());
        if (m_callback.is_valid()) m_callback->on_state_changed();
    }

    void reset() override {
        writeEdit(kDefaultSpawnIntervalMs);
        writeTracks(kDefaultTrackCount);
        writeSpeed(kDefaultSpeedPercent);
        if (m_callback.is_valid()) m_callback->on_state_changed();
    }

private:
    enum { IDC_INTERVAL = 1001, IDC_TRACKS = 1002, IDC_SPEED = 1003 };

    HWND m_parent;
    preferences_page_callback::ptr m_callback;
    HWND m_wnd;
    HWND m_intervalEdit;
    HWND m_tracksEdit;
    HWND m_speedEdit;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<DanmakuPreferencesInstance*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<DanmakuPreferencesInstance*>(cs->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (self && msg == WM_COMMAND &&
            (LOWORD(wp) == IDC_INTERVAL || LOWORD(wp) == IDC_TRACKS || LOWORD(wp) == IDC_SPEED) &&
            HIWORD(wp) == EN_CHANGE) {
            if (self->m_callback.is_valid()) self->m_callback->on_state_changed();
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
            L"Larger = sparser. Default: 5000 ms. Suggested range: 1000–8000 ms.",
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
            L"Fewer lanes = wider line spacing. Default: 4. Suggested range: 3–6.",
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
            L"Settings take effect immediately after Apply.",
            WS_CHILD | WS_VISIBLE,
            12, 206, 560, 20, m_wnd, nullptr, inst, nullptr);

        SendMessageW(m_intervalEdit, EM_SETLIMITTEXT, 5, 0);
        SendMessageW(m_tracksEdit, EM_SETLIMITTEXT, 2, 0);
        SendMessageW(m_speedEdit, EM_SETLIMITTEXT, 3, 0);
        writeEdit(danmaku_get_spawn_interval_ms());
        writeTracks(danmaku_get_track_count());
        writeSpeed(danmaku_get_speed_percent());
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

    bool hasChanged() const {
        return readInt(m_intervalEdit, kDefaultSpawnIntervalMs, clamp_interval) != danmaku_get_spawn_interval_ms() ||
               readInt(m_tracksEdit, kDefaultTrackCount, clamp_tracks) != danmaku_get_track_count() ||
               readInt(m_speedEdit, kDefaultSpeedPercent, clamp_speed) != danmaku_get_speed_percent();
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
