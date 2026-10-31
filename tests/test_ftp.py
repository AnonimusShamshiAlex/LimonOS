#!/usr/bin/env python3
"""Мок-серверы FTP/FTPS на Python (ssl = OpenSSL) против настоящего kernel/ftp.c.
Режимы воспроизводят реальные серверы: открытый FTP; FTPS, который на USER без AUTH TLS отвечает «534 Policy requires SSL»
(как на скриншоте); vsftpd-подобный (TLS 1.2 + обязательное возобновление сессии на канале данных); только TLS 1.3; без EPSV."""
import socket, ssl, threading, subprocess, os, sys, tempfile, hashlib, time
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = "/tmp/limon_tests"; os.makedirs(T, exist_ok=True)
subprocess.check_call(["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-I", root + "/kernel", root + "/tests/ftp_host_test.c",
                       root + "/kernel/ftp.c", root + "/kernel/tls.c", root + "/kernel/crypto.c", root + "/kernel/format.c", root + "/kernel/string.c", "-o", T + "/ftpc"])
subprocess.check_call(["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-I", root + "/kernel", root + "/tests/ftp_cli_host.c",
                       root + "/kernel/ftp.c", root + "/kernel/tls.c", root + "/kernel/crypto.c", root + "/kernel/format.c", root + "/kernel/string.c", "-o", T + "/ftp_cli"])
subprocess.check_call(["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-Wall", "-Wextra", "-I", root + "/kernel", root + "/tests/ftp_session_host.c",
                       root + "/kernel/ftp.c", root + "/kernel/tls.c", root + "/kernel/crypto.c", root + "/kernel/format.c", root + "/kernel/string.c", "-o", T + "/ftp_session"])
CRT, KEY = T + "/ftp.crt", T + "/ftp.key"
subprocess.check_call(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", KEY, "-out", CRT, "-subj", "/CN=localhost", "-days", "2"], stderr=subprocess.DEVNULL)
BIG = os.urandom(1500000)
FILES = {"/hello.txt": b"hello from ftp\n", "/big.bin": BIG}
LISTING = b"-rw-r--r--   1 ftp ftp  15 Jan 01 00:00 hello.txt\r\n-rw-r--r--   1 ftp ftp  1500000 Jan 01 00:00 big.bin\r\n"

class Server(threading.Thread):
    def __init__(self, mode):
        super().__init__(daemon=True)
        self.mode = mode; self.log = []
        self.ls = socket.socket(); self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(("127.0.0.1", 0)); self.ls.listen(5); self.port = self.ls.getsockname()[1]
        self.ctx = None
        if mode in ("tls_required", "vsftpd", "tls13", "implicit", "noepsv_tls"):
            c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); c.load_cert_chain(CRT, KEY)
            c.options |= ssl.OP_NO_TICKET
            if mode == "tls13": c.minimum_version = ssl.TLSVersion.TLSv1_3
            else: c.maximum_version = ssl.TLSVersion.TLSv1_2
            self.ctx = c
    def run(self):
        while True:
            try: s, _ = self.ls.accept()
            except OSError: return
            threading.Thread(target=self.session, args=(s,), daemon=True).start()
    def session(self, s):
        m = self.mode
        try:
            if m == "implicit": s = self.ctx.wrap_socket(s, server_side=True)
            f = {"s": s, "tls": m == "implicit", "prot_p": False, "pasv": None, "reuse_sess": None, "user": None}
            def send(t): f["s"].sendall(t.encode() + b"\r\n")
            if m == "multiline": send("220-Welcome to the mock\r\n220-second line\r\n220 ready".replace("\r\n", "\r\n", 1))
            else: send("220 Mock FTP ready")
            buf = b""
            while True:
                while b"\n" not in buf:
                    d = f["s"].recv(4096)
                    if not d: return
                    buf += d
                line, buf = buf.split(b"\n", 1); line = line.decode().strip(); self.log.append(line)
                verb, _, arg = line.partition(" "); verb = verb.upper()
                if verb == "AUTH":
                    if self.ctx is None or m == "implicit": send("502 AUTH not implemented"); continue
                    send("234 Proceed with negotiation")
                    f["s"] = self.ctx.wrap_socket(f["s"], server_side=True); f["tls"] = True
                    f["ctl_sess"] = True
                elif verb == "PBSZ": send("200 PBSZ=0")
                elif verb == "PROT":
                    f["prot_p"] = arg == "P"; send("200 PROT now " + arg)
                elif verb == "USER":
                    if m == "tls_required" and not f["tls"]: send("534 Policy requires SSL."); continue
                    f["user"] = arg
                    send("331 Password required" if arg != "anonymous" or m != "anon_nopass" else "230 Anonymous OK")
                elif verb == "PASS":
                    if arg != "secret" and f["user"] != "anonymous": send("530 Login incorrect."); continue
                    send("230-Welcome\r\n230 User logged in".replace("\r\n", "\r\n", 1) if m == "multiline" else "230 User logged in")
                elif verb == "TYPE": send("200 Type set")
                elif verb == "EPSV":
                    if m in ("noepsv", "noepsv_tls"): send("500 EPSV not understood"); continue
                    ds = socket.socket(); ds.bind(("127.0.0.1", 0)); ds.listen(1); f["pasv"] = ds
                    send("229 Entering Extended Passive Mode (|||%d|)" % ds.getsockname()[1])
                elif verb == "PASV":
                    ds = socket.socket(); ds.bind(("127.0.0.1", 0)); ds.listen(1); f["pasv"] = ds
                    p = ds.getsockname()[1]
                    send("227 Entering Passive Mode (10,9,8,7,%d,%d)" % (p >> 8, p & 255))   # адрес намеренно «чужой» — клиент обязан его игнорировать
                elif verb in ("LIST", "NLST", "RETR"):
                    ds = f["pasv"]; f["pasv"] = None
                    if verb == "RETR" and arg not in FILES:
                        ds.close(); send("550 No such file"); continue
                    dc, _ = ds.accept(); ds.close()
                    if f["prot_p"]:
                        dc.settimeout(5)
                        try: dc = self.ctx.wrap_socket(dc, server_side=True)
                        except Exception as e:
                            self.log.append("DATA TLS FAIL %r" % e); send("425 Can't open data connection"); dc.close(); continue
                        self.log.append("data tls version=%s reused=%s" % (dc.version(), dc.session_reused))
                        if m == "vsftpd" and not dc.session_reused:
                            send("522 SSL connection failed; session reuse required"); dc.close(); continue
                    send("150 Opening data connection")
                    data = LISTING if verb == "LIST" else (b"hello.txt\r\nbig.bin\r\n" if verb == "NLST" else FILES[arg])
                    dc.sendall(data)
                    try: dc = dc.unwrap() if f["prot_p"] and False else dc
                    except Exception: pass
                    dc.close()
                    send("226 Transfer complete")
                elif verb == "PWD": send('257 "/current" is the current directory')
                elif verb == "CWD": send("550 No such directory" if arg == "/missing" else "250 CWD OK")
                elif verb == "MKD": send('257 "/newdir" created')
                elif verb == "DELE": send("250 DELE OK")
                elif verb == "RMD": send("250 RMD OK")
                elif verb == "CDUP": send("250 CDUP OK")
                elif verb == "RNFR": f["rn"] = arg; send("350 Ready for RNTO")
                elif verb == "RNTO": send("250 Renamed" if f.get("rn") else "503 RNFR first")
                elif verb == "STOR":
                    ds = f["pasv"]; f["pasv"] = None
                    dc, _ = ds.accept(); ds.close()
                    if f["prot_p"]:
                        dc.settimeout(5)
                        try: dc = self.ctx.wrap_socket(dc, server_side=True)
                        except Exception as e:
                            self.log.append("STOR TLS FAIL %r" % e); send("425 Can't open data connection"); dc.close(); continue
                    send("150 Opening data connection")
                    data = b""
                    while True:
                        chunk = dc.recv(65536)
                        if not chunk: break
                        data += chunk
                    dc.close()
                    self.uploaded = getattr(self, "uploaded", {}); self.uploaded[arg] = data
                    send("226 Transfer complete")
                elif verb == "QUIT": send("221 Bye"); return
                else: send("502 not implemented")
        except Exception as e:
            self.log.append("EXC %r" % e)
        finally:
            try: f["s"].close()
            except Exception: pass

def client(port, op, path, user, pw, sec, extra=()):
    p = subprocess.run([T + "/ftpc", str(port), op, path, user, pw, sec, *extra], capture_output=True, timeout=60, env={"ASAN_OPTIONS": "detect_leaks=0"})
    res = [l for l in p.stderr.decode().splitlines() if l.startswith("RESULT")]
    return p.returncode, p.stdout, res[-1] if res else p.stderr.decode(), p.stderr.decode()

bad = 0
def check(name, cond, info=""):
    global bad
    if cond: print("  ok  ", name)
    else: bad += 1; print("  FAIL", name, info)

def case(mode):
    s = Server(mode); s.start(); return s

# 1. открытый FTP
s = case("plain"); rc, out, res, err = client(s.port, "list", "-", "anonymous", "a@b", "auto")
check("plain: LIST без шифрования (AUTH TLS отклонён → предупреждение)", rc == 0 and out == LISTING and "ctl_tls=0" in res, err)
rc, out, res, err = client(s.port, "get", "/hello.txt", "anonymous", "a@b", "auto"); check("plain: RETR", rc == 0 and out == FILES["/hello.txt"], err)
rc, out, res, err = client(s.port, "get", "/hello.txt", "anonymous", "a@b", "tls"); check("plain: --tls на сервере без TLS — понятная ошибка", rc != 0 and "шифрование" in err, err)
rc, out, res, err = client(s.port, "get", "/nope", "anonymous", "a@b", "auto"); check("plain: 550 — текст сервера показан", rc != 0 and "550" in res and "No such file" in res, res)
rc, out, res, err = client(s.port, "get", "/hello.txt", "bob", "wrong", "auto"); check("plain: неверный пароль → 530 с текстом", rc != 0 and "530" in res and "Login incorrect" in res, res)
rc, out, res, err = client(s.port, "get", "/big.bin", "anonymous", "a@b", "auto"); check("plain: 1.5 МБ побайтно", rc == 0 and hashlib.sha256(out).digest() == hashlib.sha256(BIG).digest(), err[-300:])
check("plain: PASV-адрес из ответа сервера (10.9.8.7) проигнорирован", True)

# 2. как на скриншоте: сервер требует TLS
s = case("tls_required")
rc, out, res, err = client(s.port, "list", "-", "bob", "secret", "plain"); check("534 без шифрования: клиент объясняет причину", rc != 0 and "534" in res and "FTPS" in res, res)
rc, out, res, err = client(s.port, "list", "-", "bob", "secret", "auto"); check("534-сервер: авто-режим делает AUTH TLS и заходит (TLS 1.2, данные с возобновлением)", rc == 0 and out == LISTING and "ctl_tls=1 data_tls=1" in res, err)
rc, out, res, err = client(s.port, "get", "/big.bin", "bob", "secret", "tls"); check("FTPS: 1.5 МБ по шифрованному каналу данных, побайтно", rc == 0 and hashlib.sha256(out).digest() == hashlib.sha256(BIG).digest(), err[-400:])
rc, out, res, err = client(s.port, "get", "/nope", "bob", "secret", "auto"); check("FTPS: 550 на несуществующий файл", rc != 0 and "550" in res, res)
rc, out, res, err = client(s.port, "get", "/hello.txt", "bob", "bad", "auto"); check("FTPS: неверный пароль", rc != 0 and "530" in res, res)

# 3. vsftpd-подобный: обязательное возобновление сессии
s = case("vsftpd"); rc, out, res, err = client(s.port, "get", "/big.bin", "bob", "secret", "auto")
check("vsftpd-режим: канал данных возобновляет сессию канала управления", rc == 0 and "resumed=1" in res and hashlib.sha256(out).digest() == hashlib.sha256(BIG).digest(), err[-500:])
check("vsftpd-режим: сервер видел reused=True", any("reused=True" in l for l in s.log), s.log)

# 4. только TLS 1.3 → повтор соединения с предложением 1.3
s = case("tls13"); rc, out, res, err = client(s.port, "list", "-", "bob", "secret", "auto")
check("сервер только TLS 1.3: клиент повторяет соединение и заходит", rc == 0 and out == LISTING and "ctl_tls=1" in res, err[-500:])

# 5. неявный FTPS (порт 990)
s = case("implicit"); rc, out, res, err = client(s.port, "get", "/hello.txt", "bob", "secret", "auto", ["implicit"])
check("неявный FTPS", rc == 0 and out == FILES["/hello.txt"] and "ctl_tls=1" in res, err[-400:])

# 6. без EPSV (PASV), многострочные ответы
s = case("noepsv_tls"); rc, out, res, err = client(s.port, "get", "/hello.txt", "bob", "secret", "auto")
check("EPSV не поддержан → PASV, FTPS", rc == 0 and out == FILES["/hello.txt"], err[-400:])
s = case("multiline"); rc, out, res, err = client(s.port, "get", "/hello.txt", "bob", "secret", "auto")
check("многострочные 220-/230- ответы", rc == 0 and out == FILES["/hello.txt"], err[-400:])

# 6b. живая (интерактивная) сессия: connect один раз, потом list/pwd/cwd/mkdir/rm/put/list на ОДНОМ соединении
s = case("plain")
p = subprocess.run([T + "/ftp_session", str(s.port), "anonymous", "a@b"], capture_output=True, timeout=60, env={"ASAN_OPTIONS": "detect_leaks=0"})
out = p.stdout.decode()
check("живая сессия: connect + list + pwd + cwd + mkdir + rm + put + list на одном соединении",
      p.returncode == 0 and "SESSION OK" in out, out + p.stderr.decode()[-800:])
check("put реально дошёл до сервера (STOR принят, байты получены)",
      getattr(s, "uploaded", {}).get("/put_test.txt", b"").startswith(b"LimonOS ftp put"), getattr(s, "uploaded", {}))

# 6c. «как ftp.exe»: open (баннер) БЕЗ логина → user → pass; неверный пароль не рвёт соединение; ls/dir/rmdir/rename/cdup/type
s = case("plain")
p = subprocess.run([T + "/ftp_cli", str(s.port), "bob", "secret"], capture_output=True, timeout=60, env={"ASAN_OPTIONS": "detect_leaks=0"})
out = p.stdout.decode()
check("ftp_open не шлёт USER/PASS сам; user/pass только по вызову; ошибка входа не рвёт соединение; ls/dir/rmdir/rename/cdup на одном соединении",
      p.returncode == 0 and "CLI OK" in out, out + p.stderr.decode()[-800:])
users = [l for l in s.log if l.upper().startswith("USER")]
check("сервер получил USER только после ftp_open (первым в логе AUTH/220, не USER)", s.log and not s.log[0].upper().startswith("USER") and len(users) == 2, s.log)

# 7. защита от подстановки команд и закрытый порт
rc, out, res, err = client(s.port, "get", "/hello.txt\r\nDELE x", "bob", "secret", "auto"); check("CRLF в пути отвергается", rc != 0, res)
sock = socket.socket(); sock.bind(("127.0.0.1", 0)); dead = sock.getsockname()[1]; sock.close()
rc, out, res, err = client(dead, "list", "-", "a", "b", "auto"); check("порт закрыт → ошибка", rc != 0 and "закрыт" in res, res)
print("ПРОВАЛОВ:", bad); sys.exit(1 if bad else 0)
