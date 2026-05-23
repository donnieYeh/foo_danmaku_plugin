#ifndef DANMAKU_ENGINE_H
#define DANMAKU_ENGINE_H

#include "danmaku_engine_types.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
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

    // 鈹€鈹€ Pool-based looping playback 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
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

    // Album art shown as the spinning vinyl label. The engine takes ownership
    // of the HBITMAP and will DeleteObject it on replacement / shutdown.
    // Pass nullptr to clear the cover and fall back to the decorative wedges.
    void setCoverArt(HBITMAP bitmap);
    bool hasCoverArt() const;

    // Toggles the tone-arm landing animation. When true the arm swings onto
    // the record; when false it parks at the rest position outside the disc.
    // The animation is driven by onTimer() using time-based easing.
    void setArmLanded(bool landed);
    bool isArmLanded() const;

    // Hit-test the album-cover/center-label circle. Used by the UI to toggle
    // danmaku display when the user clicks the cover area.
    bool isPointInCoverArea(int x, int y) const;

private:
    HWND m_hwnd;
    HDC m_memDC;
    HBITMAP m_memBM;
    HFONT m_font;     // cached, created once, not per-frame
    HBITMAP m_coverBitmap; // optional album art rendered onto the vinyl label
    int     m_coverBitmapW;
    int     m_coverBitmapH;
    ID2D1Factory*          m_d2dFactory;
    IDWriteFactory*        m_dwriteFactory;
    ID2D1DCRenderTarget*   m_d2dTarget;
    IDWriteTextFormat*     m_dwriteTextFormat;
    ID2D1Bitmap*           m_coverD2DBitmap;
    bool                   m_coverD2DDirty;
    int m_width;
    int m_height;
    float m_recordAngle; // decorative vinyl rotation angle, radians
    bool  m_armLanded;       // target state: arm on disc (true) or parked off-disc (false)
    float m_armProgress;     // eased current progress, 0=parked, 1=landed
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
    void drawSoftBackground(HDC dc);
    void drawTurntable(HDC dc);
    bool createD2DTarget();
    void discardD2DTarget();
    void shutdownD2D();
    void rebuildD2DCoverIfNeeded();
    ID2D1Bitmap* createD2DBitmapFromHBITMAP(HBITMAP bitmap);
    void drawSoftBackgroundD2D();
    void drawTurntableD2D();
    void drawDanmakuD2D();
    int allocateTrack();
    void recycleTrack(int track);
    void dripFromPool();      // try to spawn next pool item if any track has room
    int  pickTrackForSpawn(); // returns track index that can accept a new item right now, or -1
};

#endif // DANMAKU_ENGINE_H


