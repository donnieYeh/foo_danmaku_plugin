#include "danmaku_engine.h"
#include <windows.h>
#include <gdiplus.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cmath>
#include <algorithm>
#include <utility>  // std::min / std::max
#include <cstdio>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// UI translation unit provides this 鈥?routes to foobar console + OutputDebugString.
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

template <typename T>
static void releaseCom(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

DanmakuEngine::DanmakuEngine()
    : m_hwnd(nullptr), m_memDC(nullptr), m_memBM(nullptr)
    , m_font(nullptr), m_coverBitmap(nullptr), m_coverBitmapW(0), m_coverBitmapH(0)
    , m_d2dFactory(nullptr), m_dwriteFactory(nullptr)
    , m_d2dTarget(nullptr), m_dwriteTextFormat(nullptr)
    , m_coverD2DBitmap(nullptr), m_coverD2DDirty(true)
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

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
    if (FAILED(hr)) {
        engine_log("[Danmaku/engine] D2D factory creation failed hr=0x%08x", (unsigned)hr);
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&m_dwriteFactory));
    if (FAILED(hr)) {
        engine_log("[Danmaku/engine] DWrite factory creation failed hr=0x%08x", (unsigned)hr);
    }
    if (m_dwriteFactory) {
        hr = m_dwriteFactory->CreateTextFormat(
            L"Microsoft YaHei", nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            40.0f,
            L"zh-cn",
            &m_dwriteTextFormat);
        if (SUCCEEDED(hr) && m_dwriteTextFormat) {
            m_dwriteTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_dwriteTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    }

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

    // 鍒涘缓瀛椾綋涓€娆★紝鍚庣画澶嶇敤锛屼笉鍐嶆瘡甯?CreateFont
    m_font = CreateFontW(40, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
}

void DanmakuEngine::shutdown() {
    shutdownD2D();

    // 鍏堟妸瀛椾綋浠?DC 涓?deselect锛屽啀鍒犻櫎
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
    releaseCom(m_coverD2DBitmap);
    m_coverD2DDirty = true;
    m_coverBitmap = bitmap;
    if (m_coverBitmap) {
        BITMAP bm = {};
        if (GetObject(m_coverBitmap, sizeof(bm), &bm)) {
            m_coverBitmapW = bm.bmWidth;
            m_coverBitmapH = bm.bmHeight;
        }
    }
}

bool DanmakuEngine::hasCoverArt() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_coverBitmap != nullptr && m_coverBitmapW > 0 && m_coverBitmapH > 0;
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
        // No track has clearance 鈥?try least-used as fallback (overlap acceptable).
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

    if (m_hwnd) {
        RECT rc = {};
        if (GetClientRect(m_hwnd, &rc)) {
            int cw = std::max<LONG>(1, rc.right - rc.left);
            int ch = std::max<LONG>(1, rc.bottom - rc.top);
            if (m_width != cw || m_height != ch) {
                m_width = cw;
                m_height = ch;
            }
        }
    }

    if (createD2DTarget() && m_d2dTarget) {
        RECT bindRc = {0, 0, m_width, m_height};
        HRESULT bindHr = m_d2dTarget->BindDC(hdc, &bindRc);
        if (FAILED(bindHr)) { discardD2DTarget(); return; }
        rebuildD2DCoverIfNeeded();
        m_d2dTarget->BeginDraw();
        m_d2dTarget->PushAxisAlignedClip(
            D2D1::RectF(0, 0, (FLOAT)m_width, (FLOAT)m_height),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        drawSoftBackgroundD2D();
        drawTurntableD2D();
        drawDanmakuD2D();
        m_d2dTarget->PopAxisAlignedClip();
        HRESULT hr = m_d2dTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            discardD2DTarget();
        }
        return;
    }

    if (!m_memDC || !m_memBM) return;
    drawSoftBackground(m_memDC);
    drawTurntable(m_memDC);
    if (m_config.enabled && m_font) {
        HGDIOBJ oldFont = SelectObject(m_memDC, m_font);
        SetBkMode(m_memDC, TRANSPARENT);
        for (const auto& item : m_danmakuList) {
            if (!item.active) continue;
            SetTextColor(m_memDC, item.color);
            TextOutW(m_memDC, (int)item.x, (int)item.y, item.text.c_str(), (int)item.text.length());
        }
        SelectObject(m_memDC, oldFont);
    }
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

    // Arm landing easing 鈥?runs in both paused and playing state so the arm
    // can smoothly lift away when playback stops.
    {
        float target = m_armLanded ? 1.0f : 0.0f;
        float diff = target - m_armProgress;
        // Exponential ease 鈥?visually smooth, framerate-independent.
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
    
    // 鍏堝皢瀛椾綋浠?DC 涓?deselect锛岀劧鍚庨噴鏀炬棫璧勬簮
    if (m_memDC && m_font)
        SelectObject(m_memDC, GetStockObject(SYSTEM_FONT));

    if (m_memBM) { DeleteObject(m_memBM); m_memBM = nullptr; }
    if (m_memDC) { DeleteDC(m_memDC);     m_memDC = nullptr; }

    HDC hdc = GetDC(m_hwnd);
    if (!hdc) hdc = GetDC(nullptr); // fallback锛氱敤灞忓箷 DC 涔熻兘鍒涘缓鍏煎 DC
    m_memDC = CreateCompatibleDC(hdc);
    m_memBM = CreateCompatibleBitmap(hdc, width, height);
    ReleaseDC(m_hwnd, hdc);

    if (m_memDC && m_memBM) {
        SelectObject(m_memDC, m_memBM);
    } else {
        // 鍒涘缓澶辫触鏃舵竻鐞嗭紝閬垮厤鎮┖鎸囬拡
        if (m_memBM) { DeleteObject(m_memBM); m_memBM = nullptr; }
        if (m_memDC) { DeleteDC(m_memDC);     m_memDC = nullptr; }
        return;
    }

    // resize 鍚庨噸寤哄瓧浣擄紙瀛楀彿璺熼殢楂樺害缂╂斁锛屾斁澶т竴鍊嶏級
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

    // 鈹€鈹€ Record shadow / seat 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
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

    // 鈹€鈹€ Black vinyl disc 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
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

    // 鈹€鈹€ Center label / album art 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
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

    // 鈹€鈹€ Tone arm 鈥?animated landing, polished detail 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
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

    // Main arm 鈥?two slightly different thicknesses for a tapered look.
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
    if (m_trackUsage.empty()) return 0; // 闃叉绌烘暟缁勮秺鐣?

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
    if (m_hwnd == hwnd) return;
    m_hwnd = hwnd;
    discardD2DTarget();
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
    if (track < 0) return; // no lane ready yet 鈥?wait for items to scroll left

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

bool DanmakuEngine::createD2DTarget() {
    if (m_d2dTarget) return true;
    if (!m_d2dFactory || !m_hwnd || m_width <= 0 || m_height <= 0) return false;

    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    UINT32 w = (UINT32)std::max<LONG>(1, rc.right - rc.left);
    UINT32 h = (UINT32)std::max<LONG>(1, rc.bottom - rc.top);
    m_width = (int)w;
    m_height = (int)h;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    HRESULT hr = m_d2dFactory->CreateDCRenderTarget(&props, &m_d2dTarget);

    if (FAILED(hr)) {
        engine_log("[Danmaku/engine] D2D target creation failed hr=0x%08x", (unsigned)hr);
        m_d2dTarget = nullptr;
        return false;
    }
    m_d2dTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return true;
}

void DanmakuEngine::discardD2DTarget() {
    releaseCom(m_d2dTarget);
}

void DanmakuEngine::shutdownD2D() {
    discardD2DTarget();
    releaseCom(m_dwriteTextFormat);
    releaseCom(m_dwriteFactory);
    releaseCom(m_d2dFactory);
}



static D2D1_COLOR_F d2dFromColorRef(COLORREF c, float a = 1.0f) {
    return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a);
}

ID2D1Bitmap* DanmakuEngine::createD2DBitmapFromHBITMAP(HBITMAP bitmap) {
    if (!m_d2dTarget || !bitmap) return nullptr;
    Gdiplus::Bitmap gdip(bitmap, nullptr);
    if (gdip.GetLastStatus() != Gdiplus::Ok) return nullptr;
    Gdiplus::Rect rect(0, 0, (INT)gdip.GetWidth(), (INT)gdip.GetHeight());
    Gdiplus::BitmapData data = {};
    if (gdip.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) return nullptr;
    ID2D1Bitmap* out = nullptr;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    HRESULT hr = m_d2dTarget->CreateBitmap(D2D1::SizeU((UINT32)data.Width, (UINT32)data.Height),
        data.Scan0, (UINT32)data.Stride, &props, &out);
    gdip.UnlockBits(&data);
    return SUCCEEDED(hr) ? out : nullptr;
}

void DanmakuEngine::rebuildD2DCoverIfNeeded() {
    if (!m_coverD2DDirty) return;
    releaseCom(m_coverD2DBitmap);
    if (m_coverBitmap) m_coverD2DBitmap = createD2DBitmapFromHBITMAP(m_coverBitmap);
    m_coverD2DDirty = false;
}

void DanmakuEngine::drawSoftBackgroundD2D() {
    if (!m_d2dTarget) return;
    ID2D1GradientStopCollection* stops = nullptr;
    ID2D1LinearGradientBrush* grad = nullptr;
    D2D1_GRADIENT_STOP gs[3] = {
        {0.0f, d2dFromColorRef(RGB(62,64,64))},
        {0.55f, d2dFromColorRef(RGB(45,47,47))},
        {1.0f, d2dFromColorRef(RGB(28,30,30))}
    };
    if (SUCCEEDED(m_d2dTarget->CreateGradientStopCollection(gs, 3, &stops)) &&
        SUCCEEDED(m_d2dTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0), D2D1::Point2F(0,(FLOAT)m_height)), stops, &grad))) {
        m_d2dTarget->FillRectangle(D2D1::RectF(0,0,(FLOAT)m_width,(FLOAT)m_height), grad);
    } else {
        m_d2dTarget->Clear(d2dFromColorRef(RGB(36,38,40)));
    }
    releaseCom(grad); releaseCom(stops);
    if (m_coverD2DBitmap) {
        D2D1_SIZE_F sz = m_coverD2DBitmap->GetSize();
        float scale = std::max((float)m_width / sz.width, (float)m_height / sz.height);
        float dw = sz.width * scale, dh = sz.height * scale;
        D2D1_RECT_F dst = D2D1::RectF((m_width-dw)*0.5f, (m_height-dh)*0.5f, (m_width+dw)*0.5f, (m_height+dh)*0.5f);
        m_d2dTarget->DrawBitmap(m_coverD2DBitmap, dst, 0.18f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        ID2D1SolidColorBrush* veil = nullptr;
        if (SUCCEEDED(m_d2dTarget->CreateSolidColorBrush(D2D1::ColorF(0.08f,0.09f,0.10f,0.58f), &veil))) {
            m_d2dTarget->FillRectangle(D2D1::RectF(0,0,(FLOAT)m_width,(FLOAT)m_height), veil);
        }
        releaseCom(veil);
    }
}

void DanmakuEngine::drawTurntableD2D() {
    if (!m_d2dTarget || m_width <= 80 || m_height <= 80) return;
    int panelMin = std::min(m_width, m_height);
    int recordR = std::min(std::max(48, (int)(panelMin * 0.36f)), std::min((int)(m_width*0.42f), (int)(m_height*0.42f)));
    float cx = m_width * 0.5f;
    float cy = (m_height < 220) ? m_height * 0.5f : m_height * 0.47f;
    ID2D1SolidColorBrush* b = nullptr;
    if (FAILED(m_d2dTarget->CreateSolidColorBrush(D2D1::ColorF(0,0,0,1), &b))) return;
    for (int k=12;k>=2;k-=2) { b->SetColor(D2D1::ColorF(0,0,0,0.05f+k*0.006f)); m_d2dTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy+6), recordR+(FLOAT)k, recordR+(FLOAT)k), b); }
    b->SetColor(d2dFromColorRef(RGB(16,17,17))); m_d2dTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),(FLOAT)recordR,(FLOAT)recordR), b);
    b->SetColor(d2dFromColorRef(RGB(4,5,7))); m_d2dTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),(FLOAT)recordR,(FLOAT)recordR), b, 2.0f);
    for (int r=recordR-7;r>recordR/2;r-=5) { int shade=24+((recordR-r)%18); b->SetColor(d2dFromColorRef(RGB(shade,shade,shade),0.75f)); m_d2dTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),(FLOAT)r,(FLOAT)r), b, 1.0f); }
    int labelR = std::max(28, (int)(recordR*0.68f));
    if (m_coverD2DBitmap) {
        ID2D1EllipseGeometry* clip = nullptr; ID2D1Layer* layer = nullptr;
        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx,cy),(FLOAT)labelR,(FLOAT)labelR), &clip)) && SUCCEEDED(m_d2dTarget->CreateLayer(nullptr, &layer))) {
            m_d2dTarget->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
        }
        D2D1_SIZE_F sz = m_coverD2DBitmap->GetSize();
        D2D1_MATRIX_3X2_F old; m_d2dTarget->GetTransform(&old);
        m_d2dTarget->SetTransform(D2D1::Matrix3x2F::Rotation(m_recordAngle*57.2957795f, D2D1::Point2F(cx,cy)) * old);
        m_d2dTarget->DrawBitmap(m_coverD2DBitmap, D2D1::RectF(cx-labelR,cy-labelR,cx+labelR,cy+labelR), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, D2D1::RectF(0,0,sz.width,sz.height));
        m_d2dTarget->SetTransform(old);
        if (layer) { m_d2dTarget->PopLayer(); releaseCom(layer); } releaseCom(clip);
        b->SetColor(d2dFromColorRef(RGB(20,18,16))); m_d2dTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),(FLOAT)labelR,(FLOAT)labelR), b, 2.0f);
    } else { b->SetColor(d2dFromColorRef(RGB(118,122,128))); m_d2dTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),(FLOAT)labelR,(FLOAT)labelR), b); }
    b->SetColor(d2dFromColorRef(RGB(6,7,8))); m_d2dTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),5,5), b);
    int pivotX=(int)(cx+recordR*1.12f), pivotY=(int)(cy-recordR*0.98f), armLen=(int)(recordR*0.84f);
    float pe=m_armProgress*m_armProgress*(3.0f-2.0f*m_armProgress), angle=1.30f+(1.736f-1.30f)*pe;
    float dirX=cosf(angle), dirY=sinf(angle); int jointX=pivotX+(int)(dirX*armLen*0.62f), jointY=pivotY+(int)(dirY*armLen*0.62f);
    float bendAngle=angle+0.30f; int headX=jointX+(int)(cosf(bendAngle)*armLen*0.42f), headY=jointY+(int)(sinf(bendAngle)*armLen*0.42f);
    float armThick=(float)std::max(4,recordR/22), armThin=(float)std::max(3,recordR/28);
    b->SetColor(D2D1::ColorF(0,0,0,0.24f)); m_d2dTarget->DrawLine(D2D1::Point2F((FLOAT)pivotX+2,(FLOAT)pivotY+3),D2D1::Point2F((FLOAT)jointX+2,(FLOAT)jointY+3),b,armThick+2); m_d2dTarget->DrawLine(D2D1::Point2F((FLOAT)jointX+2,(FLOAT)jointY+3),D2D1::Point2F((FLOAT)headX+2,(FLOAT)headY+3),b,armThin+2);
    b->SetColor(d2dFromColorRef(RGB(238,236,230))); m_d2dTarget->DrawLine(D2D1::Point2F((FLOAT)pivotX,(FLOAT)pivotY),D2D1::Point2F((FLOAT)jointX,(FLOAT)jointY),b,armThick);
    b->SetColor(d2dFromColorRef(RGB(228,226,218))); m_d2dTarget->DrawLine(D2D1::Point2F((FLOAT)jointX,(FLOAT)jointY),D2D1::Point2F((FLOAT)headX,(FLOAT)headY),b,armThin);

    // Headshell block at the stylus end. This mirrors the old GDI polygon that
    // was temporarily lost during the Direct2D migration.
    {
        float hx = (float)headX;
        float hy = (float)headY;
        float hw = (float)std::max(10, recordR / 7);
        float hh = (float)std::max(7, recordR / 11);
        float c1 = cosf(bendAngle), s1 = sinf(bendAngle);
        auto pt = [&](float lx, float ly) -> D2D1_POINT_2F {
            return D2D1::Point2F(hx + lx * c1 - ly * s1,
                                 hy + lx * s1 + ly * c1);
        };
        D2D1_POINT_2F shell[4] = {
            pt(-hw * 0.35f, -hh * 0.5f),
            pt( hw * 0.65f, -hh * 0.5f),
            pt( hw * 0.65f,  hh * 0.5f),
            pt(-hw * 0.35f,  hh * 0.5f)
        };
        ID2D1PathGeometry* geom = nullptr;
        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(&geom)) && geom) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(geom->Open(&sink)) && sink) {
                sink->BeginFigure(shell[0], D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(shell[1]);
                sink->AddLine(shell[2]);
                sink->AddLine(shell[3]);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                releaseCom(sink);

                b->SetColor(D2D1::ColorF(0, 0, 0, 0.22f));
                D2D1_MATRIX_3X2_F oldTransform;
                m_d2dTarget->GetTransform(&oldTransform);
                m_d2dTarget->SetTransform(D2D1::Matrix3x2F::Translation(2.0f, 3.0f) * oldTransform);
                m_d2dTarget->FillGeometry(geom, b);
                m_d2dTarget->SetTransform(oldTransform);

                b->SetColor(d2dFromColorRef(RGB(244,242,238)));
                m_d2dTarget->FillGeometry(geom, b);
                b->SetColor(d2dFromColorRef(RGB(30,28,24)));
                m_d2dTarget->DrawGeometry(geom, b, 1.0f);
            }
            releaseCom(geom);
        }

        D2D1_POINT_2F styT = pt(hw * 0.55f, hh * 0.1f);
        D2D1_POINT_2F styB = pt(hw * 0.85f, hh * 0.55f);
        b->SetColor(d2dFromColorRef(RGB(40,38,34)));
        m_d2dTarget->DrawLine(styT, styB, b, 2.0f);
    }
    float po=(float)std::max(10,recordR/9), pi=(float)std::max(5,(int)po/2); b->SetColor(d2dFromColorRef(RGB(60,56,52))); m_d2dTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F((FLOAT)pivotX,(FLOAT)pivotY),po,po),b); b->SetColor(d2dFromColorRef(RGB(238,236,230))); m_d2dTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F((FLOAT)pivotX,(FLOAT)pivotY),pi,pi),b);
    releaseCom(b);
}

void DanmakuEngine::drawDanmakuD2D() {
    if (!m_d2dTarget || !m_dwriteTextFormat || !m_config.enabled) return;
    ID2D1SolidColorBrush* b = nullptr; if (FAILED(m_d2dTarget->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &b))) return;
    for (const auto& item : m_danmakuList) { if (!item.active) continue; b->SetColor(d2dFromColorRef(item.color,1.0f)); m_d2dTarget->DrawTextW(item.text.c_str(), (UINT32)item.text.length(), m_dwriteTextFormat, D2D1::RectF(item.x,item.y,item.x+item.width+96.0f,item.y+96.0f), b); }
    releaseCom(b);
}






