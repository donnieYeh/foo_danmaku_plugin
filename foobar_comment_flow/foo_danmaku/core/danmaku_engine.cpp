#include "danmaku_engine.h"
#include <windows.h>
#include <cmath>
#include <algorithm>
#include <utility>  // std::min / std::max

DanmakuEngine::DanmakuEngine()
    : m_hwnd(nullptr), m_memDC(nullptr), m_memBM(nullptr)
    , m_font(nullptr), m_width(0), m_height(0) {
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

    if (m_config.maxTracks <= 0) m_config.maxTracks = 1;
    m_trackUsage.resize(m_config.maxTracks, 0);

    // 创建字体一次，后续复用，不再每帧 CreateFont
    m_font = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
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
    m_config = config;
    if (m_config.maxTracks <= 0) m_config.maxTracks = 1;
    m_trackUsage.resize(m_config.maxTracks, 0);
}

DanmakuConfig DanmakuEngine::getConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void DanmakuEngine::addDanmaku(const std::wstring& text, COLORREF color) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled || (int)m_danmakuList.size() >= m_config.maxDanmaku) {
        return;
    }

    DanmakuItem item;
    item.text = text;
    item.color = color;
    item.x = (float)m_width;
    item.y = (float)allocateTrack() * (m_height / m_config.maxTracks);
    item.speed = m_config.baseSpeed;
    item.active = true;
    item.startTime = GetTickCount();

    m_danmakuList.push_back(item);
}

void DanmakuEngine::clearDanmaku() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_danmakuList.clear();
    std::fill(m_trackUsage.begin(), m_trackUsage.end(), 0);
}

void DanmakuEngine::onPaint(HDC hdc) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled) return;
    if (!m_memDC || !m_memBM) return;

    // Clear memory DC with black background
    HBRUSH hbr = (HBRUSH)GetStockObject(BLACK_BRUSH);
    HBRUSH oldBrush = (HBRUSH)SelectObject(m_memDC, hbr);
    PatBlt(m_memDC, 0, 0, m_width, m_height, PATCOPY);
    SelectObject(m_memDC, oldBrush);

    // 使用缓存字体，不再每帧 CreateFont（原来每帧都泄漏字体句柄）
    HGDIOBJ oldFont = nullptr;
    if (m_font) {
        oldFont = SelectObject(m_memDC, m_font);
        SetBkMode(m_memDC, TRANSPARENT);
    }

    for (const auto& item : m_danmakuList) {
        if (!item.active) continue;
        SetTextColor(m_memDC, item.color);
        TextOutW(m_memDC, (int)item.x, (int)item.y, item.text.c_str(), (int)item.text.length());
    }

    // 必须先 deselect 字体再用，否则字体句柄被 DC 占用
    if (oldFont) SelectObject(m_memDC, oldFont);

    // Copy to target DC
    BitBlt(hdc, 0, 0, m_width, m_height, m_memDC, 0, 0, SRCCOPY);
}

void DanmakuEngine::onTimer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_config.enabled) return;

    updateDanmakuPositions();

    // Remove off-screen danmaku
    for (auto& item : m_danmakuList) {
        if (item.x < -500) {
            item.active = false;
        }
    }

    m_danmakuList.erase(
        std::remove_if(m_danmakuList.begin(), m_danmakuList.end(),
            [](const DanmakuItem& item) { return !item.active; }),
        m_danmakuList.end()
    );
}

void DanmakuEngine::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.enabled = enabled;
}

bool DanmakuEngine::isEnabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.enabled;
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

    // resize 后重建字体（字号可跟随高度缩放）
    if (m_font) { DeleteObject(m_font); m_font = nullptr; }
    int fontSize = std::max(14, std::min(32, height / 6));
    m_font = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
}

void DanmakuEngine::updateDanmakuPositions() {
    for (auto& item : m_danmakuList) {
        if (item.active) {
            item.x -= item.speed;
        }
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
    }
}