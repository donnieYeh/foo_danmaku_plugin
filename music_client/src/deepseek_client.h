#ifndef DEEPSEEK_CLIENT_H
#define DEEPSEEK_CLIENT_H

#include <string>

#include <vector>

struct DeepSeekSongVariant {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
};

struct DeepSeekCleanResult {
    std::vector<DeepSeekSongVariant> variants;
};

void deepseek_set_api_key(const char* api_key);
bool deepseek_has_api_key();
bool deepseek_clean_metadata(
    const std::wstring& q_title,
    const std::wstring& q_artist,
    const std::wstring& q_album,
    DeepSeekCleanResult& out_result,
    std::wstring&       error_msg);

void deepseek_clear_cache();

#endif // DEEPSEEK_CLIENT_H
