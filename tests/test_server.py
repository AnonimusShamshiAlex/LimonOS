#!/usr/bin/env python3
"""Тестовый сервер для сетевого стека LimonOS: HTTP :8080 и HTTPS :8443 (TLS 1.3, самоподписанный сертификат).
Пути: /big.bin (3 МБ), /chunked (chunked, 1 МБ), /redir (302 -> /big.bin), /redir2 (302 на относительный путь),
      /downgrade (302 на http://), /echo (POST: вернёт sha256 тела)"""
import http.server, socketserver, ssl, hashlib, os, sys, threading
T = sys.argv[1]
BIG = open(T + "/big.bin", "rb").read()
CH = open(T + "/chunk.bin", "rb").read()
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_GET(self):
        p = self.path
        if p == "/big.bin":
            self.send_response(200); self.send_header("Content-Length", str(len(BIG))); self.send_header("Content-Type", "application/octet-stream"); self.end_headers(); self.wfile.write(BIG)
        elif p == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            i = 0; sizes = [1, 17, 4096, 70000, 255]
            k = 0
            while i < len(CH):
                n = sizes[k % len(sizes)]; k += 1
                part = CH[i:i+n]; i += n
                self.wfile.write(b"%x;ext=1\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\nX-Trailer: yes\r\n\r\n")
        elif p == "/redir":
            self.send_response(302); self.send_header("Location", "/big.bin"); self.send_header("Content-Length", "0"); self.end_headers()
        elif p == "/redir2":
            self.send_response(301); self.send_header("Location", "sub/../big.bin".replace("sub/../", "")); self.send_header("Content-Length", "0"); self.end_headers()
        elif p == "/downgrade":
            self.send_response(302); self.send_header("Location", "http://10.0.2.1:8080/big.bin"); self.send_header("Content-Length", "0"); self.end_headers()
        elif p == "/hello":
            b = b"hello limon\n"; self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
        elif p == "/eof":    # без Content-Length, конец по закрытию соединения
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(CH[:300000]); self.close_connection = True
        else:
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); d = self.rfile.read(n)
        out = hashlib.sha256(d).hexdigest().encode()
        self.send_response(200); self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True; allow_reuse_address = True; request_queue_size = 32
a = S(("0.0.0.0", 8080), H)
b = S(("0.0.0.0", 8443), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(T + "/ec.crt", T + "/ec.key")
b.socket = ctx.wrap_socket(b.socket, server_side=True)
threading.Thread(target=a.serve_forever, daemon=True).start()
print("ready", flush=True)
b.serve_forever()
