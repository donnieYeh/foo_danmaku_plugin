"""weapi encryption: double AES-128-CBC + RSA(no padding).

Reverse-engineered from NetEase Cloud Music web frontend (core.js).
"""
from __future__ import annotations

import base64
import json
import secrets
from typing import Any, Mapping

from Crypto.Cipher import AES
from Crypto.Util.Padding import pad

# --- constants from NetEase core.js ---
_PRESET_KEY = b"0CoJUm6Qyw8W8jud"
_IV = b"0102305060708090"[:16]  # 实为 "0102030405060708"
_IV = b"0102030405060708"
_PUBKEY = "010001"
_MODULUS = (
    "00e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725"
    "152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312"
    "ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424"
    "d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7"
)
_HEX = "0123456789abcdef"


def _aes_encrypt(text: bytes, key: bytes) -> str:
    cipher = AES.new(key, AES.MODE_CBC, _IV)
    ct = cipher.encrypt(pad(text, AES.block_size))
    return base64.b64encode(ct).decode()


def _rsa_encrypt(text: str) -> str:
    """NetEase custom RSA: no padding, raw modpow over reversed plaintext."""
    reversed_text = text[::-1].encode()
    m = int(reversed_text.hex(), 16)
    e = int(_PUBKEY, 16)
    n = int(_MODULUS, 16)
    c = pow(m, e, n)
    return format(c, "x").zfill(256)


def _random_secret(length: int = 16) -> str:
    return "".join(secrets.choice(_HEX) for _ in range(length))


def encrypt_weapi(payload: Mapping[str, Any]) -> dict[str, str]:
    """Encrypt a JSON payload for /weapi/* endpoints.

    Returns dict with 'params' and 'encSecKey' to be POSTed as form-urlencoded.
    """
    text = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode()
    sec_key = _random_secret()
    enc1 = _aes_encrypt(text, _PRESET_KEY)
    enc2 = _aes_encrypt(enc1.encode(), sec_key.encode())
    enc_sec_key = _rsa_encrypt(sec_key)
    return {"params": enc2, "encSecKey": enc_sec_key}
