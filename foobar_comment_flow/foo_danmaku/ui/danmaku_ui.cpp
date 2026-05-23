// danmaku_ui.cpp - UI element implementation for foobar2000 danmaku plugin
#include "ui/danmaku_ui.h"
#include "ui/danmaku_preferences.h"
#include "../core/danmaku_engine.h"
#include "../core/playback_monitor.h"
#include "netease_client.h"   // standalone NetEase comment library
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

// ── logging helpers ──────────────────────────────────────
// Writes to foobar2000's View→Console AND OutputDebugString.
static void danmaku_log(const char* msg) {
    console::print(msg);
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}
// Exported for engine TU (declared extern there) so its diagnostic logs
// also land in foobar2000's View → Console, not only DebugView.
extern "C" void danmaku_log_external(const char* msg) {
    danmaku_log(msg);
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

// GDI+ token + ref count so multiple UI elements share one Startup/Shutdown.
static ULONG_PTR        g_gdiplusToken = 0;
static int              g_gdiplusRefs  = 0;

static void ensureGdiplusStartup() {
    if (g_gdiplusRefs++ == 0) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
    }
}
static void ensureGdiplusShutdown() {
    if (--g_gdiplusRefs <= 0 && g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
        g_gdiplusRefs  = 0;
    }
}

// Decode raw image bytes (jpg/png/etc.) into a 32-bit DIB HBITMAP using GDI+.
// Large embedded covers can make per-frame vinyl rotation expensive. If the
// source image exceeds 1000x1000, downsample it in memory before handing it to
// the engine. No temporary files are created.
// Caller owns the returned HBITMAP. Returns nullptr on failure.
static HBITMAP decodeCoverBytes(const void* data, size_t size) {
    if (!data || size == 0) return nullptr;
    IStream* stream = SHCreateMemStream(
        reinterpret_cast<const BYTE*>(data),
        (UINT)size);
    if (!stream) return nullptr;
    HBITMAP out = nullptr;
    {
        Gdiplus::Bitmap bmp(stream, FALSE);
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            const UINT srcW = bmp.GetWidth();
            const UINT srcH = bmp.GetHeight();
            const UINT kMaxCoverSide = 1000;
            if (srcW > kMaxCoverSide || srcH > kMaxCoverSide) {
                float scale = std::min((float)kMaxCoverSide / (float)srcW,
                                       (float)kMaxCoverSide / (float)srcH);
                UINT dstW = std::max<UINT>(1, (UINT)(srcW * scale + 0.5f));
                UINT dstH = std::max<UINT>(1, (UINT)(srcH * scale + 0.5f));
                Gdiplus::Bitmap resized(dstW, dstH, PixelFormat32bppARGB);
                Gdiplus::Graphics g(&resized);
                g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                g.DrawImage(&bmp, Gdiplus::Rect(0, 0, (INT)dstW, (INT)dstH),
                            0, 0, srcW, srcH, Gdiplus::UnitPixel);
                resized.GetHBITMAP(Gdiplus::Color(0, 0, 0), &out);
            } else {
                bmp.GetHBITMAP(Gdiplus::Color(0, 0, 0), &out);
            }
        }
    }
    stream->Release();
    return out;
}

static void fetchAndApplyCoverArtAsync(metadb_handle_ptr track, uint64_t gen);

// Each new track bumps the generation; in-flight worker threads from prior
// tracks detect the mismatch and exit immediately. Atomic so the worker
// thread can read it without locking.
static std::atomic<uint64_t> g_trackGen{0};

/* Per-comment callback: pushes each comment into the engine's pool.
   The engine then drips them onto the screen at its own pace and loops back
   to the first comment when the pool is exhausted. */
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
    if (content && *content) {
        eng->addToPool(content, color);
    }
    int n = ++g_comment_count;
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
static void onPlayStateCallback(bool playing, void* userdata);

static void fetchAndApplyCoverArtAsync(metadb_handle_ptr track, uint64_t gen) {
    if (track.is_empty() || !g_engine) return;
    std::thread([track, gen]() {
        try {
            auto api = album_art_manager_v2::get();
            metadb_handle_list list;
            list.add_item(track);
            pfc::list_t<GUID> ids;
            ids.add_item(album_art_ids::cover_front);
            abort_callback_dummy abort;
            auto extractor = api->open(list, ids, abort);
            if (extractor.is_empty()) return;
            album_art_data_ptr blob;
            try {
                blob = extractor->query(album_art_ids::cover_front, abort);
            } catch (...) {
                return;
            }
            if (blob.is_empty()) return;

            HBITMAP bmp = decodeCoverBytes(blob->get_ptr(), blob->get_size());
            if (!bmp) {
                danmaku_log("[Danmaku] cover art decode failed");
                return;
            }
            // If the track changed while we were fetching, drop this cover.
            if (g_trackGen.load() != gen) {
                DeleteObject(bmp);
                return;
            }
            if (g_engine) {
                g_engine->setCoverArt(bmp); // engine takes ownership
                danmaku_log("[Danmaku] cover art applied to vinyl label");
            } else {
                DeleteObject(bmp);
            }
        } catch (...) {
            danmaku_log("[Danmaku] cover art fetch threw");
        }
    }).detach();
}

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
    g_monitor->setOnPlayState(onPlayStateCallback, m_wnd);

    if (!g_netease) {
        g_netease = netease_create(nullptr); // anonymous access
        // Route all netease_client logs → foobar2000 View→Console
        netease_set_global_log([](const wchar_t* msg, void*) {
            danmaku_logW(msg);
        }, nullptr);
        danmaku_log("[Danmaku] netease_client handle created, log callback registered");
    }

    if (g_engine) {
        DanmakuConfig cfg = g_engine->getConfig();
        cfg.spawnIntervalMs = danmaku_get_spawn_interval_ms();
        cfg.maxTracks = danmaku_get_track_count();
        cfg.baseSpeed = danmaku_get_base_speed();
        cfg.turntableSpeed = danmaku_get_turntable_speed();
        g_engine->setConfig(cfg);
    }

    ensureGdiplusStartup();

    // If something is already playing when the UI is created, seed cover + arm.
    if (g_monitor) {
        metadb_handle_ptr cur = g_monitor->getCurrentTrack();
        if (!cur.is_empty()) {
            fetchAndApplyCoverArtAsync(cur, g_trackGen.load());
        }
    }
}

DanmakuUIInstance::~DanmakuUIInstance() {
    // 先清回调，防止异步线程在销毁后访问已销毁的窗口
    if (g_monitor) {
        g_monitor->setOnNewTrack(nullptr, nullptr);
        g_monitor->setOnPlayState(nullptr, nullptr);
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

    ensureGdiplusShutdown();
}

static void onNewTrackCallback(const wchar_t* title, const wchar_t* artist, void* userdata) {
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

    // Bump generation BEFORE clearing pool, so any still-running worker thread
    // from a previous track sees the mismatch and exits without polluting the
    // new pool. Streaming worker captures this gen and re-checks on every page.
    uint64_t gen = ++g_trackGen;
    if (g_fetching.exchange(true)) {
        danmaku_log("[Danmaku] New track arrived while previous fetch is active; cancelling stale worker");
    }
    g_engine->clearPool();
    g_comment_count = 0;

    // Drop the previous album art immediately so we don't show stale cover
    // while the new one is being fetched.
    g_engine->setCoverArt(nullptr);
    if (g_monitor) {
        fetchAndApplyCoverArtAsync(g_monitor->getCurrentTrack(), gen);
    }

    std::thread([targetHwnd, titleStr, artistStr, gen]() {
        const int  kInitialBurst   = 100;   // first batch — enough to start playing
        const int  kPageSize       = 50;    // incremental page size
        const int  kLowWaterMark   = 30;    // when poolRemaining ≤ this, prefetch
        const int  kMaxPages       = 100;   // hard cap = up to ~5000 comments
        const int  kSleepTickMs    = 200;

        auto cancelled = [gen]() {
            return g_trackGen.load() != gen;
        };

        danmaku_log("[Danmaku] streaming worker: resolving song id\u2026");

        // ── Step 1: resolve song id from "title artist" keyword ──────────
        std::wstring kw = titleStr;
        if (!artistStr.empty()) kw += L" " + artistStr;
        wchar_t song_id[64] = {0};
        int rc = netease_search_song(g_netease, kw.c_str(), song_id, 64);
        if (rc != NETEASE_OK) {
            std::wstring err = L"[Danmaku] search_song FAILED rc="
                             + std::to_wstring(rc)
                             + L" err=" + netease_last_error(g_netease);
            danmaku_logW(err.c_str());
            if (!cancelled()) g_fetching = false;
            return;
        }
        if (cancelled()) { return; }

        // ── Step 2: fetch initial burst (one page of 100) so playback can start ──
        int delivered = 0;
        rc = netease_get_comments_by_id_paged(
            g_netease, song_id, 0, kInitialBurst,
            comment_cb, g_engine, &delivered);
        if (rc != NETEASE_OK) {
            std::wstring err = L"[Danmaku] initial page FAILED rc="
                             + std::to_wstring(rc)
                             + L" err=" + netease_last_error(g_netease);
            danmaku_logW(err.c_str());
            if (!cancelled()) g_fetching = false;
            return;
        }

        {
            std::wstring msg = L"[Danmaku] initial burst: "
                             + std::to_wstring(delivered)
                             + L" comments — playback can start";
            danmaku_logW(msg.c_str());
        }

        if (IsWindow(targetHwnd)) InvalidateRect(targetHwnd, nullptr, FALSE);

        // Server returned fewer than asked — already at end of stream.
        bool serverExhausted = (delivered < kInitialBurst);
        int  nextOffset      = delivered;
        int  pagesFetched    = 1;

        // ── Step 3: trickle more pages as the drip index approaches the end ──
        while (!serverExhausted && !cancelled() && pagesFetched < kMaxPages) {
            // Wait until the engine's pool is running low.
            while (!cancelled()) {
                int remaining = g_engine->poolRemaining();
                int total     = g_engine->poolSize();
                // remaining counts unread items in CURRENT cycle; once it wraps
                // (remaining == total) we've started replaying — also a signal
                // to prefetch ASAP.
                if (remaining <= kLowWaterMark || remaining == total) break;
                Sleep(kSleepTickMs);
            }
            if (cancelled()) break;

            delivered = 0;
            rc = netease_get_comments_by_id_paged(
                g_netease, song_id, nextOffset, kPageSize,
                comment_cb, g_engine, &delivered);
            if (rc != NETEASE_OK) {
                std::wstring err = L"[Danmaku] page@"
                                 + std::to_wstring(nextOffset)
                                 + L" FAILED rc=" + std::to_wstring(rc)
                                 + L" err=" + netease_last_error(g_netease);
                danmaku_logW(err.c_str());
                // Don't kill the loop on transient errors — back off and retry once.
                Sleep(2000);
                if (cancelled()) break;
                continue;
            }

            pagesFetched++;
            nextOffset += delivered;

            {
                std::wstring msg = L"[Danmaku] page#"
                                 + std::to_wstring(pagesFetched)
                                 + L" offset=" + std::to_wstring(nextOffset - delivered)
                                 + L" got=" + std::to_wstring(delivered)
                                 + L" poolSize=" + std::to_wstring(g_engine->poolSize());
                danmaku_logW(msg.c_str());
            }

            // Short page → server has no more comments. Pool will loop from here.
            if (delivered < kPageSize) {
                serverExhausted = true;
                danmaku_log("[Danmaku] streaming complete — pool will loop from now on");
            }
        }

        if (!cancelled()) g_fetching = false;
    }).detach();
}

static void onPlayStateCallback(bool playing, void* userdata) {
    (void)userdata;
    if (!g_engine) return;
    g_engine->setPaused(!playing);
    g_engine->setArmLanded(playing); // arm onto disc while playing, lift on pause/stop
    danmaku_log(playing ? "[Danmaku] playback resumed -> danmaku resumed, arm landing"
                        : "[Danmaku] playback paused/stopped -> danmaku paused, arm lifting");
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
    {
        char msg[128];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[Danmaku] WM_SIZE w=%d h=%d engine=%p", w, h, (void*)m_engine);
        danmaku_log(msg);
    }
    if (m_engine && w > 0 && h > 0) {
        m_engine->resize(w, h);
    }
    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void DanmakuUIWindow::startTimer() {
    if (m_hwnd) {
        // WM_TIMER is otherwise often quantized to ~15.6ms or worse. Request
        // 1ms system timer precision and tick at ~120Hz; movement itself is
        // time-based, so this mainly improves visual sampling smoothness.
        timeBeginPeriod(1);
        SetTimer(m_hwnd, 1, 8, nullptr);
    }
}

void DanmakuUIWindow::stopTimer() {
    if (m_hwnd) {
        KillTimer(m_hwnd, 1);
        timeEndPeriod(1);
    }
}

void DanmakuUIWindow::onPaint() {
    if (!m_hwnd) return;

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    // No on-HDC FillRect: the engine's memDC is already cleared to black each
    // frame and BitBlt is the single, atomic on-screen update. Drawing black
    // on hdc first and then BitBlt'ing over it is the classic source of GDI
    // flicker on large panels.
    if (m_engine) {
        m_engine->onPaint(hdc);
    } else {
        // Engine not yet attached — paint the invalidated region black so we
        // don't show garbage.
        RECT rect;
        GetClientRect(m_hwnd, &rect);
        HBRUSH hbr = (HBRUSH)GetStockObject(BLACK_BRUSH);
        FillRect(hdc, &rect, hbr);
    }

    EndPaint(m_hwnd, &ps);
}

void DanmakuUIWindow::onTimer() {
    if (!m_hwnd) return;

    if (m_engine) {
        DanmakuConfig cfg = m_engine->getConfig();
        int prefInterval = danmaku_get_spawn_interval_ms();
        int prefTracks = danmaku_get_track_count();
        float prefSpeed = danmaku_get_base_speed();
        float prefTurntableSpeed = danmaku_get_turntable_speed();
        if (cfg.spawnIntervalMs != prefInterval ||
            cfg.maxTracks != prefTracks ||
            cfg.baseSpeed != prefSpeed ||
            cfg.turntableSpeed != prefTurntableSpeed) {
            cfg.spawnIntervalMs = prefInterval;
            cfg.maxTracks = prefTracks;
            cfg.baseSpeed = prefSpeed;
            cfg.turntableSpeed = prefTurntableSpeed;
            m_engine->setConfig(cfg);
        }
        m_engine->onTimer();
    }
    // Paint immediately instead of merely posting WM_PAINT; this reduces jitter
    // when foobar's UI message queue is busy.
    RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

LRESULT CALLBACK DanmakuUIWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DanmakuUIWindow* wnd = reinterpret_cast<DanmakuUIWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        if (cs && cs->lpCreateParams) {
            auto* self = reinterpret_cast<DanmakuUIWindow*>(cs->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            // CRITICAL: assign m_hwnd here, BEFORE WM_CREATE fires, so that
            // onCreate()->startTimer()->SetTimer(m_hwnd,...) actually receives
            // a valid HWND. Otherwise the ctor only assigns m_hwnd AFTER
            // CreateWindowExW returns, by which point WM_CREATE has already
            // run with m_hwnd==nullptr and the 60Hz timer is never armed.
            self->m_hwnd = hwnd;
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
    case WM_LBUTTONUP:
        if (wnd && wnd->m_engine) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (wnd->m_engine->isPointInCoverArea(x, y)) {
                wnd->m_engine->setEnabled(!wnd->m_engine->isEnabled());
                danmaku_log(wnd->m_engine->isEnabled()
                    ? "[Danmaku] cover clicked -> danmaku shown"
                    : "[Danmaku] cover clicked -> danmaku hidden");
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
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
