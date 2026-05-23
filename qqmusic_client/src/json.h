#pragma once
#include <string>
#include <vector>

namespace qqmusic {
namespace json {

/** Extract string value for a JSON key (handles \\ and \" escapes). */
std::string str(const std::string& json, const std::string& key,
                const std::string& def = "");

/** Extract integer value for a JSON key. */
long long num(const std::string& json, const std::string& key,
              long long def = 0);

/** Extract the raw JSON array string for a key (the "[…]" portion). */
std::string array_raw(const std::string& json, const std::string& key);

/** Extract the raw JSON object string for a key (the "{…}" portion). */
std::string object_raw(const std::string& json, const std::string& key);

/** Split a JSON array string into individual object strings. */
std::vector<std::string> array_items(const std::string& array_json);

/** UTF-8 → UTF-16 */
std::wstring to_wide(const std::string& utf8);

/** UTF-16 → UTF-8 */
std::string to_utf8(const std::wstring& wide);

} // namespace json
} // namespace qqmusic
