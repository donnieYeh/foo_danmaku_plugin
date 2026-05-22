// danmaku_ui.cpp - UI element implementation for foobar2000 danmaku plugin
#include "ui/danmaku_ui.h"
#include "../core/danmaku_engine.h"
#include "../core/playback_monitor.h"
#include "netease_client.h"   // standalone NetEase comment library
#include <windows.h>
#include <windowsx.h>
#include <string>
#include <thread>
#include <atomic>

// ── logging helpers ──────────────────────────────────────
// Writes to foobar2000's View→Console AND OutputDebugString.
static void danmaku_log(const char* msg) {
    console::print(msg);
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}
static void danmaku_logW(const wchar_t* wmsg) {
    // Convert to UTF-8 for fb2k console
    int n = WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        std::string s(n - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wmsg, -1, &s[0], n, nullptr, nullptr);
        console::print(s.c_str());
    }
    OutputDebugStringW(wmsg);
    OutputDebugStringW(L"\n");
}

const wchar_t* DanmakuUIWindow::kClassName = L"FooBarDanmakuWindow";

// {A1B2C3D4-E5F6-7890-ABCD-EF1234567891}
const GUID g_danmaku_guid = { 0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x91} };

static DanmakuEngine*  g_engine  = nullptr;
static PlaybackMonitor* g_monitor = nullptr;
static NeteaseHandle    g_netease = nullptr;
static std::atomic<bool> g_fetching{false};

/* Per-comment callback: adds each comment to the engine */
static std::atomic<int> g_comment_count{0};
static int __stdcall comment_cb(
    const wchar_t* content, const wchar_t* nickname,
    int like_count, void* userdata)
{
    auto* eng = static_cast<DanmakuEngine*>(userdata);
    COLORREF color = RGB(255,255,255);
    if      (like_count > 1000) color = RGB(255,100,100);
    else if (like_count > 100)  color = RGB(255,200,100);
    else if (like_count > 10)   color = RGB(100,200,255);
    eng->addDanmaku(content, color);
    int n = ++g_comment_count;
    // Log first comment as sample
    if (n == 1) {
        std::wstring sample = L"[Danmaku] first comment: ";
        sample += content ? content : L"(null)";
        sample += L"  (likes=";
        sample += std::to_wstring(like_count);
        sample += L")";
        danmaku_logW(sample.c_str());
    }
    return 0; // continue
}

static void onNewTrackCallback(const wchar_t* title, const wchar_t* artist, void* userdata);

DanmakuUI::DanmakuUI() {}

const GUID& DanmakuUI::g_get_guid() {
    return g_danmaku_guid;
}

void DanmakuUI::get_name(pfc::string_base& p_out) {
    p_out = "FooBar Danmaku";
}

ui_element_config::ptr DanmakuUI::get_default_configuration() {
    return ui_element_config::g_create_empty(g_danmaku_guid);
}

ui_element_instance_ptr DanmakuUI::instantiate(fb2k::hwnd_t p_parent, ui_element_config::ptr cfg, ui_element_instance_callback_ptr p_callback) {
    return new service_impl_t<DanmakuUIInstance>(p_parent, p_callback);
}

DanmakuUIInstance::DanmakuUIInstance(HWND parent, ui_element_instance_callback_ptr callback)
    : m_callback(callback), m_wnd(new DanmakuUIWindow(parent)) {

    HWND panelHwnd = m_wnd->getHwnd();

    if (!g_engine) {
        g_engine = new DanmakuEngine();
        g_engine->init(panelHwnd);
    } else {
        // 旧实例的 HWND 已销毁，必须更新；否则 resize() 里 GetDC(旧HWND) 崩溃
        g_engine->setHwnd(panelHwnd);
    }
    m_wnd->setEngine(g_engine);

    if (!g_monitor) {
        g_monitor = new PlaybackMonitor();
        g_monitor->init();
    }
    g_monitor->setOnNewTrack(onNewTrackCallback, m_wnd);

    if (!g_netease) {
        g_netease = netease_create(nullptr); // anonymous access
        // Route all netease_client logs → foobar2000 View→Console
        netease_set_global_log([](const wchar_t* msg, void*) {
            danmaku_logW(msg);
        }, nullptr);
        danmaku_log("[Danmaku] netease_client handle created, log callback registered");
    }
}

DanmakuUIInstance::~DanmakuUIInstance() {
    // 先清回调，防止异步线程在销毁后访问已销毁的窗口
    if (g_monitor) {
        g_monitor->setOnNewTrack(nullptr, nullptr);
    }

    // 等待正在进行的异步请求完成（最多等 200ms）
    for (int i = 0; i < 20 && g_fetching.load(); ++i) {
        Sleep(10);
    }
    g_fetching = false;

    // 先关子窗口（不就是 engine 的 HWND）
    delete m_wnd;
    m_wnd = nullptr;

    // 清理 engine（HWND 已销毁，立即清除）
    delete g_engine;
    g_engine = nullptr;

    if (g_netease) { netease_destroy(g_netease); g_netease = nullptr; }
}

static void onNewTrackCallback(const wchar_t* title, const wchar_t* artist, void* userdata) {
    if (g_fetching.exchange(true)) return;

    DanmakuUIWindow* wnd = reinterpret_cast<DanmakuUIWindow*>(userdata);
    if (!wnd || !g_netease || !g_engine) {
        danmaku_log("[Danmaku] onNewTrack: guard failed (wnd/netease/engine null)");
        g_fetching = false;
        return;
    }

    std::wstring titleStr (title  ? title  : L"");
    std::wstring artistStr(artist ? artist : L"");
    if (titleStr.empty()) {
        danmaku_log("[Danmaku] onNewTrack: empty title, skipping");
        g_fetching = false;
        return;
    }

    // Log that we received a new track
    {
        std::wstring info = L"[Danmaku] New track → title=\"" + titleStr
                          + L"\" artist=\"" + artistStr + L"\"";
        danmaku_logW(info.c_str());
    }

    HWND targetHwnd = wnd->getHwnd();

    std::thread([targetHwnd, titleStr, artistStr]() {
        danmaku_log("[Danmaku] Fetching comments (background thread started)");
        g_comment_count = 0;
        g_engine->clearDanmaku();

        int rc = netease_get_comments_by_keyword(
            g_netease,
            titleStr.c_str(),
            artistStr.empty() ? nullptr : artistStr.c_str(),
            50,
            comment_cb,
            g_engine);

        if (rc != NETEASE_OK) {
            std::wstring err = L"[Danmaku] Comment fetch FAILED rc="
                             + std::to_wstring(rc)
                             + L" err=" + netease_last_error(g_netease);
            danmaku_logW(err.c_str());
        } else {
            std::wstring ok = L"[Danmaku] Comments loaded: "
                            + std::to_wstring(g_comment_count.load())
                            + L" items";
            danmaku_logW(ok.c_str());
        }

        if (IsWindow(targetHwnd))
            InvalidateRect(targetHwnd, nullptr, FALSE);

        g_fetching = false;
    }).detach();
}

DanmakuUIWindow::DanmakuUIWindow(HWND parent)
    : m_parent(parent), m_visible(true), m_hwnd(nullptr), m_engine(nullptr) {

    // 使用 DLL 自身的 HINSTANCE，不用主程序的
    HINSTANCE hInst = (HINSTANCE)GetModuleHandleW(L"foo_danmaku.dll");
    if (!hInst) hInst = (HINSTANCE)GetModuleHandleW(nullptr);

    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEXW);
    wcex.lpfnWndProc   = WindowProc;
    wcex.hInstance     = hInst;
    wcex.lpszClassName = kClassName;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    if (!RegisterClassExW(&wcex) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }

    // 作为 foobar2000 布局框架的子窗口嵌入，不再是浮动弹出窗
    // 位置和大小由 foobar2000 框架管理，初始传 0
    m_hwnd = CreateWindowExW(
        0,
        kClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        parent,
        nullptr,
        hInst,
        this
    );

    if (m_hwnd) {
        RegisterHotKey(m_hwnd, 1, MOD_CONTROL | MOD_SHIFT, 'D');
    }
}

DanmakuUIWindow::~DanmakuUIWindow() {
    if (m_hwnd) {
        UnregisterHotKey(m_hwnd, 1);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void DanmakuUIWindow::setEngine(DanmakuEngine* engine) {
    m_engine = engine;
    if (m_engine && m_hwnd) {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        int w = rc.right  - rc.left;
        int h = rc.bottom - rc.top;
        if (w > 0 && h > 0) m_engine->resize(w, h);
    }
}

void DanmakuUIWindow::setVisible(bool visible) {
    m_visible = visible;
    if (m_hwnd) {
        ShowWindow(m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
}

void DanmakuUIWindow::onCreate() {
    startTimer();
}

void DanmakuUIWindow::onDestroy() {
    stopTimer();
}

void DanmakuUIWindow::onShow() {
    startTimer();
}

void DanmakuUIWindow::onHide() {
    stopTimer();
}

void DanmakuUIWindow::onSize(int w, int h) {
    if (m_engine && w > 0 && h > 0) {
        m_engine->resize(w, h);
    }
}

void DanmakuUIWindow::startTimer() {
    if (m_hwnd) {
        SetTimer(m_hwnd, 1, 16, nullptr);
    }
}

void DanmakuUIWindow::stopTimer() {
    if (m_hwnd) {
        KillTimer(m_hwnd, 1);
    }
}

void DanmakuUIWindow::onPaint() {
    if (!m_hwnd) return;

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT rect;
    GetClientRect(m_hwnd, &rect);

    HBRUSH hbr = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rect, hbr);
    DeleteObject(hbr);

    if (m_engine) {
        m_engine->onPaint(hdc);
    }

    EndPaint(m_hwnd, &ps);
}

void DanmakuUIWindow::onTimer() {
    if (!m_hwnd) return;

    if (m_engine) {
        m_engine->onTimer();
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

LRESULT CALLBACK DanmakuUIWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DanmakuUIWindow* wnd = reinterpret_cast<DanmakuUIWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        if (cs && cs->lpCreateParams) {
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_CREATE:
        if (wnd) wnd->onCreate();
        break;
    case WM_DESTROY:
        if (wnd) wnd->onDestroy();
        break;
    case WM_SHOWWINDOW:
        if (wnd) {
            if (wParam) wnd->onShow();
            else wnd->onHide();
        }
        break;
    case WM_PAINT:
        if (wnd) wnd->onPaint();
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wParam == 1 && wnd) wnd->onTimer();
        break;
    case WM_SIZE: {
        if (wnd) {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            wnd->onSize(w, h);
        }
        break;
    }
    case WM_HOTKEY:
        if (wParam == 1 && wnd) {
            wnd->setVisible(!wnd->m_visible);
        }
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}
