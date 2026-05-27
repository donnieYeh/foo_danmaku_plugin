/* crypto.cpp — NetEase weapi encryption
 *
 * Ported from:
 *   netease_comments/src/netease_comments/crypto.py
 *
 * Algorithm (from reverse-engineered core.js):
 *   sec_key  = random 16 lowercase hex chars
 *   enc1     = Base64( AES-CBC( utf8_payload, PRESET_KEY, IV ) )
 *   enc2     = Base64( AES-CBC( enc1,          sec_key,   IV ) )
 *   enc_skey = Hex( RSA_noPad( reverse(sec_key), e, n ) )
 *   POST body: params=<enc2>&encSecKey=<enc_skey>
 *
 * Windows APIs used:
 *   BCrypt  — AES-128-CBC, random bytes
 *   (RSA modpow implemented manually to avoid BCrypt padding constraints)
 */

#include "crypto.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <algorithm>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "bcrypt.lib")

namespace netease {

/* ── hardcoded constants from NetEase core.js ───────── */

static const uint8_t kPresetKey[16] = {
    '0','C','o','J','U','m','6','Q','y','w','8','W','8','j','u','d'
};
static const uint8_t kIV[16] = {
    '0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8'
};

/* RSA public exponent = 0x10001 */
static const uint8_t kPubExp[3] = { 0x01, 0x00, 0x01 };

/* RSA 1024-bit modulus (128 bytes, big-endian, leading 00 stripped).
 * Source: _MODULUS in crypto.py, "00e0b509..." → strip leading "00". */
static const uint8_t kModulus[128] = {
    0xe0,0xb5,0x09,0xf6,0x25,0x9d,0xf8,0x64,0x2d,0xbc,0x35,0x66,0x29,0x01,0x47,0x7d,
    0xf2,0x26,0x77,0xec,0x15,0x2b,0x5f,0xf6,0x8a,0xce,0x61,0x5b,0xb7,0xb7,0x25,0x15,
    0x2b,0x3a,0xb1,0x7a,0x87,0x6a,0xea,0x8a,0x5a,0xa7,0x6d,0x2e,0x41,0x76,0x29,0xec,
    0x4e,0xe3,0x41,0xf5,0x61,0x35,0xfc,0xcf,0x69,0x52,0x80,0x10,0x4e,0x03,0x12,0xec,
    0xbd,0xa9,0x25,0x57,0xc9,0x38,0x70,0x11,0x4a,0xf6,0xc9,0xd0,0x5c,0x4f,0x7f,0x0c,
    0x36,0x85,0xb7,0xa4,0x6b,0xee,0x25,0x59,0x32,0x57,0x5c,0xce,0x10,0xb4,0x24,0xd8,
    0x13,0xcf,0xe4,0x87,0x5d,0x3e,0x82,0x04,0x7b,0x97,0xdd,0xef,0x52,0x74,0x1d,0x54,
    0x6b,0x8e,0x28,0x9d,0xc6,0x93,0x5b,0x3e,0xce,0x04,0x62,0xdb,0x0a,0x22,0xb8,0xe7
};

/* ── base64 encoder (standard alphabet, no line breaks) ─ */

static const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i+1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i+2];

        out += kB64Chars[(b >> 18) & 0x3F];
        out += kB64Chars[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? kB64Chars[(b >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kB64Chars[(b     ) & 0x3F] : '=';
    }
    return out;
}

/* ── AES-128-CBC encrypt (BCrypt, PKCS7 padding) ─────── */

static bool aes_cbc_encrypt(
    const uint8_t* key,   /* 16 bytes */
    const uint8_t* iv,    /* 16 bytes */
    const uint8_t* plain, size_t plain_len,
    std::vector<uint8_t>& cipher,
    std::wstring& err)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ok = false;

    NTSTATUS s = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(s)) { err = L"BCryptOpenAlgorithmProvider failed"; goto done; }

    s = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
        (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    if (!BCRYPT_SUCCESS(s)) { err = L"BCryptSetProperty(CBC) failed"; goto done; }

    /* key object size query */
    DWORD keyObjSz = 0, dummy = 0;
    s = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&keyObjSz, sizeof(keyObjSz), &dummy, 0);
    if (!BCRYPT_SUCCESS(s)) { err = L"BCryptGetProperty failed"; goto done; }

    {
        std::vector<uint8_t> keyObj(keyObjSz);
        s = BCryptGenerateSymmetricKey(hAlg, &hKey,
            keyObj.data(), keyObjSz, (PUCHAR)key, 16, 0);
        if (!BCRYPT_SUCCESS(s)) { err = L"BCryptGenerateSymmetricKey failed"; goto done; }

        /* IV is consumed in-place — pass a writable copy */
        uint8_t ivCopy[16];
        memcpy(ivCopy, iv, 16);

        ULONG outLen = 0;
        s = BCryptEncrypt(hKey, (PUCHAR)plain, (ULONG)plain_len,
            nullptr, ivCopy, 16, nullptr, 0, &outLen, BCRYPT_BLOCK_PADDING);
        if (!BCRYPT_SUCCESS(s)) { err = L"BCryptEncrypt size query failed"; goto done; }

        cipher.resize(outLen);
        memcpy(ivCopy, iv, 16); /* reset IV for actual call */
        s = BCryptEncrypt(hKey, (PUCHAR)plain, (ULONG)plain_len,
            nullptr, ivCopy, 16, cipher.data(), outLen, &outLen, BCRYPT_BLOCK_PADDING);
        if (!BCRYPT_SUCCESS(s)) { err = L"BCryptEncrypt failed"; goto done; }
        cipher.resize(outLen);
        ok = true;
    }

done:
    if (hKey)  BCryptDestroyKey(hKey);
    if (hAlg)  BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

/* ── 1024-bit big integer (little-endian uint32 array) ── */
/* Represents a non-negative integer up to 2^1024 - 1.    */

typedef uint32_t u32;
typedef uint64_t u64;

struct BigInt1024 {
    u32 d[32]; /* d[0] = LSW */
    BigInt1024() { memset(d, 0, sizeof(d)); }
};

struct BigInt2048 {
    u32 d[64];
    BigInt2048() { memset(d, 0, sizeof(d)); }
};

/* Compare: -1 / 0 / +1 */
static int bi_cmp(const BigInt1024& a, const BigInt1024& b) {
    for (int i = 31; i >= 0; --i) {
        if (a.d[i] > b.d[i]) return  1;
        if (a.d[i] < b.d[i]) return -1;
    }
    return 0;
}

/* a -= b  (assumes a >= b) */
static void bi_sub_inplace(BigInt1024& a, const BigInt1024& b) {
    u64 borrow = 0;
    for (int i = 0; i < 32; ++i) {
        u64 diff = (u64)a.d[i] - b.d[i] - borrow;
        a.d[i] = (u32)diff;
        borrow = (diff >> 63) & 1;
    }
}

/* result = a * b (1024 × 1024 → 2048) */
static BigInt2048 bi_mul(const BigInt1024& a, const BigInt1024& b) {
    BigInt2048 r;
    for (int i = 0; i < 32; ++i) {
        u64 carry = 0;
        for (int j = 0; j < 32; ++j) {
            u64 cur = (u64)a.d[i] * b.d[j] + r.d[i+j] + carry;
            r.d[i+j] = (u32)cur;
            carry = cur >> 32;
        }
        r.d[i+32] += (u32)carry;
    }
    return r;
}

/* Reduce 2048-bit number mod 1024-bit n → fits in 1024 bits.
 * Uses repeated bit-shift subtraction (schoolbook; acceptable for 17 calls). */
static BigInt1024 bi_mod2048(const BigInt2048& a, const BigInt1024& n) {
    /* Work with a copy in 2048 bits */
    BigInt2048 r = a;

    /* Find highest set bit of n in position [0..1023] */
    /* n fits in 1024 bits by construction (MSB of n[31] may be set) */

    /* Shift n left until it aligns with the top of r */
    /* We do binary long division: for shift = 1023 down to 0,
     * if r >= (n << shift), subtract. */

    /* Build a shifted version of n as BigInt2048 */
    for (int shift = 1023; shift >= 0; --shift) {
        /* Build n_shifted = n << shift (only in the 2048 range) */
        BigInt2048 ns;
        int word_shift = shift / 32;
        int bit_shift  = shift % 32;
        for (int i = 0; i < 32; ++i) {
            int dst = i + word_shift;
            if (dst < 64) {
                ns.d[dst] |= (u32)(n.d[i] << bit_shift);
            }
            if (bit_shift > 0 && dst + 1 < 64) {
                ns.d[dst+1] |= (u32)(n.d[i] >> (32 - bit_shift));
            }
        }

        /* Compare r >= ns  (init true: if all words equal, r==ns → subtract) */
        bool ge = true;
        for (int i = 63; i >= 0; --i) {
            if (r.d[i] > ns.d[i]) { ge = true;  break; }
            if (r.d[i] < ns.d[i]) { ge = false; break; }
        }
        if (ge) {
            /* r -= ns */
            u64 borrow = 0;
            for (int i = 0; i < 64; ++i) {
                u64 diff = (u64)r.d[i] - ns.d[i] - borrow;
                r.d[i] = (u32)diff;
                borrow = (diff >> 63) & 1;
            }
        }
    }

    BigInt1024 result;
    memcpy(result.d, r.d, sizeof(result.d));
    return result;
}

/* Modular multiply: (a * b) mod n */
static BigInt1024 bi_mulmod(const BigInt1024& a, const BigInt1024& b,
                            const BigInt1024& n)
{
    return bi_mod2048(bi_mul(a, b), n);
}

/* Modular exponentiation: a^e mod n, e = 65537 = 2^16 + 1 */
static BigInt1024 bi_powmod_65537(const BigInt1024& m, const BigInt1024& n) {
    BigInt1024 r = m;                 /* r = m^1 */
    for (int i = 0; i < 16; ++i)
        r = bi_mulmod(r, r, n);       /* r = m^(2^16) */
    r = bi_mulmod(r, m, n);           /* r = m^(2^16 + 1) = m^65537 */
    return r;
}

/* ── RSA encrypt (no padding, custom NetEase scheme) ──── */
/* Mirrors: c = pow(int(reversed_key_bytes), 0x10001, n)  */

static std::string rsa_encrypt_sec_key(const std::string& sec_key /* 16 ASCII */) {
    /* Step 1: reverse sec_key bytes */
    std::string rev(sec_key.rbegin(), sec_key.rend());

    /* Step 2: load as big-endian integer.
     * rev is 16 bytes: rev[0] is the MOST significant byte.
     * Mirrors Python: int.from_bytes(rev, 'big')
     * In the 1024-bit little-endian word array, rev[0] ends up in word 3 at
     * bit 24 (the highest byte of word 3), rev[15] in word 0 at bit 0. */
    BigInt1024 m;
    for (int i = 0; i < 16; ++i) {
        int byte_pos = 15 - i; /* 0-based from LSB end; rev[0]=MSB → byte_pos=15 */
        int word = byte_pos / 4;
        int bit  = (byte_pos % 4) * 8;
        m.d[word] |= ((u32)(uint8_t)rev[i]) << bit;
    }

    /* Step 3: load modulus */
    BigInt1024 n;
    for (int i = 0; i < 128; ++i) {
        int byte_pos = 127 - i;
        int word = byte_pos / 4;
        int bit  = (byte_pos % 4) * 8;
        n.d[word] |= ((u32)kModulus[i]) << bit;
    }

    /* Step 4: modpow with e=65537 */
    BigInt1024 c = bi_powmod_65537(m, n);

    /* Step 5: output as 256 lowercase hex chars (big-endian) */
    std::string hex;
    hex.reserve(256);
    char buf[3];
    for (int i = 31; i >= 0; --i) {         /* word 31 = MSW */
        for (int b = 3; b >= 0; --b) {       /* byte 3 = MSB of word */
            uint8_t byte = (uint8_t)(c.d[i] >> (b * 8));
            snprintf(buf, sizeof(buf), "%02x", byte);
            hex += buf;
        }
    }
    return hex; /* always 256 chars */
}

/* ── random sec_key (16 lowercase hex chars) ─────────── */

static std::string make_sec_key() {
    static const char kHex[] = "0123456789abcdef";
    uint8_t raw[16];
    BCryptGenRandom(nullptr, raw, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string key;
    key.reserve(16);
    for (int i = 0; i < 16; ++i)
        key += kHex[raw[i] & 0xF];
    return key;
}

/* ── URL-encode (for POST body) ─────────────────────── */

static std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    static const char kSafe[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    char buf[4];
    for (unsigned char c : s) {
        if (strchr(kSafe, c)) {
            out += c;
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

/* ── public entry point ──────────────────────────────── */

bool encrypt_weapi(
    const std::string& json_payload,
    std::string&       out_params,
    std::string&       out_enc_sec_key,
    std::wstring&      error_msg)
{
    /* 1. AES( json, PRESET_KEY ) → base64 */
    std::vector<uint8_t> cipher1;
    if (!aes_cbc_encrypt(kPresetKey, kIV,
            (const uint8_t*)json_payload.data(), json_payload.size(),
            cipher1, error_msg))
        return false;

    std::string enc1 = base64_encode(cipher1.data(), cipher1.size());

    /* 2. random sec_key */
    std::string sec_key = make_sec_key();

    /* 3. AES( enc1, sec_key ) → base64 */
    std::vector<uint8_t> cipher2;
    if (!aes_cbc_encrypt((const uint8_t*)sec_key.data(), kIV,
            (const uint8_t*)enc1.data(), enc1.size(),
            cipher2, error_msg))
        return false;

    out_params      = url_encode(base64_encode(cipher2.data(), cipher2.size()));

    /* 4. RSA( reverse(sec_key) ) → hex */
    out_enc_sec_key = rsa_encrypt_sec_key(sec_key);

    return true;
}

} // namespace netease
