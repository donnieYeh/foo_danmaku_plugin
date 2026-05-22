#ifndef DANMAKU_ENGINE_TYPES_H
#define DANMAKU_ENGINE_TYPES_H

#include <windows.h>
#include <string>
#include <vector>

struct DanmakuItem {
    std::wstring text;
    float x;          // current x position
    float y;         // y position (track)
    float speed;     // pixels per frame
    COLORREF color;   // RGB color
    DWORD startTime;  // start timestamp
    bool active;

    DanmakuItem() : x(0), y(0), speed(2.0f), color(RGB(255, 255, 255)),
                    startTime(0), active(false) {}
};

struct DanmakuConfig {
    int maxTracks;       // number of vertical tracks
    int maxDanmaku;      // max concurrent danmaku
    float baseSpeed;     // base scroll speed
    bool enabled;        // danmaku enabled
    int opacity;         // 0-255

    DanmakuConfig() : maxTracks(8), maxDanmaku(50), baseSpeed(2.0f),
                      enabled(true), opacity(230) {}
};

#endif // DANMAKU_ENGINE_TYPES_H