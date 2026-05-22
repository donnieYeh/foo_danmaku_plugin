#ifndef DANMAKU_ENGINE_H
#define DANMAKU_ENGINE_H

#include "danmaku_engine_types.h"
#include <windows.h>
#include <mutex>

class DanmakuEngine {
public:
    DanmakuEngine();
    ~DanmakuEngine();

    void init(HWND parentWnd);
    void shutdown();

    void setConfig(const DanmakuConfig& config);
    DanmakuConfig getConfig() const;

    void addDanmaku(const std::wstring& text, COLORREF color = RGB(255, 255, 255));
    void clearDanmaku();

    void onPaint(HDC hdc);
    void onTimer();

    void setEnabled(bool enabled);
    bool isEnabled() const;

    void resize(int width, int height);
    void setHwnd(HWND hwnd);

private:
    HWND m_hwnd;
    HDC m_memDC;
    HBITMAP m_memBM;
    HFONT m_font;     // cached, created once, not per-frame
    int m_width;
    int m_height;
    DanmakuConfig m_config;
    std::vector<DanmakuItem> m_danmakuList;
    std::vector<int> m_trackUsage;  // track usage counter for collision avoidance
    mutable std::mutex m_mutex;

    void updateDanmakuPositions();
    int allocateTrack();
    void recycleTrack(int track);
};

#endif // DANMAKU_ENGINE_H