#include "danmaku_engine.h"
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <utility>  // std::min / std::max
#include <cstdio>

// UI translation unit provides this — routes to foobar console + OutputDebugString.
// Declared extern here so we don't need to pull in fb2k SDK headers from engine.
extern "C" void danmaku_log_external(const char* msg);

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
    , m_font(nullptr), m_width(0), m_height(0), m_poolIndex(0)
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

    // Clear memory DC with black background (cheap PatBlt, off-screen — no flicker).
    HBRUSH hbr = (HBRUSH)GetStockObject(BLACK_BRUSH);
    HBRUSH oldBrush = (HBRUSH)SelectObject(m_memDC, hbr);
    PatBlt(m_memDC, 0, 0, m_width, m_height, PATCOPY);
    SelectObject(m_memDC, oldBrush);

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
    if (!m_config.enabled) return;
    DWORD now = GetTickCount();
    if (m_lastUpdateTick == 0) m_lastUpdateTick = now;
    if (m_paused) {
        // While paused, keep the timestamp fresh so resume doesn't jump.
        m_lastUpdateTick = now;
        return;   // audio paused: freeze scroll AND suspend drip
    }

    DWORD deltaMs = now - m_lastUpdateTick;
    m_lastUpdateTick = now;
    // Clamp long UI stalls; otherwise one busy foobar frame would teleport items.
    if (deltaMs > 50) deltaMs = 50;
    // GetTickCount is integer ms; keep fractional remainder from target 60Hz-ish
    // pacing so movement is less quantized at high speeds.
    double deltaPreciseMs = (double)deltaMs + m_subpixelRemainderMs;
    m_subpixelRemainderMs = deltaPreciseMs - floor(deltaPreciseMs);
    float deltaSeconds = (float)(deltaPreciseMs / 1000.0);

    updateDanmakuPositions(deltaSeconds);

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