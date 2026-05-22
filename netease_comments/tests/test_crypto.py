"""Sanity checks on crypto module (does not call network)."""
from netease_comments.crypto import encrypt_weapi


def test_encrypt_shape():
    out = encrypt_weapi({"hello": "world", "csrf_token": ""})
    assert set(out.keys()) == {"params", "encSecKey"}
    # encSecKey is 256 hex chars
    assert len(out["encSecKey"]) == 256
    assert all(ch in "0123456789abcdef" for ch in out["encSecKey"])
    # params is base64 (AES-CBC -> multiple of block)
    import base64
    raw = base64.b64decode(out["params"])
    assert len(raw) % 16 == 0


if __name__ == "__main__":
    test_encrypt_shape()
    print("crypto ok")
