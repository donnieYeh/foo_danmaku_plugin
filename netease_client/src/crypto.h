#pragma once
#include <string>

namespace netease {

/**
 * Encrypt a JSON payload with the NetEase weapi scheme:
 *   1. AES-128-CBC(json, PRESET_KEY) → base64  → enc1
 *   2. AES-128-CBC(enc1, random_key) → base64  → enc2   (= params)
 *   3. RSA_raw(reverse(random_key))             → hex    (= encSecKey)
 *
 * On success returns true and sets out_params / out_enc_sec_key.
 * On failure returns false and sets error_msg.
 */
bool encrypt_weapi(
    const std::string&  json_payload,
    std::string&        out_params,
    std::string&        out_enc_sec_key,
    std::wstring&       error_msg
);

} // namespace netease
