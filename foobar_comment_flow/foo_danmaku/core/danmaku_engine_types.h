#ifndef DANMAKU_ENGINE_TYPES_H
#define DANMAKU_ENGINE_TYPES_H

#include <windows.h>
#include <string>
#include <vector>

struct DanmakuItem {
    std::wstring text;
    float x;          // current x position
    float y;         // y position (track)
    float width;     // measured text width in pixels; used for precise off-screen removal
    float speed;     // pixels per second
    COLORREF color;   // RGB color
    DWORD startTime;  // start timestamp
    bool active;
    int  track;       // which lane this item occupies (for recycleTrack)

    DanmakuItem() : x(0), y(0), width(0), speed(120.0f), color(RGB(255, 255, 255)),
                    startTime(0), active(false), track(-1) {}
};

struct DanmakuConfig {
    int maxTracks;       // number of vertical tracks
    int maxDanmaku;      // max concurrent danmaku on screen
    float baseSpeed;     // base scroll speed (pixels/second)
    bool enabled;        // danmaku enabled
    int opacity;         // 0-255 (unused for now)
    int spawnGapPx;      // min horizontal gap (on the same track) before next spawn
    int spawnIntervalMs; // global min time between any two consecutive spawns
    float turntableSpeed; // vinyl rotation speed, radians/second

    // Density-tuned defaults: 4 lanes (wider vertical spacing), 5s global
    // cooldown, and 2x the original 2px@60fps scroll speed (240px/s).
    // spawnGapPx <= 0 means dynamic same-lane gap: at least half the panel width.
    DanmakuConfig() : maxTracks(4), maxDanmaku(60), baseSpeed(240.0f),
                      enabled(true), opacity(230),
                      spawnGapPx(0), spawnIntervalMs(5000),
                      turntableSpeed(0.42f) {}
};

// One immutable entry in the engine's comment pool. The engine drips these
// onto the screen over time and loops back to index 0 once exhausted.
struct PooledComment {
    std::wstring text;
    COLORREF     color;
    PooledComment() : color(RGB(255, 255, 255)) {}
    PooledComment(std::wstring t, COLORREF c) : text(std::move(t)), color(c) {}
};

#endif // DANMAKU_ENGINE_TYPES_H