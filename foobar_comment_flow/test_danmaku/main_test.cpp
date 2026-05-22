#include "test_danmaku.h"
#include <windows.h>
#include <wininet.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstdio>

#pragma comment(lib, "wininet.lib")

static int g_topMargin = 0;
static int g_bottomMargin = 200;
static int g_leftMargin = 0;
static int g_rightMargin = 0;
static int g_fontSize = 24;
static int g_trackCount = 8;

static HWND g_danmakuHwnd = nullptr;
static HWND g_mainHwnd = nullptr;
static HWND g_msgHwnd = nullptr;

class DanmakuEngine;
static DanmakuEngine* g_engine = nullptr;

static RECT g_wa;
static int g_screenW, g_screenH;
static BOOL g_mouseOver = FALSE;
static DWORD g_lastActivity = 0;
static int g_fadeTimeout = 2000;
static std::vector<TestComment> g_tempComments;

static std::wstring utf8ToUnicode(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len == 0) return std::wstring();
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], len);
    return result;
}

static std::string unicodeToUtf8(const std::wstring& unicode) {
    if (unicode.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, unicode.c_str(), (int)unicode.size(), nullptr, 0, nullptr, nullptr);
    if (len == 0) return std::string();
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, unicode.c_str(), (int)unicode.size(), &result[0], len, nullptr, nullptr);
    return result;
}

static std::string urlEncode(const std::string& input) {
    std::string result;
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : input) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else if (c == ' ') {
            result += '+';
        } else {
            result += '%';
            result += hex[(c >> 4) & 0xF];
            result += hex[c & 0xF];
        }
    }
    return result;
}

static std::wstring utf8ToWstring(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len == 0) return std::wstring();
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], len);
    return result;
}

static std::string httpGet(const std::wstring& wurl) {
    std::string response;
    printf("[HTTP] GET: %ws\n", wurl.c_str());

    HINTERNET hInternet = InternetOpenW(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
    if (!hInternet) { printf("[HTTP] InternetOpen failed\n"); return response; }

    HINTERNET hConnect = InternetOpenUrlW(hInternet, wurl.c_str(),
        nullptr, 0,
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_RELOAD, 0);
    if (!hConnect) {
        printf("[HTTP] InternetOpenUrl failed. Error: %d\n", GetLastError());
        InternetCloseHandle(hInternet);
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusCodeLen = sizeof(statusCode);
    HttpQueryInfo(hConnect, HTTP_QUERY_STATUS_CODE, &statusCode, &statusCodeLen, nullptr);
    printf("[HTTP] Status: %d\n", statusCode);

    char buffer[8192];
    DWORD bytesRead = 0;
    while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        response += buffer;
        bytesRead = 0;
    }
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    printf("[HTTP] Got %d bytes\n", (int)response.length());
    if (!response.empty()) {
        printf("[HTTP] First 200 chars: %.200s\n", response.c_str());
    }
    return response;
}

static std::string extractString(const char* start, const char* end, int maxLen) {
    std::string result;
    result.reserve(maxLen);
    int count = 0;
    while (start < end && *start != '"' && count < maxLen) {
        if (*start == '\\') {
            start++;
            if (start < end) {
                if (*start == 'n') result += '\n';
                else if (*start == '"') result += '"';
                else result += *start;
            }
        } else {
            result += *start;
        }
        start++;
        count++;
    }
    return result;
}

bool searchSong(const std::wstring& keyword, std::vector<TestSongResult>& results) {
    // Convert UTF-16LE keyword to UTF-8 for Python
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, keyword.c_str(), (int)keyword.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8Keyword(utf8Len, 0);
    WideCharToMultiByte(CP_UTF8, 0, keyword.c_str(), (int)keyword.size(), &utf8Keyword[0], utf8Len, nullptr, nullptr);

    printf("[API] Searching via Python SDK: %ws\n", keyword.c_str());

    // Build Python command
    std::string pyCmd = "python fetch_comments.py ";
    pyCmd += utf8Keyword;

    FILE* pipe = _popen(pyCmd.c_str(), "rb");
    if (!pipe) { printf("[API] _popen failed\n"); return false; }

    std::string resultJson;
    char buffer[4096];
    while (fread(buffer, 1, sizeof(buffer)-1, pipe) > 0) {
        buffer[sizeof(buffer)-1] = '\0';
        resultJson += buffer;
    }
    int rc = _pclose(pipe);
    if (rc != 0 || resultJson.empty()) { printf("[API] Python script failed or empty\n"); return false; }

    // Parse JSON result
    const char* json = resultJson.c_str();
    const char* end = json + resultJson.length();

    const char* songIdP = strstr(json, "\"songId\":");
    const char* songNameP = strstr(json, "\"songName\":");
    const char* artistNameP = strstr(json, "\"artistName\":");
    const char* commentsP = strstr(json, "\"comments\":");

    if (songIdP && songNameP) {
        TestSongResult r;

        // Extract songId
        const char* idStart = strchr(songIdP + 9, '"');
        if (idStart && ++idStart < end) {
            const char* idEnd = strchr(idStart, '"');
            if (idEnd && idEnd < end) {
                r.songId = std::string(idStart, idEnd - idStart);
                printf("[API] Song ID: %s\n", r.songId.c_str());
            }
        }

        // Extract songName (UTF-8 to wstring)
        const char* nameStart = strchr(songNameP + 11, '"');
        if (nameStart && ++nameStart < end) {
            const char* nameEnd = strchr(nameStart, '"');
            if (nameEnd && nameEnd < end) {
                r.songName = utf8ToUnicode(std::string(nameStart, nameEnd - nameStart));
                printf("[API] Song name: %ws\n", r.songName.c_str());
            }
        }

        // Extract artistName
        if (artistNameP) {
            const char* artStart = strchr(artistNameP + 13, '"');
            if (artStart && ++artStart < end) {
                const char* artEnd = strchr(artStart, '"');
                if (artEnd && artEnd < end) {
                    r.artistName = utf8ToUnicode(std::string(artStart, artEnd - artStart));
                    printf("[API] Artist: %ws\n", r.artistName.c_str());
                }
            }
        }

        if (!r.songId.empty()) {
            results.push_back(r);
        }
    }

    if (results.empty()) { printf("[API] No songs parsed from Python output\n"); return false; }

    // Also load comments
    if (commentsP) {
        printf("[API] Loading comments from Python output\n");
        const char* p = commentsP + 12;
        if (*p == '[') p++;
        int count = 0;
        while (count < 30) {
            const char* objStart = strchr(p, '{');
            if (!objStart || objStart >= end) break;
            const char* objEnd = strchr(objStart, '}');
            if (!objEnd || objEnd >= end) break;

            TestComment comment;
            comment.likeCount = 0;

            const char* contentP = strstr(objStart, "\"content\":\"");
            if (contentP && contentP < objEnd) {
                const char* cStart = contentP + 12;
                const char* cEnd = strchr(cStart, '"');
                if (cEnd && cEnd < objEnd) {
                    std::string contentStr(cStart, cEnd - cStart);
                    comment.content = utf8ToUnicode(contentStr);
                }
            }

            const char* likedP = strstr(objStart, "\"likedCount\":");
            if (likedP && likedP < objEnd) {
                const char* numP = likedP + 13;
                while (*numP && !isdigit(*numP) && numP < objEnd) numP++;
                if (numP < objEnd && isdigit(*numP)) {
                    char numStr[32] = {0};
                    int j = 0;
                    while (*numP && isdigit(*numP) && j < 31) numStr[j++] = *numP++;
                    comment.likeCount = atoi(numStr);
                }
            }

            if (!comment.content.empty()) {
                printf("[%d] %ws (likes: %d)\n", count, comment.content.c_str(), comment.likeCount);
                g_tempComments.push_back(comment);
                count++;
            }

            p = objEnd + 1;
        }
    }

    return true;
}

bool getComments(const std::string& songId, std::vector<TestComment>& comments, int limit = 20) {
    std::wstring wurl = L"https://music.163.com/api/v1/resource/comments/R_SO_4_";
    wurl += utf8ToWstring(songId);
    wurl += L"?limit=" + utf8ToWstring(std::to_string(limit)) + L"&offset=0";

    printf("[API] Getting comments for songId: %s\n", songId.c_str());
    std::string response = httpGet(wurl);
    if (response.empty()) { printf("[API] Comments failed - empty response\n"); return false; }

    const char* json = response.c_str();
    const char* end = json + response.length();

    const char* hotP = strstr(json, "\"hotComments\":[");
    const char* searchP = hotP ? hotP : strstr(json, "\"comments\":[");
    if (!searchP) { printf("[API] No comments section found\n"); return false; }
    searchP = strchr(searchP, '[');
    if (!searchP) return false;
    searchP++;

    int count = 0;
    const char* p = searchP;
    while (count < limit && p < end) {
        const char* objP = strchr(p, '{');
        if (!objP || objP >= end) break;
        p = objP;

        const char* contentP = strstr(p, "\"content\":\"");
        if (contentP && contentP < end) {
            const char* contentStart = contentP + 11;
            const char* contentEnd = strchr(contentStart, '"');
            if (contentEnd && contentEnd < end) {
                TestComment comment;
                comment.content = utf8ToUnicode(extractString(contentStart, contentEnd, 1024));

                const char* likedP = strstr(p, "\"likedCount\":");
                if (likedP) {
                    const char* numP = likedP + 13;
                    while (*numP && !isdigit(*numP) && numP < end) numP++;
                    if (numP < end && isdigit(*numP)) {
                        char likedStr[32] = {0};
                        int j = 0;
                        while (*numP && isdigit(*numP) && j < 31) likedStr[j++] = *numP++;
                        comment.likeCount = atoi(likedStr);
                    }
                }

                printf("[%d] %ws (likes: %d)\n", count, comment.content.c_str(), comment.likeCount);
                comments.push_back(comment);
                count++;
            }
        }

        p = strchr(p, '}');
        if (!p) break;
        p++;
    }

    return !comments.empty();
}

struct DanmakuItem {
    std::wstring text;
    float x, y, speed;
    COLORREF color;
    bool active;
    DanmakuItem() : x(0), y(0), speed(2.0f), color(RGB(255,255,255)), active(false) {}
};

class DanmakuEngine {
public:
    DanmakuEngine() : m_hwnd(nullptr), m_memDC(nullptr), m_memBM(nullptr),
                      m_width(800), m_height(200), m_enabled(true),
                      m_topMargin(0), m_bottomMargin(200), m_fontSize(24),
                      m_opacity(0) {
        m_config.maxTracks = g_trackCount;
        m_config.maxDanmaku = 50;
        m_config.baseSpeed = 2.0f;
    }
    ~DanmakuEngine() { shutdown(); }

    void init(HWND hwnd, int width, int height) {
        m_hwnd = hwnd;
        m_width = width;
        m_height = height;
        HDC hdc = GetDC(hwnd);
        m_memDC = CreateCompatibleDC(hdc);
        m_memBM = CreateCompatibleBitmap(hdc, width, height);
        SelectObject(m_memDC, m_memBM);
        ReleaseDC(m_hwnd, hdc);
        m_config.maxTracks = g_trackCount;
        m_trackUsage.resize(m_config.maxTracks, 0);
    }

    void shutdown() {
        if (m_memDC) { DeleteDC(m_memDC); m_memDC = nullptr; }
        if (m_memBM) { DeleteObject(m_memBM); m_memBM = nullptr; }
    }

    void addDanmaku(const std::wstring& text, COLORREF color = RGB(255,255,255)) {
        if (!m_enabled || (int)m_danmakuList.size() >= m_config.maxDanmaku) return;
        DanmakuItem item;
        item.text = text;
        item.color = color;
        item.x = (float)m_width;
        item.y = (float)allocateTrack() * (m_height / m_config.maxTracks);
        item.speed = m_config.baseSpeed;
        item.active = true;
        m_danmakuList.push_back(item);
        printf("[DANMAKU] Added: %ws\n", text.c_str());
    }

    void onTimer() {
        if (!m_enabled) return;
        for (auto& item : m_danmakuList) {
            if (item.active) item.x -= item.speed;
            if (item.x < -500) item.active = false;
        }
        m_danmakuList.erase(
            std::remove_if(m_danmakuList.begin(), m_danmakuList.end(), [](const DanmakuItem& i) { return !i.active; }),
            m_danmakuList.end());
    }

    void onPaint(HDC hdc) {
        if (!m_enabled) return;
        RECT rc = {0, 0, m_width, m_height};

        HBRUSH hbr = CreateSolidBrush(RGB(20, 20, 30));
        FillRect(m_memDC, &rc, hbr);
        DeleteObject(hbr);

        if (m_opacity > 10) {
            HBRUSH hbrBorder = CreateSolidBrush(RGB(80, 80, 100));
            FrameRect(m_memDC, &rc, hbrBorder);
            DeleteObject(hbrBorder);
        }

        HFONT hFont = CreateFontW(m_fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
        SelectObject(m_memDC, hFont);
        SetBkMode(m_memDC, TRANSPARENT);

        for (auto& item : m_danmakuList) {
            if (!item.active) continue;
            SetTextColor(m_memDC, item.color);
            TextOutW(m_memDC, (int)item.x, (int)item.y, item.text.c_str(), (int)item.text.length());
        }
        DeleteObject(hFont);
        BitBlt(hdc, 0, 0, m_width, m_height, m_memDC, 0, 0, SRCCOPY);
    }

    void setOpacity(BYTE alpha) {
        if (m_opacity != alpha) {
            m_opacity = alpha;
            if (m_hwnd) {
                SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
            }
        }
    }

    BYTE getOpacity() const { return m_opacity; }

    void resize(int width, int height) {
        if (m_width == width && m_height == height) return;
        m_width = width;
        m_height = height;
        if (m_memDC) { DeleteDC(m_memDC); DeleteObject(m_memBM); }
        HDC hdc = GetDC(m_hwnd);
        m_memDC = CreateCompatibleDC(hdc);
        m_memBM = CreateCompatibleBitmap(hdc, width, height);
        SelectObject(m_memDC, m_memBM);
        ReleaseDC(m_hwnd, hdc);
    }

    void setMargins(int topMargin, int bottomMargin) {
        m_topMargin = topMargin;
        m_bottomMargin = bottomMargin;
        m_height = bottomMargin - topMargin;
        if (m_height < 50) m_height = 200;
    }

    void setSpeed(float s) { m_config.baseSpeed = s; for (auto& i : m_danmakuList) i.speed = s; }
    void setFontSize(int size) { m_fontSize = size > 0 ? size : 24; }
    void setTrackCount(int count) {
        m_config.maxTracks = count > 0 ? count : 8;
        m_trackUsage.resize(m_config.maxTracks, 0);
    }
    void setEnabled(bool e) { m_enabled = e; }
    bool isEnabled() const { return m_enabled; }
    int getDanmakuCount() const { return (int)m_danmakuList.size(); }

private:
    int allocateTrack() {
        int track = 0, minUsage = m_trackUsage[0];
        for (int i = 1; i < (int)m_trackUsage.size(); ++i)
            if (m_trackUsage[i] < minUsage) { minUsage = m_trackUsage[i]; track = i; }
        m_trackUsage[track]++;
        return track;
    }

    HWND m_hwnd;
    HDC m_memDC;
    HBITMAP m_memBM;
    int m_width, m_height;
    bool m_enabled;
    int m_topMargin, m_bottomMargin;
    int m_fontSize;
    BYTE m_opacity;
    struct Config { int maxTracks; int maxDanmaku; float baseSpeed; } m_config;
    std::vector<DanmakuItem> m_danmakuList;
    std::vector<int> m_trackUsage;
};

static void updateOpacity() {
    DWORD now = GetTickCount();
    if (g_mouseOver) {
        g_lastActivity = now;
        if (g_engine) g_engine->setOpacity(220);
    } else if (now - g_lastActivity > (DWORD)g_fadeTimeout) {
        if (g_engine) g_engine->setOpacity(0);
    } else if (g_engine) {
        g_engine->setOpacity(50);
    }
}

static LRESULT CALLBACK DanmakuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static BOOL dragging = FALSE;
    static BOOL draggingRight = FALSE;
    static POINT dragStart;
    static int origLeft, origRight;

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_engine) g_engine->onPaint(hdc);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_TIMER:
        if (wParam == 1) {
            updateOpacity();
            if (g_engine) {
                g_engine->onTimer();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        break;
    case WM_MOUSEHOVER:
        g_mouseOver = TRUE;
        g_lastActivity = GetTickCount();
        break;
    case WM_MOUSELEAVE:
        g_mouseOver = FALSE;
        g_lastActivity = GetTickCount();
        break;
    case WM_LBUTTONDOWN: {
        POINT pt;
        GetCursorPos(&pt);
        RECT rc;
        GetWindowRect(hwnd, &rc);
        int relX = pt.x - rc.left;
        int width = rc.right - rc.left;

        if (relX < 10) {
            draggingRight = FALSE;
            dragging = TRUE;
            GetCursorPos(&dragStart);
            origLeft = g_leftMargin;
            origRight = g_rightMargin;
            SetCapture(hwnd);
        } else if (relX > width - 10) {
            draggingRight = TRUE;
            dragging = TRUE;
            GetCursorPos(&dragStart);
            origLeft = g_leftMargin;
            origRight = g_rightMargin;
            SetCapture(hwnd);
        } else {
            dragging = TRUE;
            draggingRight = FALSE;
            GetCursorPos(&dragStart);
            origLeft = g_leftMargin;
            origRight = g_rightMargin;
            SetCapture(hwnd);
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (dragging) {
            POINT pt;
            GetCursorPos(&pt);

            if (draggingRight) {
                int newRight = g_screenW - pt.x;
                if (newRight > 20 && newRight < g_screenW - 100) {
                    g_rightMargin = newRight;
                }
            } else {
                int newLeft = pt.x;
                if (newLeft >= 0 && newLeft < g_screenW - 100) {
                    g_leftMargin = newLeft;
                }
            }

            int newWidth = g_screenW - g_leftMargin - g_rightMargin;
            int danmakuH = g_bottomMargin - g_topMargin;
            int danmakuW = newWidth;

            SetWindowPos(g_danmakuHwnd, HWND_TOP,
                g_leftMargin, g_screenH - g_bottomMargin,
                danmakuW, danmakuH, SWP_NOACTIVATE);
            if (g_engine) {
                g_engine->resize(danmakuW, danmakuH);
            }
            printf("[DRAG] left=%d, right=%d, width=%d\n", g_leftMargin, g_rightMargin, danmakuW);
        }
        break;
    }
    case WM_LBUTTONUP:
        dragging = FALSE;
        draggingRight = FALSE;
        ReleaseCapture();
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY && wParam == 1) {
        printf("[HOTKEY] Ctrl+Q pressed, exiting...\n");
        DestroyWindow(g_mainHwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &g_wa, 0);
        g_screenW = g_wa.right - g_wa.left;
        g_screenH = g_wa.bottom - g_wa.top;

        int danmakuW = (int)(g_screenW * 0.8);
        g_leftMargin = (g_screenW - danmakuW) / 2;
        g_rightMargin = g_screenW - g_leftMargin - danmakuW;

        int danmakuH = (g_bottomMargin > g_topMargin) ? (g_bottomMargin - g_topMargin) : 200;
        if (danmakuH < 50) danmakuH = 200;

        WNDCLASSEXW wcex = {};
        wcex.cbSize = sizeof(WNDCLASSEXW);
        wcex.lpfnWndProc = DanmakuWndProc;
        wcex.hInstance = GetModuleHandle(nullptr);
        wcex.lpszClassName = L"DanmakuWindow";
        wcex.hbrBackground = nullptr;
        RegisterClassExW(&wcex);

        g_danmakuHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST,
            L"DanmakuWindow", L"Danmaku",
            WS_POPUP | WS_VISIBLE,
            g_leftMargin, g_screenH - g_bottomMargin, danmakuW, danmakuH,
            hwnd, nullptr, GetModuleHandle(nullptr), nullptr);

        SetLayeredWindowAttributes(g_danmakuHwnd, 0, 0, LWA_ALPHA);

        g_engine = new DanmakuEngine();
        g_engine->init(g_danmakuHwnd, danmakuW, danmakuH);
        g_engine->setMargins(g_topMargin, g_bottomMargin);
        g_engine->setSpeed(2.5f);
        g_engine->setFontSize(g_fontSize);
        g_engine->setTrackCount(g_trackCount);

        printf("[APP] Danmaku window: x=%d, y=%d, w=%d, h=%d\n",
               g_leftMargin, g_screenH - g_bottomMargin, danmakuW, danmakuH);
        printf("[APP] Drag left/right edge to resize horizontally.\n");
        printf("[APP] Drag center to move vertically.\n");
        printf("[APP] Hover to show frosted glass, leave to fade.\n");
        printf("[APP] Press Ctrl+Q to exit.\n");

        SetTimer(g_danmakuHwnd, 1, 16, nullptr);

        printf("[APP] Starting API fetch...\n");

        std::vector<TestSongResult> songs;
        if (searchSong(L"晴天", songs) && !songs.empty()) {
            printf("[APP] Found song: %ws, ID: %s\n", songs[0].songName.c_str(), songs[0].songId.c_str());
            // Use comments already loaded by searchSong via Python SDK
            if (!g_tempComments.empty()) {
                printf("[APP] Got %d comments from Python SDK, adding to danmaku...\n", (int)g_tempComments.size());
                for (auto& c : g_tempComments) {
                    COLORREF color = RGB(255,255,255);
                    if (c.likeCount > 1000) color = RGB(255,100,100);
                    else if (c.likeCount > 100) color = RGB(255,200,100);
                    g_engine->addDanmaku(c.content, color);
                }
                g_tempComments.clear();
            } else {
                printf("[APP] No comments loaded\n");
            }
        } else {
            printf("[APP] searchSong returned false or no results\n");
            g_engine->addDanmaku(L"API search failed - default danmaku", RGB(255,100,100));
            g_engine->addDanmaku(L"API test - default comments", RGB(100,255,200));
        }

        printf("[APP] Total danmaku count: %d\n", g_engine->getDanmakuCount());
        break;
    }
    case WM_DESTROY:
        UnregisterHotKey(g_msgHwnd, 1);
        if (g_danmakuHwnd) { KillTimer(g_danmakuHwnd, 1); DestroyWindow(g_danmakuHwnd); }
        delete g_engine;
        g_engine = nullptr;
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void parseCommandLine(LPCSTR lpCmdLine) {
    g_topMargin = 0;
    g_bottomMargin = 200;
    g_fontSize = 24;
    g_trackCount = 8;

    if (!lpCmdLine || !*lpCmdLine) return;

    int args[4] = {0, 0, 0, 0};
    int idx = 0;
    char* cmd = _strdup(lpCmdLine);
    char* ctx;
    char* p = strtok_s(cmd, " \t", &ctx);
    while (p && idx < 4) {
        args[idx++] = atoi(p);
        p = strtok_s(nullptr, " \t", &ctx);
    }
    free(cmd);

    if (args[0] >= 0) g_topMargin = args[0];
    if (args[1] > 0) g_bottomMargin = args[1];
    if (args[2] > 0) g_fontSize = args[2];
    if (args[3] > 0) g_trackCount = args[3];
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    parseCommandLine(lpCmdLine);

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.lpfnWndProc = MsgWndProc;
    wcex.hInstance = hInst;
    wcex.lpszClassName = L"DanmakuMsg";
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wcex);

    g_msgHwnd = CreateWindowExW(0, L"DanmakuMsg", L"DanmakuHotkey",
        WS_DISABLED,
        0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);

    RegisterHotKey(g_msgHwnd, 1, MOD_CONTROL, 'Q');

    wcex.lpfnWndProc = MainWndProc;
    wcex.lpszClassName = L"DanmakuTestApp";
    wcex.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));

    RegisterClassExW(&wcex);

    HWND hwnd = CreateWindowW(L"DanmakuTestApp", L"Danmaku Test",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 200,
        nullptr, nullptr, hInst, nullptr);

    g_mainHwnd = hwnd;
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_msgHwnd, 1);
    DestroyWindow(g_msgHwnd);

    return (int)msg.wParam;
}