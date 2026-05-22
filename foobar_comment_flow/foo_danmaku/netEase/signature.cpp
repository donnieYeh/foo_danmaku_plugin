#include "signature.h"
#include <windows.h>
#include <wincrypt.h>
#include <cstring>

#pragma comment(lib, "advapi32.lib")

std::string Signature::md5(const std::string& input) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE rgbHash[16];
    DWORD hashLen = sizeof(rgbHash);

    CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash);
    CryptHashData(hHash, (BYTE*)input.c_str(), input.length(), 0);
    CryptGetHashParam(hHash, HP_HASHVAL, rgbHash, &hashLen, 0);

    char hex[33] = {0};
    for (int i = 0; i < 16; i++) {
        sprintf_s(hex + i * 2, 33 - i * 2, "%02x", rgbHash[i]);
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    return std::string(hex);
}

std::string Signature::generateNonce(int length) {
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string result;
    result.reserve(length);

    for (int i = 0; i < length; i++) {
        result += chars[rand() % (sizeof(chars) - 1)];
    }
    return result;
}

std::string Signature::encrypt(const std::string& data) {
    // 163 box algorithm implementation
    // This is a simplified version - the actual algorithm requires JS execution
    // For production, consider using WebView2 to get proper signed requests

    std::string nonce = generateNonce(16);
    std::string combined = data + nonce;
    return md5(combined);
}
