#ifndef TEST_DANMAKU_H
#define TEST_DANMAKU_H

#include <windows.h>
#include <string>
#include <vector>

struct TestComment {
    std::wstring content;
    std::wstring nickname;
    int likeCount;
};

struct TestSongResult {
    std::string songId;  // Changed to string to handle large IDs
    std::wstring songName;
    std::wstring artistName;
};

#endif // TEST_DANMAKU_H