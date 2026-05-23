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

    // ── Pool-based looping playback ───────────────────────────────
    // UI calls addToPool() for each fetched comment. The engine then drips
    // them onto active tracks at a controlled cadence and loops back to
    // index 0 when the pool is exhausted, until clearPool() is invoked.
    void addToPool(const std::wstring& text, COLORREF color = RGB(255, 255, 255));
    void clearPool();
    int  poolSize() const;
    int  poolIndex() const;       // next item to be drip-spawned
    int  poolRemaining() const;   // poolSize - poolIndex (unread items left in current cycle)

    void onPaint(HDC hdc);
    void onTimer();

    void setEnabled(bool enabled);
    bool isEnabled() const;

    // When paused, onTimer() neither advances item positions nor drips from
    // the pool. onPaint() continues to render so the frozen frame stays
    // visible. Use this to sync the danmaku flow with audio playback state.
    void setPaused(bool paused);
    bool isPaused() const;

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
    std::vector<float> m_trackRightEdge; // rightmost x currently occupied per track
    mutable std::mutex m_mutex;

    // Pool of all fetched comments + current drip index (wraps for loop playback).
    std::vector<PooledComment> m_pool;
    size_t m_poolIndex;

    DWORD m_lastSpawnTick;    // GetTickCount() of last successful drip; for global cooldown
    DWORD m_lastUpdateTick;   // GetTickCount() of last position update; for time-based movement
    double m_subpixelRemainderMs; // accumulates fractional timer deltas for smoother movement
    bool  m_paused;           // when true, onTimer becomes a no-op

    void updateDanmakuPositions(float deltaSeconds);
    float measureTextWidth(const std::wstring& text) const;
    int allocateTrack();
    void recycleTrack(int track);
    void dripFromPool();      // try to spawn next pool item if any track has room
    int  pickTrackForSpawn(); // returns track index that can accept a new item right now, or -1
};

#endif // DANMAKU_ENGINE_H