// danmaku_ui.cpp - UI element implementation for foobar2000 danmaku plugin
#include "ui/danmaku_ui.h"
#include "ui/danmaku_preferences.h"
#include "../core/danmaku_engine.h"
#include "../core/playback_monitor.h"
#include "music_client.h"         // Layer-2 music data client (static lib)
#include "core/provider_manager.h"  // g_music global (owned by initquit)
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
#include <vector>
#include <memory>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
/* urlmon.lib is pulled in transitively by music_client.lib via #pragma comment */

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
// g_music is defined in provider_manager.cpp and initialised by initquit.
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
            Gdiplus::Bitmap* source = &bmp;
            std::unique_ptr<Gdiplus::Bitmap> croppedRightHalf;

            UINT srcW = bmp.GetWidth();
            UINT srcH = bmp.GetHeight();

            // Some files embed booklet scans as a two-page spread. When the
            // cover is close to 2:1, treat it as left+right pages and keep the
            // right page only. This happens before the 1000px downsample and is
            // fully in-memory.
            if (srcW > 0 && srcH > 0) {
                float aspect = (float)srcW / (float)srcH;
                if (aspect >= danmaku_get_bk_aspect_min() && aspect <= danmaku_get_bk_aspect_max()) {
                    UINT cropX = srcW / 2;
                    UINT cropW = srcW - cropX;
                    croppedRightHalf.reset(new Gdiplus::Bitmap(cropW, srcH, PixelFormat32bppARGB));
                    Gdiplus::Graphics cg(croppedRightHalf.get());
                    cg.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                    cg.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    cg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                    cg.DrawImage(&bmp,
                        Gdiplus::Rect(0, 0, (INT)cropW, (INT)srcH),
                        cropX, 0, cropW, srcH,
                        Gdiplus::UnitPixel);
                    source = croppedRightHalf.get();
                    srcW = cropW;
                    // srcH unchanged
                    danmaku_log("[Danmaku] cover art looks like 2-page BK spread; using right half");
                }
            }

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
                g.DrawImage(source, Gdiplus::Rect(0, 0, (INT)dstW, (INT)dstH),
                            0, 0, srcW, srcH, Gdiplus::UnitPixel);
                resized.GetHBITMAP(Gdiplus::Color(0, 0, 0), &out);
            } else {
                source->GetHBITMAP(Gdiplus::Color(0, 0, 0), &out);
            }
        }
    }
    stream->Release();
    return out;
}

static void fetchAndApplyCoverArtAsync(
    metadb_handle_ptr   track,
    uint64_t            gen,
    MusicTrackSessionHandle session = nullptr,
    HWND                invalidateHwnd  = nullptr,
    std::shared_ptr<std::atomic<bool>> done = nullptr);

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

static void onNewTrackCallback(const wchar_t* title, const wchar_t* artist, const wchar_t* album, void* userdata);
static void onPlayStateCallback(bool playing, void* userdata);

static void fetchAndApplyCoverArtAsync(
    metadb_handle_ptr track,
    uint64_t          gen,
    MusicTrackSessionHandle session,
    HWND              invalidateHwnd,
    std::shared_ptr<std::atomic<bool>> done)
{
    if (track.is_empty() || !g_engine) {
        if (done) *done = true;
        return;
    }
    std::thread([track, gen, session, invalidateHwnd, done]() {
        auto cancelled = [gen]() {
            return g_trackGen.load() != gen;
        };
        auto apply_bitmap = [gen, invalidateHwnd, cancelled](HBITMAP bmp, const char* sourceLog) {
            if (!bmp) return false;
            if (cancelled()) {
                DeleteObject(bmp);
                return false;
            }
            if (g_engine && !g_engine->hasCoverArt()) {
                g_engine->setCoverArt(bmp); // engine takes ownership
                danmaku_log(sourceLog);
                if (invalidateHwnd && IsWindow(invalidateHwnd))
                    InvalidateRect(invalidateHwnd, nullptr, FALSE);
                return true;
            }
            DeleteObject(bmp);
            return false;
        };

        try {
            bool localCoverApplied = false;
            auto api = album_art_manager_v2::get();
            metadb_handle_list list;
            list.add_item(track);
            pfc::list_t<GUID> ids;
            ids.add_item(album_art_ids::cover_front);
            abort_callback_dummy abort;
            auto extractor = api->open(list, ids, abort);
            if (!extractor.is_empty()) {
                album_art_data_ptr blob;
                try {
                    blob = extractor->query(album_art_ids::cover_front, abort);
                } catch (...) {
                    /* No embedded/local cover; provider fallback below. */
                }
                if (!blob.is_empty()) {
                    HBITMAP bmp = decodeCoverBytes(blob->get_ptr(), blob->get_size());
                    if (!bmp) {
                        danmaku_log("[Danmaku] cover art decode failed");
                    } else {
                        localCoverApplied = apply_bitmap(
                            bmp,
                            "[Danmaku] local cover art applied to vinyl label");
                    }
                }
            }

            if (localCoverApplied || cancelled() || !session ||
                (g_engine && g_engine->hasCoverArt())) {
                if (done) *done = true;
                return;
            }

            /* Lazy provider fallback via the current track session. This reuses
             * the same per-provider search metadata cache as comments. */
            void* providerCover = nullptr;
            int   providerSize  = 0;
            int rc = music_client_track_fetch_cover(
                session,
                &providerCover,
                &providerSize);
            if (rc != MUSIC_OK || !providerCover || providerSize <= 0) {
                if (rc != MUSIC_OK) {
                    std::wstring err = L"[Danmaku] provider cover fetch skipped/failed rc="
                                     + std::to_wstring(rc);
                    danmaku_logW(err.c_str());
                }
                music_client_free(providerCover);
                if (done) *done = true;
                return;
            }

            HBITMAP bmp = decodeCoverBytes(providerCover, (size_t)providerSize);
            music_client_free(providerCover);
            if (!bmp) {
                danmaku_log("[Danmaku] provider cover decode failed");
                if (done) *done = true;
                return;
            }
            apply_bitmap(bmp, "[Danmaku] provider cover applied to vinyl label");
        } catch (...) {
            danmaku_log("[Danmaku] cover art fetch threw");
        }
        if (done) *done = true;
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

    // g_music is created and providers are loaded by DanmakuProviderInit::on_init().
    // Nothing to do here.

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

    // 立即触发取消：令所有后台 worker 的 cancelled() 返回 true，
    // 使其尽快退出循环，不再访问 g_engine / g_music。
    // 注意：此处必须在等待之前执行，否则 worker 的 Sleep(200ms)
    // 恰好与旧的 200ms 等待上限相等，必然造成 use-after-free。
    ++g_trackGen;

    // 等待正在进行的异步请求完成（最多等 2000ms）
    for (int i = 0; i < 200 && g_fetching.load(); ++i) {
        Sleep(10);
    }
    g_fetching = false;

    // 先关子窗口（不就是 engine 的 HWND）
    delete m_wnd;
    m_wnd = nullptr;

    // 清理 engine（HWND 已销毁，立即清除）
    delete g_engine;
    g_engine = nullptr;

    // 注意：g_music 的生命周期由 DanmakuProviderInit::on_quit() 统一管理，
    // 此处不再重复销毁——若在 worker 仍持有 session 时销毁 g_music
    // 会导致 music_client_last_error(g_music) 等调用访问悬空指针。

    ensureGdiplusShutdown();
}

static void onNewTrackCallback(const wchar_t* title, const wchar_t* artist, const wchar_t* album, void* userdata) {
    DanmakuUIWindow* wnd = reinterpret_cast<DanmakuUIWindow*>(userdata);
    if (!wnd || !g_music || !g_engine) {
        danmaku_log("[Danmaku] onNewTrack: guard failed (wnd/music/engine null)");
        g_fetching = false;
        return;
    }

    std::wstring titleStr (title  ? title  : L"");
    std::wstring artistStr(artist ? artist : L"");
    std::wstring albumStr (album  ? album  : L"");
    if (titleStr.empty()) {
        danmaku_log("[Danmaku] onNewTrack: empty title, skipping");
        g_fetching = false;
        return;
    }

    // Log that we received a new track
    {
        std::wstring info = L"[Danmaku] New track → title=\"" + titleStr
                          + L"\" artist=\"" + artistStr
                          + L"\" album=\"" + albumStr + L"\"";
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
    metadb_handle_ptr currentTrack;
    if (g_monitor) currentTrack = g_monitor->getCurrentTrack();

    std::thread([targetHwnd, titleStr, artistStr, albumStr, currentTrack, gen]() {
        const int  kInitialBurst   = 100;   // first batch — enough to start playing
        const int  kPageSize       = 50;    // incremental page size
        const int  kLowWaterMark   = 30;    // when poolRemaining ≤ this, prefetch
        const int  kMaxPages       = 100;   // hard cap = up to ~5000 comments
        const int  kSleepTickMs    = 200;

        auto cancelled = [gen]() {
            return g_trackGen.load() != gen;
        };

        danmaku_log("[Danmaku] streaming worker: opening track session\u2026");

        // ── Step 1: open a Layer-2 track session from raw metadata only ──
        MusicTrackQuery query = {};
        query.title = titleStr.c_str();
        query.artist = artistStr.empty() ? nullptr : artistStr.c_str();
        query.album = albumStr.empty() ? nullptr : albumStr.c_str();
        query.duration_ms = 0;

        MusicTrackSessionHandle session = nullptr;
        int rc = music_client_open_track_session(g_music, &query, &session);
        if (rc != MUSIC_OK) {
            std::wstring err = L"[Danmaku] open_track_session FAILED rc="
                             + std::to_wstring(rc)
                             + L" err=" + music_client_last_error(g_music);
            danmaku_logW(err.c_str());
            if (!cancelled()) g_fetching = false;
            return;
        }
        if (cancelled()) {
            music_client_close_track_session(session);
            return;
        }

        auto coverDone = std::make_shared<std::atomic<bool>>(true);
        if (!currentTrack.is_empty()) {
            *coverDone = false;
            fetchAndApplyCoverArtAsync(
                currentTrack, gen, session, targetHwnd, coverDone);
        }

        // ── Step 2: fetch initial burst (one page of 100) so playback can start ──
        int delivered = 0;
        rc = music_client_track_next_comments(
            session, kInitialBurst,
            comment_cb, g_engine, &delivered);
        if (rc != MUSIC_OK) {
            std::wstring err = L"[Danmaku] initial page FAILED rc="
                             + std::to_wstring(rc)
                             + L" err=" + music_client_last_error(g_music);
            danmaku_logW(err.c_str());
            for (int i = 0; i < 50 && !coverDone->load(); ++i) Sleep(10);
            music_client_close_track_session(session);
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

        // Stop only when the server delivers 0 — providers may cap per-page
        // items below kInitialBurst (e.g. QQ Music caps anonymous pages at 10).
        bool serverExhausted = (delivered == 0);
        int  pagesFetched    = 1;

        // ── Step 3: trickle more pages as the drip index approaches the end ──
        while (!serverExhausted && !cancelled() && pagesFetched < kMaxPages) {
            // Wait until the engine's pool is running low.
            while (!cancelled()) {
                if (!g_engine) break; // engine 已被销毁，立即退出
                int remaining = g_engine->poolRemaining();
                int total     = g_engine->poolSize();
                // remaining counts unread items in CURRENT cycle; once it wraps
                // (remaining == total) we've started replaying — also a signal
                // to prefetch ASAP.
                if (remaining <= kLowWaterMark || remaining == total) break;
                // 分段短睡眠（每次 10ms）以便 cancelled() 能被及时响应，
                // 避免一次 Sleep(200ms) 耗尽析构函数的全部等待预算。
                for (int s = 0; s < kSleepTickMs / 10 && !cancelled(); ++s)
                    Sleep(10);
            }
            if (cancelled()) break;

            delivered = 0;
            rc = music_client_track_next_comments(
                session, kPageSize,
                comment_cb, g_engine, &delivered);
            if (rc != MUSIC_OK) {
                std::wstring err = L"[Danmaku] page@"
                                 + std::to_wstring(pagesFetched + 1)
                                 + L" FAILED rc=" + std::to_wstring(rc)
                                 + L" err=" + music_client_last_error(g_music);
                danmaku_logW(err.c_str());
                // Don't kill the loop on transient errors — back off and retry once.
                // 同样使用可中断睡眠，避免析构时卡住 2 秒。
                for (int s = 0; s < 200 && !cancelled(); ++s)
                    Sleep(10);
                if (cancelled()) break;
                continue;
            }

            pagesFetched++;

            {
                std::wstring msg = L"[Danmaku] page#"
                                 + std::to_wstring(pagesFetched)
                                 + L" got=" + std::to_wstring(delivered)
                                 + L" poolSize=" + std::to_wstring(g_engine ? g_engine->poolSize() : 0);
                danmaku_logW(msg.c_str());
            }

            // Short page → server has no more comments. Pool will loop from here.
            if (delivered < kPageSize) {
                serverExhausted = true;
                danmaku_log("[Danmaku] streaming complete — pool will loop from now on");
            }
        }

        // 等待封面抓取线程完成后再关闭 session。
        // 旧代码只等 50×10ms = 500ms，但 provider 封面需要一次完整的 HTTP
        // 请求（实测可超过 1 秒），期间两个线程共享同一个 session 句柄。
        // 一旦 streaming worker 超时关闭 session，cover art 线程再调用
        // music_client_track_fetch_cover(session) 就会访问已释放内存 → 崩溃。
        // 改为最多等 30 秒（300×100ms），同时响应取消信号以免阻塞析构。
        for (int i = 0; i < 300 && !coverDone->load() && !cancelled(); ++i)
            Sleep(100);
        music_client_close_track_session(session);
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

