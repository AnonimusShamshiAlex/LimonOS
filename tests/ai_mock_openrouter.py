"""Имитация OpenRouter (/api/v1/chat/completions, формат OpenAI): проверяет Authorization: Bearer,
   отвечает текстом с кавычками, переносом строки и \\u-юникодом — тот же набор проверок, что для Anthropic."""
import http.server, ssl, json, sys
T = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); raw = self.rfile.read(n)
        try: req = json.loads(raw)
        except Exception as e:
            self.send_response(400); self.end_headers(); self.wfile.write(str(e).encode()); return
        auth = self.headers.get("Authorization", "")
        ok = auth == "Bearer sk-or-v1-TEST456" and req.get("model") and req["messages"][0]["role"] == "user"
        if not ok:
            self.send_response(401); b = json.dumps({"error":{"message":"No auth credentials found"}}).encode()
            self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        q = req["messages"][0]["content"]
        reply = f'Через OpenRouter: "{q}"\nВторая строка: \u0422\u0435\u0441\u0442 and "quotes" and \\backslash.'
        body = json.dumps({"id":"gen-1","choices":[{"index":0,"message":{"role":"assistant","content":reply},"finish_reason":"stop"}],"model":req.get("model")}).encode("utf-8")
        self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
S = http.server.HTTPServer(("0.0.0.0", 8443), H); S.allow_reuse_address = True
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(T + "/ec.crt", T + "/ec.key")
S.socket = ctx.wrap_socket(S.socket, server_side=True)
print("ready", flush=True)
S.serve_forever()
