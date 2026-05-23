#include "danmaku_engine.h"
#include <windows.h>
#include <gdiplus.h>
#include <cmath>
#include <algorithm>
#include <utility>  // std::min / std::max
#include <cstdio>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

// UI translation unit provides this — routes to foobar console + OutputDebugString.
// Declared extern here so we don't need to pull in fb2k SDK headers from engine.
extern "C" void danmaku_log_external(const char* msg);

static COLORREF blendColor(COLORREF a, COLORREF b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    return RGB((int)(ar + (br - ar) * t),
               (int)(ag + (bg - ag) * t),
               (int)(ab + (bb - ab) * t));
}

static void engine_log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    danmaku_log_external(buf);
}

DanmakuEngine::DanmakuEngine()
    : m_hwnd(nullptr), m_memDC(nullptr), m_memBM(nullptr)
    , m_font(nullptr), m_coverBitmap(nullptr), m_coverBitmapW(0), m_coverBitmapH(0)
    , m_width(0), m_height(0), m_recordAngle(0.0f)
    , m_armLanded(false), m_armProgress(0.0f)
    , m_poolIndex(0)
    , m_lastSpawnTick(0), m_lastUpdateTick(0), m_subpixelRemainderMs(0.0)
    , m_paused(false) {
    m_config = DanmakuConfig();
}

DanmakuEngine::~DanmakuEngine() {
    shutdown();
}

void DanmakuEngine::init(HWND parentWnd) {
    m_hwnd = parentWnd;

    HDC hdc = GetDC(m_hwnd);
    if (!hdc) hdc = GetDC(nullptr); // fallback to screen DC
    m_memDC = CreateCompatibleDC(hdc);
    m_memBM = CreateCompatibleBitmap(hdc, 1920, 200);
    if (m_memDC && m_memBM)
        SelectObject(m_memDC, m_memBM);
    ReleaseDC(m_hwnd, hdc);

    m_width  = 1920;
    m_height = 200;
    m_lastUpdateTick = GetTickCount();

    if (m_config.maxTracks <= 0) m_config.maxTracks = 1;
    m_trackUsage.resize(m_config.maxTracks, 0);
    m_trackRightEdge.assign(m_config.maxTracks, -1e9f);

    // 创建字体一次，后续复用，不再每帧 CreateFont
    m_font = CreateFontW(40, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
}

void DanmakuEngine::shutdown() {
    // 先把字体从 DC 中 deselect，再删除
    if (m_memDC && m_font) {
        SelectObject(m_memDC, GetStockObject(SYSTEM_FONT));
    }
    if (m_font) {
        DeleteObject(m_font);
        m_font = nullptr;
    }
    if (m_memDC) {
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    if (m_memBM) {
        DeleteObject(m_memBM);
        m_memBM = nullptr;
    }
    if (m_coverBitmap) {
        DeleteObject(m_coverBitmap);
        m_coverBitmap = nullptr;
        m_coverBitmapW = 0;
        m_coverBitmapH = 0;
    }
}

void DanmakuEngine::setCoverArt(HBITMAP bitmap) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_coverBitmap == bitmap) return;
    if (m_coverBitmap) {
        DeleteObject(m_coverBitmap);
        m_coverBitmap = nullptr;
        m_coverBitmapW = 0;
        m_coverBitmapH = 0;
    }
    m_coverBitmap = bitmap;
    if (m_coverBitmap) {
        BITMAP bm = {};
        if (GetObject(m_coverBitmap, sizeof(bm), &bm)) {
            m_coverBitmapW = bm.bmWidth;
            m_coverBitmapH = bm.bmHeight;
        }
    }
}

void DanmakuEngine::setArmLanded(bool landed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_armLanded = landed;
}

bool DanmakuEngine::isArmLanded() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_armLanded;
}

bool DanmakuEngine::isPointInCoverArea(int x, int y) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_width <= 80 || m_height <= 80) return false;
    int panelMin = std::min(m_width, m_height);
    int recordR = (int)((float)panelMin * 0.36f);
    recordR = std::max(48, recordR);
    recordR = std::min(recordR, std::min((int)(m_width * 0.42f), (int)(m_height * 0.42f)));
    int cx = m_width / 2;
    int cy = (int)(m_height * 0.47f);
    if (m_height < 220) cy = m_height / 2;
    int labelR = std::max(28, (int)(recordR * 0.68f));
    int dx = x - cx;
    int dy = y - cy;
    return dx * dx + dy * dy <= labelR * labelR;
}

void DanmakuEngine::setConfig(const DanmakuConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    int oldTracks = m_config.maxTracks;
    m_config = config;
    if (m_config.maxTracks <= 0) m_config.maxTracks = 1;
    if (oldTracks != m_config.maxTracks ||
        (int)m_trackUsage.size() != m_config.maxTracks ||
        (int)m_trackRightEdge.size() != m_config.maxTracks) {
        m_trackUsage.resize(m_config.maxTracks, 0);
        m_trackRightEdge.assign(m_config.maxTracks, -1e9f);
    }
}

DanmakuConfig DanmakuEngine::getConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void DanmakuEngine::addDanmaku(const std::wstring& text, COLORREF color) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled) return;
    if ((int)m_danmakuList.size() >= m_config.maxDanmaku) return;

    int track = pickTrackForSpawn();
    if (track < 0) {
        // No track has clearance — try least-used as fallback (overlap acceptable).
        track = allocateTrack();
    } else {
        m_trackUsage[track]++; // pickTrackForSpawn doesn't bump usage itself
    }

    int laneH = (m_config.maxTracks > 0) ? (m_height / m_config.maxTracks) : m_height;
    if (laneH < 1) laneH = 1;
    int spawnW = (m_width > 0) ? m_width : 800;

    DanmakuItem item;
    item.text = text;
    item.color = color;
    item.x = (float)spawnW;
    item.y = (float)(track * laneH);
    item.speed = (m_config.baseSpeed > 0.f) ? m_config.baseSpeed : 120.0f;
    item.active = true;
    item.startTime = GetTickCount();
    item.track = track;

    item.width = measureTextWidth(item.text);
    m_trackRightEdge[track] = item.x + item.width;

    m_danmakuList.push_back(item);
}

void DanmakuEngine::clearDanmaku() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_danmakuList.clear();
    std::fill(m_trackUsage.begin(), m_trackUsage.end(), 0);
    std::fill(m_trackRightEdge.begin(), m_trackRightEdge.end(), -1e9f);
}

void DanmakuEngine::addToPool(const std::wstring& text, COLORREF color) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (text.empty()) return;
    m_pool.emplace_back(text, color);
}

void DanmakuEngine::clearPool() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pool.clear();
    m_poolIndex = 0;
    m_danmakuList.clear();
    std::fill(m_trackUsage.begin(), m_trackUsage.end(), 0);
    std::fill(m_trackRightEdge.begin(), m_trackRightEdge.end(), -1e9f);
    engine_log("[Danmaku/engine] pool cleared");
}

int DanmakuEngine::poolSize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_pool.size();
}

int DanmakuEngine::poolIndex() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return (int)m_poolIndex;
}

int DanmakuEngine::poolRemaining() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pool.empty()) return 0;
    return (int)(m_pool.size() - m_poolIndex);
}

void DanmakuEngine::onPaint(HDC hdc) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_memDC || !m_memBM) return;

    drawSoftBackground(m_memDC);
    drawTurntable(m_memDC);

    if (m_config.enabled && m_font) {
        HGDIOBJ oldFont = SelectObject(m_memDC, m_font);
        SetBkMode(m_memDC, TRANSPARENT);

        for (const auto& item : m_danmakuList) {
            if (!item.active) continue;
            SetTextColor(m_memDC, item.color);
            TextOutW(m_memDC, (int)item.x, (int)item.y,
                     item.text.c_str(), (int)item.text.length());
        }

        SelectObject(m_memDC, oldFont);
    }

    // Single atomic blit to screen — the only on-screen draw this frame.
    BitBlt(hdc, 0, 0, m_width, m_height, m_memDC, 0, 0, SRCCOPY);
}

void DanmakuEngine::onTimer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    DWORD now = GetTickCount();
    if (m_lastUpdateTick == 0) m_lastUpdateTick = now;
    DWORD deltaMs = now - m_lastUpdateTick;
    m_lastUpdateTick = now;
    // Clamp long UI stalls; otherwise one busy foobar frame would teleport items.
    if (deltaMs > 50) deltaMs = 50;
    // GetTickCount is integer ms; keep fractional remainder from target 60Hz-ish
    // pacing so movement is less quantized at high speeds.
    double deltaPreciseMs = (double)deltaMs + m_subpixelRemainderMs;
    m_subpixelRemainderMs = deltaPreciseMs - floor(deltaPreciseMs);
    float deltaSeconds = (float)(deltaPreciseMs / 1000.0);

    // Arm landing easing — runs in both paused and playing state so the arm
    // can smoothly lift away when playback stops.
    {
        float target = m_armLanded ? 1.0f : 0.0f;
        float diff = target - m_armProgress;
        // Exponential ease — visually smooth, framerate-independent.
        float k = 1.0f - expf(-deltaSeconds * 4.5f);
        m_armProgress += diff * k;
        if (fabsf(diff) < 0.001f) m_armProgress = target;
    }

    if (m_paused) {
        // While paused, keep the timestamp fresh so resume doesn't jump.
        m_lastUpdateTick = now;
        return;   // audio paused: freeze scroll AND suspend drip
    }

    if (m_config.enabled) {
        updateDanmakuPositions(deltaSeconds);
    }
    if (!m_paused) {
        m_recordAngle += deltaSeconds * std::max(0.0f, m_config.turntableSpeed);
        const float twoPi = 6.28318530718f;
        if (m_recordAngle > twoPi) m_recordAngle = fmodf(m_recordAngle, twoPi);
    }

    if (m_config.enabled) {
        // Decay tracked right-edges so they stay in sync with the items that own them.
        for (auto& edge : m_trackRightEdge) {
            if (edge > -1e8f) edge -= m_config.baseSpeed * deltaSeconds;
        }

    // Mark off-screen danmaku inactive.
        for (auto& item : m_danmakuList) {
            // Only remove after the final glyph has fully crossed the left edge.
            if (item.x + item.width <= 0.0f) item.active = false;
        }

    // Recycle tracks for items being dropped so trackUsage stays balanced.
        for (auto& item : m_danmakuList) {
            if (!item.active && item.track >= 0) {
                recycleTrack(item.track);
                item.track = -1;
            }
        }

        m_danmakuList.erase(
            std::remove_if(m_danmakuList.begin(), m_danmakuList.end(),
                [](const DanmakuItem& item) { return !item.active; }),
            m_danmakuList.end()
        );

        // Drip a new item from the pool if there's clearance on any track. Loops
        // back to index 0 when the pool is exhausted so the show continues until
        // clearPool() is called (new song / stop).
        dripFromPool();
    }
}

void DanmakuEngine::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.enabled = enabled;
}

bool DanmakuEngine::isEnabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.enabled;
}

void DanmakuEngine::setPaused(bool paused) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_paused == paused) return;
    m_paused = paused;
    // On resume, snap the spawn cooldown so we don't instantly fire a backlog
    // of items that "should" have spawned during the pause interval.
    if (!paused) {
        DWORD now = GetTickCount();
        m_lastSpawnTick = now;
        m_lastUpdateTick = now;
        m_subpixelRemainderMs = 0.0;
    }
    engine_log(paused ? "[Danmaku/engine] paused" : "[Danmaku/engine] resumed");
}

bool DanmakuEngine::isPaused() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_paused;
}

void DanmakuEngine::resize(int width, int height) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (width <= 0 || height <= 0) return;
    if (m_width == width && m_height == height) return;

    m_width  = width;
    m_height = height;

    // 先将字体从 DC 中 deselect，然后释放旧资源
    if (m_memDC && m_font)
        SelectObject(m_memDC, GetStockObject(SYSTEM_FONT));

    if (m_memBM) { DeleteObject(m_memBM); m_memBM = nullptr; }
    if (m_memDC) { DeleteDC(m_memDC);     m_memDC = nullptr; }

    HDC hdc = GetDC(m_hwnd);
    if (!hdc) hdc = GetDC(nullptr); // fallback：用屏幕 DC 也能创建兼容 DC
    m_memDC = CreateCompatibleDC(hdc);
    m_memBM = CreateCompatibleBitmap(hdc, width, height);
    ReleaseDC(m_hwnd, hdc);

    if (m_memDC && m_memBM) {
        SelectObject(m_memDC, m_memBM);
    } else {
        // 创建失败时清理，避免悬空指针
        if (m_memBM) { DeleteObject(m_memBM); m_memBM = nullptr; }
        if (m_memDC) { DeleteDC(m_memDC);     m_memDC = nullptr; }
        return;
    }

    // resize 后重建字体（字号跟随高度缩放，放大一倍）
    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
    int fontSize = std::max(28, std::min(64, height / 4));
    m_font = CreateFontW(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
}

void DanmakuEngine::updateDanmakuPositions(float deltaSeconds) {
    for (auto& item : m_danmakuList) {
        if (item.active) {
            item.x -= item.speed * deltaSeconds;
        }
    }
}

float DanmakuEngine::measureTextWidth(const std::wstring& text) const {
    if (text.empty()) return 0.0f;

    HDC dc = m_memDC;
    HDC tempDC = nullptr;
    if (!dc) {
        tempDC = GetDC(m_hwnd ? m_hwnd : nullptr);
        dc = tempDC;
    }

    if (!dc) {
        int fontPx = (m_height > 0) ? std::max(28, std::min(64, m_height / 4)) : 40;
        return (float)std::max(60, (int)((float)fontPx * 0.65f * (float)text.size()));
    }

    HGDIOBJ oldFont = nullptr;
    if (m_font) oldFont = SelectObject(dc, m_font);

    SIZE sz = {};
    BOOL ok = GetTextExtentPoint32W(dc, text.c_str(), (int)text.length(), &sz);

    if (oldFont) SelectObject(dc, oldFont);
    if (tempDC) ReleaseDC(m_hwnd ? m_hwnd : nullptr, tempDC);

    if (!ok || sz.cx <= 0) {
        int fontPx = (m_height > 0) ? std::max(28, std::min(64, m_height / 4)) : 40;
        return (float)std::max(60, (int)((float)fontPx * 0.65f * (float)text.size()));
    }
    return (float)sz.cx;
}

void DanmakuEngine::drawSoftBackground(HDC dc) {
    if (!dc || m_width <= 0 || m_height <= 0) return;

    // Base tone: clear vertical light-to-dark gradient. When cover art exists,
    // it is painted above this as a dim, desaturated, intentionally blurred
    // atmosphere layer so the vinyl/tonearm remain visually dominant.
    const COLORREF top = RGB(62, 64, 64);
    const COLORREF mid = RGB(45, 47, 47);
    const COLORREF bottom = RGB(28, 30, 30);

    for (int y = 0; y < m_height; ++y) {
        float t = (m_height > 1) ? (float)y / (float)(m_height - 1) : 0.0f;
        COLORREF c = (t < 0.55f)
            ? blendColor(top, mid, t / 0.55f)
            : blendColor(mid, bottom, (t - 0.55f) / 0.45f);
        HBRUSH br = CreateSolidBrush(c);
        RECT r = { 0, y, m_width, y + 1 };
        FillRect(dc, &r, br);
        DeleteObject(br);
    }

    if (m_coverBitmap && m_coverBitmapW > 0 && m_coverBitmapH > 0) {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        // Cheap but effective blur: draw the cover into a much smaller bitmap,
        // then stretch it back up with high-quality interpolation.
        int smallW = std::max(32, m_width / 12);
        int smallH = std::max(32, m_height / 12);
        Gdiplus::Bitmap blurred(smallW, smallH, PixelFormat32bppARGB);
        Gdiplus::Graphics bg(&blurred);
        bg.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        bg.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        bg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        bg.Clear(Gdiplus::Color(255, 36, 38, 40));

        Gdiplus::Bitmap cover(m_coverBitmap, nullptr);
        float scale = std::max((float)smallW / (float)m_coverBitmapW,
                               (float)smallH / (float)m_coverBitmapH);
        int drawW = std::max(1, (int)((float)m_coverBitmapW * scale + 0.5f));
        int drawH = std::max(1, (int)((float)m_coverBitmapH * scale + 0.5f));
        int drawX = (smallW - drawW) / 2;
        int drawY = (smallH - drawH) / 2;

        // Low saturation + slight darkening + partial opacity.
        Gdiplus::ColorMatrix cm = {{
            {0.105f, 0.105f, 0.105f, 0.0f, 0.0f},
            {0.205f, 0.205f, 0.205f, 0.0f, 0.0f},
            {0.045f, 0.045f, 0.045f, 0.0f, 0.0f},
            {0.0f,   0.0f,   0.0f,   0.36f, 0.0f},
            {0.0f,   0.0f,   0.0f,   0.0f,  1.0f}
        }};
        Gdiplus::ImageAttributes attr;
        attr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault,
                            Gdiplus::ColorAdjustTypeBitmap);

        bg.DrawImage(&cover,
            Gdiplus::Rect(drawX, drawY, drawW, drawH),
            0, 0, m_coverBitmapW, m_coverBitmapH,
            Gdiplus::UnitPixel,
            &attr);

        g.DrawImage(&blurred, Gdiplus::Rect(0, 0, m_width, m_height),
            0, 0, smallW, smallH, Gdiplus::UnitPixel);

        // A translucent charcoal veil binds the background back into the UI and
        // keeps foreground contrast high.
        Gdiplus::LinearGradientBrush veil(
            Gdiplus::Point(0, 0), Gdiplus::Point(0, m_height),
            Gdiplus::Color(118, 58, 61, 63),
            Gdiplus::Color(168, 20, 22, 24));
        g.FillRectangle(&veil, 0, 0, m_width, m_height);
    }
}

void DanmakuEngine::drawTurntable(HDC dc) {
    if (!dc || m_width <= 80 || m_height <= 80) return;

    int panelMin = std::min(m_width, m_height);
    // NetEase reference proportions: the black vinyl is the dominant element.
    // Diameter should be >= 2/3 of the smaller UI dimension, with a little
    // margin reserved for the tonearm pivot above/right.
    int recordR = (int)((float)panelMin * 0.36f); // diameter ~= 72%
    recordR = std::max(48, recordR);
    recordR = std::min(recordR, std::min((int)(m_width * 0.42f), (int)(m_height * 0.42f)));
    int cx = m_width / 2;
    int cy = (int)(m_height * 0.47f);
    if (m_height < 220) cy = m_height / 2;

    // ── Record shadow / seat ────────────────────────────────────────
    // No glow/halo: just a controlled shadow between the outer contour and vinyl.
    for (int k = 12; k >= 2; k -= 2) {
        COLORREF c = RGB(18 + k, 20 + k, 23 + k);
        HBRUSH br = CreateSolidBrush(c);
        HPEN pen = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ oldB = SelectObject(dc, br);
        HGDIOBJ oldP = SelectObject(dc, pen);
        Ellipse(dc, cx - recordR - k, cy - recordR + 4,
                    cx + recordR + k, cy + recordR + 8 + k);
        SelectObject(dc, oldB);
        SelectObject(dc, oldP);
        DeleteObject(br);
        DeleteObject(pen);
    }

    // ── Black vinyl disc ─────────────────────────────────────────────
    HBRUSH vinyl = CreateSolidBrush(RGB(16, 17, 17));
    HPEN rimPen = CreatePen(PS_SOLID, 2, RGB(4, 5, 7));
    HGDIOBJ oldB = SelectObject(dc, vinyl);
    HGDIOBJ oldP = SelectObject(dc, rimPen);
    Ellipse(dc, cx - recordR, cy - recordR, cx + recordR, cy + recordR);
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    DeleteObject(vinyl);
    DeleteObject(rimPen);

    // Concentric grooves.
    for (int r = recordR - 7; r > recordR / 2; r -= 5) {
        int shade = 24 + ((recordR - r) % 18);
        HPEN groove = CreatePen(PS_SOLID, 1, RGB(shade, shade, shade));
        HGDIOBJ old = SelectObject(dc, groove);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
        SelectObject(dc, old);
        DeleteObject(groove);
    }

    // ── Center label / album art ─────────────────────────────────────
    // Cover is intentionally large, matching the reference's prominent center
    // image while still leaving a substantial black vinyl ring.
    int labelR = std::max(28, (int)(recordR * 0.68f));
    if (m_coverBitmap && m_coverBitmapW > 0 && m_coverBitmapH > 0) {
        // Draw with GDI+ so the actual album art rotates with the vinyl.
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        Gdiplus::GraphicsPath clipPath;
        clipPath.AddEllipse((Gdiplus::REAL)(cx - labelR), (Gdiplus::REAL)(cy - labelR),
                            (Gdiplus::REAL)(labelR * 2), (Gdiplus::REAL)(labelR * 2));
        Gdiplus::Region clipRegion(&clipPath);
        g.SetClip(&clipRegion, Gdiplus::CombineModeReplace);

        Gdiplus::Bitmap cover(m_coverBitmap, nullptr);
        Gdiplus::GraphicsState state = g.Save();
        g.TranslateTransform((Gdiplus::REAL)cx, (Gdiplus::REAL)cy);
        g.RotateTransform((Gdiplus::REAL)(m_recordAngle * 57.2957795f));
        g.DrawImage(&cover,
            Gdiplus::Rect(-labelR, -labelR, labelR * 2, labelR * 2),
            0, 0, m_coverBitmapW, m_coverBitmapH,
            Gdiplus::UnitPixel);
        g.Restore(state);
        g.ResetClip();

        // Subtle rim around the cover.
        HPEN rim = CreatePen(PS_SOLID, 2, RGB(20, 18, 16));
        HGDIOBJ oldP2 = SelectObject(dc, rim);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - labelR, cy - labelR, cx + labelR, cy + labelR);
        SelectObject(dc, oldP2);
        DeleteObject(rim);
    } else {
        // Fallback: plain gray label when the track has no album art.
        HBRUSH labelBg = CreateSolidBrush(RGB(118, 122, 128));
        HPEN labelPen = CreatePen(PS_SOLID, 2, RGB(28, 30, 34));
        HGDIOBJ oB = SelectObject(dc, labelBg);
        HGDIOBJ oP = SelectObject(dc, labelPen);
        Ellipse(dc, cx - labelR, cy - labelR, cx + labelR, cy + labelR);
        SelectObject(dc, oB);
        SelectObject(dc, oP);
        DeleteObject(labelBg);
        DeleteObject(labelPen);
    }

    // Spindle hole.
    HBRUSH hole = CreateSolidBrush(RGB(6, 7, 8));
    HGDIOBJ oldH = SelectObject(dc, hole);
    Ellipse(dc, cx - 5, cy - 5, cx + 5, cy + 5);
    SelectObject(dc, oldH);
    DeleteObject(hole);

    // ── Tone arm — animated landing, polished detail ────────────────
    // Pivot anchored to the upper-right of the disc, similar to the
    // reference NetEase Cloud Music player. The arm sweeps in/out around
    // this pivot driven by m_armProgress (0=parked off-disc, 1=landed).
    // Pivot is deliberately outside the vinyl, slightly farther right/up than
    // before so the base no longer visually intrudes into the record.
    int pivotX = cx + (int)(recordR * 1.12f);
    int pivotY = cy - (int)(recordR * 0.98f);
    int armLen = (int)(recordR * 0.84f);

    // Screen Y points down, so positive angles rotate clockwise.
    // 0 -> parked: stylus sits outside the record, upper-right.
    // 1 -> landed: stylus rests near the upper-right portion of the black
    // vinyl / cover boundary, matching the reference image.
    // Parked: just outside the vinyl edge. Landed: the stylus reaches the
    // black vinyl ring, outside the large center cover area.
    const float parkedAngle = 1.30f;
    const float landedAngle = 1.736f; // parked + ~25 degrees
    float p = m_armProgress;
    // smoothstep for an organic ease-in-out feel
    float pe = p * p * (3.0f - 2.0f * p);
    float angle = parkedAngle + (landedAngle - parkedAngle) * pe;

    // Compute arm segments. The arm bends slightly near the headshell.
    float dirX = cosf(angle);
    float dirY = sinf(angle);
    int jointX = pivotX + (int)(dirX * armLen * 0.62f);
    int jointY = pivotY + (int)(dirY * armLen * 0.62f);
    // Bend the second segment a bit toward the disc.
    float bendAngle = angle + 0.30f;
    int headX = jointX + (int)(cosf(bendAngle) * armLen * 0.42f);
    int headY = jointY + (int)(sinf(bendAngle) * armLen * 0.42f);

    int armThick = std::max(4, recordR / 22);
    int armThin  = std::max(3, recordR / 28);

    // Soft drop shadow behind the arm: subtle, close to the metal, not a
    // separate black "arm".
    HPEN shadowPen = CreatePen(PS_SOLID, armThick + 2, RGB(24, 22, 20));
    HGDIOBJ oldPen = SelectObject(dc, shadowPen);
    MoveToEx(dc, pivotX + 2, pivotY + 3, nullptr);
    LineTo(dc, jointX + 2, jointY + 3);
    LineTo(dc, headX + 2, headY + 3);
    SelectObject(dc, oldPen);
    DeleteObject(shadowPen);

    // Main arm — two slightly different thicknesses for a tapered look.
    HPEN armPen1 = CreatePen(PS_SOLID, armThick, RGB(238, 236, 230));
    oldPen = SelectObject(dc, armPen1);
    MoveToEx(dc, pivotX, pivotY, nullptr);
    LineTo(dc, jointX, jointY);
    SelectObject(dc, oldPen);
    DeleteObject(armPen1);

    HPEN armPen2 = CreatePen(PS_SOLID, armThin, RGB(228, 226, 218));
    oldPen = SelectObject(dc, armPen2);
    MoveToEx(dc, jointX, jointY, nullptr);
    LineTo(dc, headX, headY);
    SelectObject(dc, oldPen);
    DeleteObject(armPen2);

    // Pivot base: outer ring + inner cap.
    int pivotOuter = std::max(10, recordR / 9);
    int pivotInner = std::max(5, pivotOuter / 2);
    HBRUSH baseBr = CreateSolidBrush(RGB(60, 56, 52));
    HPEN basePen = CreatePen(PS_SOLID, 1, RGB(28, 26, 22));
    HGDIOBJ oB = SelectObject(dc, baseBr);
    HGDIOBJ oP = SelectObject(dc, basePen);
    Ellipse(dc, pivotX - pivotOuter, pivotY - pivotOuter,
                pivotX + pivotOuter, pivotY + pivotOuter);
    SelectObject(dc, oB);
    SelectObject(dc, oP);
    DeleteObject(baseBr);
    DeleteObject(basePen);

    HBRUSH capBr = CreateSolidBrush(RGB(238, 236, 230));
    oB = SelectObject(dc, capBr);
    Ellipse(dc, pivotX - pivotInner, pivotY - pivotInner,
                pivotX + pivotInner, pivotY + pivotInner);
    SelectObject(dc, oB);
    DeleteObject(capBr);

    // Tiny counterweight stub behind the pivot.
    int cwX = pivotX - (int)(dirX * pivotOuter * 1.4f);
    int cwY = pivotY - (int)(dirY * pivotOuter * 1.4f);
    int cwR = std::max(4, pivotInner);
    HBRUSH cwBr = CreateSolidBrush(RGB(48, 44, 40));
    oB = SelectObject(dc, cwBr);
    Ellipse(dc, cwX - cwR, cwY - cwR, cwX + cwR, cwY + cwR);
    SelectObject(dc, oB);
    DeleteObject(cwBr);

    // Headshell: a small rectangle at the tip, rotated to follow bendAngle.
    {
        float hx = (float)headX;
        float hy = (float)headY;
        float hw = (float)std::max(10, recordR / 7);
        float hh = (float)std::max(7, recordR / 11);
        float c1 = cosf(bendAngle), s1 = sinf(bendAngle);
        auto rot = [&](float lx, float ly, POINT& out) {
            out.x = (LONG)(hx + lx * c1 - ly * s1);
            out.y = (LONG)(hy + lx * s1 + ly * c1);
        };
        POINT shell[4];
        rot(-hw * 0.35f, -hh * 0.5f, shell[0]);
        rot( hw * 0.65f, -hh * 0.5f, shell[1]);
        rot( hw * 0.65f,  hh * 0.5f, shell[2]);
        rot(-hw * 0.35f,  hh * 0.5f, shell[3]);
        HBRUSH shellBr = CreateSolidBrush(RGB(244, 242, 238));
        HPEN shellPen = CreatePen(PS_SOLID, 1, RGB(30, 28, 24));
        HGDIOBJ oSB = SelectObject(dc, shellBr);
        HGDIOBJ oSP = SelectObject(dc, shellPen);
        Polygon(dc, shell, 4);
        SelectObject(dc, oSB);
        SelectObject(dc, oSP);
        DeleteObject(shellBr);
        DeleteObject(shellPen);

        // Stylus needle pointing toward the disc.
        POINT styT, styB;
        rot(hw * 0.55f, hh * 0.1f, styT);
        rot(hw * 0.85f, hh * 0.55f, styB);
        HPEN styPen = CreatePen(PS_SOLID, 2, RGB(40, 38, 34));
        HGDIOBJ oStP = SelectObject(dc, styPen);
        MoveToEx(dc, styT.x, styT.y, nullptr);
        LineTo(dc, styB.x, styB.y);
        SelectObject(dc, oStP);
        DeleteObject(styPen);
    }
}

int DanmakuEngine::allocateTrack() {
    if (m_trackUsage.empty()) return 0; // 防止空数组越界

    int track = 0;
    int minUsage = m_trackUsage[0];

    for (int i = 1; i < (int)m_trackUsage.size(); ++i) {
        if (m_trackUsage[i] < minUsage) {
            minUsage = m_trackUsage[i];
            track = i;
        }
    }

    m_trackUsage[track]++;
    return track;
}

void DanmakuEngine::setHwnd(HWND hwnd) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hwnd = hwnd;
}

void DanmakuEngine::recycleTrack(int track) {
    if (track >= 0 && track < (int)m_trackUsage.size()) {
        m_trackUsage[track]--;
        if (m_trackUsage[track] <= 0) {
            m_trackUsage[track]    = 0;
            m_trackRightEdge[track] = -1e9f; // lane fully free again
        }
    }
}

int DanmakuEngine::pickTrackForSpawn() {
    // Prefer lanes from top to bottom so a fresh/empty screen starts inserting
    // from the first row instead of an arbitrary least-used/bottom row. Still
    // require enough horizontal gap to avoid same-lane collisions.
    int requiredGap = m_config.spawnGapPx;
    if (requiredGap <= 0) {
        // User requested generous same-lane clearance: at least about 1/3~1/2 screen.
        requiredGap = std::max(m_width / 3, m_width / 2);
    }
    float bestEdge = (float)m_width - (float)requiredGap;
    for (int i = 0; i < (int)m_trackRightEdge.size(); ++i) {
        if (m_trackRightEdge[i] <= bestEdge) {
            return i;
        }
    }
    return -1;
}

void DanmakuEngine::dripFromPool() {
    if (m_pool.empty()) return;
    if (!m_config.enabled) return;
    if ((int)m_danmakuList.size() >= m_config.maxDanmaku) return;

    // Global cooldown: at most one new comment every spawnIntervalMs, no matter
    // how many lanes are free. This is the single biggest knob for visual density.
    DWORD now = GetTickCount();
    if (m_config.spawnIntervalMs > 0 &&
        (now - m_lastSpawnTick) < (DWORD)m_config.spawnIntervalMs) {
        return;
    }

    int track = pickTrackForSpawn();
    if (track < 0) return; // no lane ready yet — wait for items to scroll left

    const PooledComment& pc = m_pool[m_poolIndex];
    m_poolIndex = (m_poolIndex + 1) % m_pool.size(); // wrap = loop

    m_trackUsage[track]++;

    int laneH = (m_config.maxTracks > 0) ? (m_height / m_config.maxTracks) : m_height;
    if (laneH < 1) laneH = 1;
    int spawnW = (m_width > 0) ? m_width : 800;

    // Vertical padding inside a lane so consecutive lanes don't visually touch.
    int vPad = std::max(4, laneH / 8);

    DanmakuItem item;
    item.text  = pc.text;
    item.color = pc.color;
    item.x = (float)spawnW;
    item.y = (float)(track * laneH + vPad);
    item.speed = (m_config.baseSpeed > 0.f) ? m_config.baseSpeed : 120.0f;
    item.active = true;
    item.startTime = now;
    item.track = track;

    item.width = measureTextWidth(item.text);
    m_trackRightEdge[track] = item.x + item.width;

    m_danmakuList.push_back(item);
    m_lastSpawnTick = now;
}