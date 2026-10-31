#!/usr/bin/env python3
"""Проверка криптографии LimonOS против эталонных библиотек Python (hashlib, cryptography).
Запуск:  python3 tests/test_crypto.py   (из корня проекта)"""
import ctypes, os, subprocess, hashlib, hmac, sys
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305, AESGCM
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives import serialization

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
so = "/tmp/libcrypto_limon.so"
subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-I", root + "/kernel", root + "/kernel/crypto.c", "-o", so])
L = ctypes.CDLL(so)
def buf(b): return (ctypes.c_uint8 * max(len(b), 1)).from_buffer_copy(b or b"\0")
fails = 0
def check(name, ok):
    global fails
    print(("  ok   " if ok else "  FAIL ") + name)
    if not ok: fails += 1

# SHA-256 (разные длины, включая границы блока)
good = True
for n in list(range(0, 130)) + [1000, 4096, 65537]:
    d = os.urandom(n); out = (ctypes.c_uint8 * 32)()
    L.sha256(buf(d), ctypes.c_size_t(n), out)
    good &= bytes(out) == hashlib.sha256(d).digest()
check("SHA-256", good)

# HMAC
good = True
for kl in (0, 5, 32, 64, 100):
    k = os.urandom(kl); d = os.urandom(77); out = (ctypes.c_uint8 * 32)()
    L.hmac_sha256(buf(k), ctypes.c_size_t(kl), buf(d), ctypes.c_size_t(77), out)
    good &= bytes(out) == hmac.new(k, d, hashlib.sha256).digest()
check("HMAC-SHA256", good)

# HKDF (RFC 5869, тест 1)
ikm = bytes([0x0b]) * 22; salt = bytes.fromhex("000102030405060708090a0b0c"); info = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9")
prk = (ctypes.c_uint8 * 32)(); okm = (ctypes.c_uint8 * 42)()
L.hkdf_extract(buf(salt), 13, buf(ikm), 22, prk)
L.hkdf_expand(prk, buf(info), 10, okm, 42)
check("HKDF RFC5869", bytes(okm).hex() == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865")

# TLS 1.3 key schedule: RFC 8448 (Simple 1-RTT), early secret = HKDF-Extract(0, 0)
early = (ctypes.c_uint8 * 32)(); L.hkdf_extract(None, 0, buf(b"\0" * 32), 32, early)
check("early secret (RFC 8448)", bytes(early).hex() == "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a")
empty = hashlib.sha256(b"").digest(); derived = (ctypes.c_uint8 * 32)()
L.tls13_expand_label(early, b"derived", buf(empty), 32, derived, 32)
check("derived secret (RFC 8448)", bytes(derived).hex() == "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba")

# ChaCha20-Poly1305
good = True
for n in list(range(0, 70)) + [1000, 16384]:
    key = os.urandom(32); nonce = os.urandom(12); aad = os.urandom(n % 23); pt = os.urandom(n)
    ref = ChaCha20Poly1305(key).encrypt(nonce, pt, aad)
    ct = (ctypes.c_uint8 * max(n, 1))(); tag = (ctypes.c_uint8 * 16)()
    L.chacha20poly1305_seal(buf(key), buf(nonce), buf(aad), len(aad), buf(pt), n, ct, tag)
    good &= (bytes(ct)[:n] + bytes(tag)) == ref
    back = (ctypes.c_uint8 * max(n, 1))()
    good &= L.chacha20poly1305_open(buf(key), buf(nonce), buf(aad), len(aad), buf(ref[:n]), n, buf(ref[n:]), back) == 0
    good &= bytes(back)[:n] == pt
    bad = bytearray(ref); bad[-1] ^= 1
    good &= L.chacha20poly1305_open(buf(key), buf(nonce), buf(aad), len(aad), buf(bytes(bad[:n])), n, buf(bytes(bad[n:])), back) != 0
check("ChaCha20-Poly1305 (шифр, проверка, отказ при порче)", good)

# AES-128-GCM
good = True
for n in list(range(0, 70)) + [1000, 16384]:
    key = os.urandom(16); nonce = os.urandom(12); aad = os.urandom(n % 23); pt = os.urandom(n)
    ref = AESGCM(key).encrypt(nonce, pt, aad)
    ct = (ctypes.c_uint8 * max(n, 1))(); tag = (ctypes.c_uint8 * 16)()
    L.aes128gcm_seal(buf(key), buf(nonce), buf(aad), len(aad), buf(pt), n, ct, tag)
    good &= (bytes(ct)[:n] + bytes(tag)) == ref
    back = (ctypes.c_uint8 * max(n, 1))()
    good &= L.aes128gcm_open(buf(key), buf(nonce), buf(aad), len(aad), buf(ref[:n]), n, buf(ref[n:]), back) == 0
    good &= bytes(back)[:n] == pt
    bad = bytearray(ref); bad[0] ^= 1
    good &= L.aes128gcm_open(buf(key), buf(nonce), buf(aad), len(aad), buf(bytes(bad[:n])), n, buf(bytes(bad[n:])), back) != 0 if n else True
check("AES-128-GCM (шифр, проверка, отказ при порче)", good)

# X25519: вектор RFC 7748 и 50 случайных пар
a = bytes.fromhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4")
u = bytes.fromhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")
out = (ctypes.c_uint8 * 32)(); L.x25519(out, buf(a), buf(u))
check("X25519 RFC 7748", bytes(out).hex() == "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
good = True
for _ in range(50):
    sk = os.urandom(32); pk = X25519PrivateKey.generate().public_key()
    pkb = pk.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    L.x25519(out, buf(sk), buf(pkb))
    ref = X25519PrivateKey.from_private_bytes(sk).exchange(pk)
    good &= bytes(out) == ref
check("X25519 случайные пары (50)", good)

# RNG: не одинаковые, не нулевые
r1 = (ctypes.c_uint8 * 32)(); r2 = (ctypes.c_uint8 * 32)()
L.rng_bytes(r1, 32); L.rng_bytes(r2, 32)
check("RNG выдаёт разные значения", bytes(r1) != bytes(r2) and any(r1))
print("ПРОВАЛОВ:", fails); sys.exit(1 if fails else 0)
