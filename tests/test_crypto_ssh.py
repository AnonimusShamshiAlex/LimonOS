#!/usr/bin/env python3
"""Сверяет crypto.c (SHA-512, Ed25519-проверка, ChaCha20/Poly1305, AES-CTR) с независимыми реализациями (hashlib, cryptography)."""
import subprocess, os, sys, hashlib
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import poly1305
T = "/tmp/limon_tests"; os.makedirs(T, exist_ok=True)
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
subprocess.check_call(["gcc", "-O2", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-I", root + "/kernel", root + "/tests/crypto_ssh_host.c", root + "/kernel/crypto.c", root + "/kernel/string.c", "-o", T + "/csh"])
def run(*a): return subprocess.check_output([T + "/csh", *a], env={"ASAN_OPTIONS": "detect_leaks=0"}).decode().strip()
bad = 0
def check(name, ok):
    global bad
    if not ok: bad += 1; print("  FAIL", name)
for n in list(range(0, 300, 7)) + [111, 112, 113, 127, 128, 129, 255, 256, 1000, 5000]:
    d = os.urandom(n); check("sha512 %d" % n, run("sha512", d.hex() or "") == hashlib.sha512(d).hexdigest()) if n else check("sha512 0", run("sha512", "") == hashlib.sha512(b"").hexdigest())
print("sha512 ok")
cnt = 0
for i in range(40):
    k = Ed25519PrivateKey.generate(); pk = k.public_key().public_bytes_raw()
    m = os.urandom(i * 5 + 1); s = k.sign(m)
    check("ed ok %d" % i, run("ed25519", s.hex(), pk.hex(), m.hex()) == "0")
    for j in (0, 17, 33, 63):   # порча подписи
        sb = bytearray(s); sb[j] ^= 1 << (i % 8)
        check("ed bad sig %d/%d" % (i, j), run("ed25519", bytes(sb).hex(), pk.hex(), m.hex()) != "0")
    check("ed bad msg", run("ed25519", s.hex(), pk.hex(), (m + b"x").hex()) != "0")
    pkb = bytearray(pk); pkb[5] ^= 4
    check("ed bad pk", run("ed25519", s.hex(), bytes(pkb).hex(), m.hex()) != "0")
    # неканоническое S (S+L) должно отвергаться
    L = 2**252 + 27742317777372353535851937790883648493
    S = int.from_bytes(s[32:], "little") + L
    if S < 2**256:
        check("ed S+L", run("ed25519", (s[:32] + S.to_bytes(32, "little")).hex(), pk.hex(), m.hex()) != "0")
    cnt += 1
# RFC 8032, тест 1 (пустое сообщение)
check("rfc8032 t1", run("ed25519", "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "") == "0")
print("ed25519 ok (%d ключей)" % cnt)
for n in (1, 63, 64, 65, 200):
    key = os.urandom(32); nonce = os.urandom(12); d = os.urandom(n); ctr = 1
    full = nonce  # cryptography: 16 байт = счётчик(4, LE) + нонс(12)
    exp = Cipher(algorithms.ChaCha20(key, ctr.to_bytes(4, "little") + nonce), mode=None).encryptor().update(d)
    check("chacha %d" % n, run("chacha", key.hex(), str(ctr), nonce.hex(), d.hex()) == exp.hex())
    pk = os.urandom(32)
    check("poly %d" % n, run("poly", pk.hex(), d.hex()) == poly1305.Poly1305.generate_tag(pk, d).hex())
    k16 = os.urandom(16); iv = os.urandom(16)
    iv = iv[:12] + b"\xff\xff\xff\xf0"            # заставим счётчик переносить в старшие байты
    exp = Cipher(algorithms.AES(k16), modes.CTR(iv)).encryptor().update(d)
    check("aesctr %d" % n, run("aesctr", k16.hex(), iv.hex(), d.hex()) == exp.hex())
print("chacha/poly/aes-ctr ok")
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
def pt(pub): return pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
for i in range(30):
    a = ec.generate_private_key(ec.SECP256R1()); b = ec.generate_private_key(ec.SECP256R1())
    ab = a.private_numbers().private_value.to_bytes(32, "big")
    r = run("p256pub", ab.hex()).split()
    check("p256 pub %d" % i, r[0] == "0" and r[1] == pt(a.public_key()).hex())
    r = run("p256ecdh", ab.hex(), pt(b.public_key()).hex()).split()
    check("p256 ecdh %d" % i, r[0] == "0" and r[1] == a.exchange(ec.ECDH(), b.public_key()).hex())
bp = bytearray(pt(b.public_key())); bp[40] ^= 1
check("p256 точка не на кривой отвергается", run("p256ecdh", ab.hex(), bytes(bp).hex()).split()[0] != "0")
check("p256 малый ключ 1", run("p256pub", "00"*31+"01").split()[1] == pt(ec.derive_private_key(1, ec.SECP256R1()).public_key()).hex())
check("p256 ключ n-1", run("p256pub", "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc63255" + "0").split()[1] == pt(ec.derive_private_key(ec.SECP256R1().key_size and 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550, ec.SECP256R1()).public_key()).hex())
print("p256 ok")
print("ПРОВАЛОВ:", bad); sys.exit(1 if bad else 0)
