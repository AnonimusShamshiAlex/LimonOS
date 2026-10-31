#!/usr/bin/env python3
"""Прокси для ИИ-команды LimonOS: старый ПК шлёт обычный HTTP, прокси сам говорит с поставщиком по HTTPS.

  Запуск на основном ПК (ключ остаётся только здесь и в LimonOS вообще не попадает):
      LIMON_AI_KEY=sk-ant-...  python3 tools/ai_proxy.py                 # Anthropic
      LIMON_AI_KEY=sk-or-v1-.. python3 tools/ai_proxy.py                 # OpenRouter
  В LimonOS:   aikey proxy http://192.168.0.5:8787 anthropic    (или openrouter)

  Безопасность: слушает только адреса, перечисленные в --allow (по умолчанию — локальная сеть
  192.168.0.0/16, 10.0.0.0/8, 172.16.0.0/12), принимает только POST на два известных пути,
  тело не больше 64 КиБ, ключ клиента игнорируется. Не выставляй порт в интернет!"""
import argparse, ipaddress, json, os, sys, urllib.request, urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

UP = {
    "/v1/messages": os.environ.get("LIMON_UPSTREAM_ANTHROPIC", "https://api.anthropic.com/v1/messages"),
    "/api/v1/chat/completions": os.environ.get("LIMON_UPSTREAM_OPENROUTER", "https://openrouter.ai/api/v1/chat/completions"),
}
KEY = os.environ.get("LIMON_AI_KEY", "")
MAX_BODY = 64 * 1024
ALLOW = []

def allowed(ip):
    a = ipaddress.ip_address(ip)
    return any(a in n for n in ALLOW)

class H(BaseHTTPRequestHandler):
    server_version = "LimonProxy"
    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.client_address[0], fmt % a))
    def _send(self, code, body, ctype="application/json"):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.send_header("Connection", "close"); self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        if not allowed(self.client_address[0]): return self._send(403, b'{"error":{"message":"forbidden"}}')
        url = UP.get(self.path)
        if not url: return self._send(404, b'{"error":{"message":"unknown path"}}')
        try: n = int(self.headers.get("Content-Length", "0"))
        except ValueError: n = -1
        if n <= 0 or n > MAX_BODY: return self._send(413, b'{"error":{"message":"bad body size"}}')
        raw = self.rfile.read(n)
        try: json.loads(raw)
        except Exception: return self._send(400, b'{"error":{"message":"bad json"}}')
        h = {"Content-Type": "application/json"}
        if self.path == "/v1/messages": h["x-api-key"] = KEY; h["anthropic-version"] = "2023-06-01"
        else: h["Authorization"] = "Bearer " + KEY
        req = urllib.request.Request(url, data=raw, headers=h, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=60) as r: self._send(r.status, r.read())
        except urllib.error.HTTPError as e: self._send(e.code, e.read())
        except Exception as e: self._send(502, json.dumps({"error": {"message": "upstream: %s" % e}}).encode())
    def do_GET(self): self._send(200, b"LimonOS AI proxy: OK\n", "text/plain")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0"); ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--allow", action="append", help="разрешённая сеть, можно несколько раз (например 192.168.0.0/24)")
    a = ap.parse_args()
    for net in (a.allow or ["127.0.0.0/8", "192.168.0.0/16", "10.0.0.0/8", "172.16.0.0/12"]): ALLOW.append(ipaddress.ip_network(net))
    if not KEY: sys.exit("Задай ключ: LIMON_AI_KEY=sk-... python3 tools/ai_proxy.py")
    print("LimonOS AI proxy слушает %s:%d" % (a.host, a.port), flush=True)
    ThreadingHTTPServer((a.host, a.port), H).serve_forever()
