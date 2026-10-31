"""Имитирует /v1/messages: проверяет заголовки (x-api-key, anthropic-version), JSON тела,
   отвечает текстом с кавычками, переводом строки и \u0041, чтобы проверить разбор на стороне LimonOS."""
import http.server, ssl, json, sys
T = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); raw = self.rfile.read(n)
        try:
            req = json.loads(raw)
        except Exception as e:
            self.send_response(400); self.end_headers(); self.wfile.write(str(e).encode()); return
        ok = self.headers.get("x-api-key") == "sk-ant-TEST123" and self.headers.get("anthropic-version") == "2023-06-01" \
             and req.get("model") == "claude-haiku-5-5" and req["messages"][0]["role"] == "user"
        if not ok:
            self.send_response(401); b = json.dumps({"type":"error","error":{"message":"bad auth or body"}}).encode()
            self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        q = req["messages"][0]["content"]
        reply = f'Ты спросил: "{q}"\nВторая строка с юникодом: \u0041\u042f (A and Я) и забавным символом.'
        body = json.dumps({"id":"msg_1","type":"message","role":"assistant",
                            "content":[{"type":"text","text":reply}], "model":"claude-haiku-5-5"}).encode("utf-8")
        self.send_response(200); self.send_header("Content-Type","application/json"); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)
S = http.server.HTTPServer(("0.0.0.0", 8443), H); S.allow_reuse_address = True
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(T + "/ec.crt", T + "/ec.key")
S.socket = ctx.wrap_socket(S.socket, server_side=True)
print("ready", flush=True)
S.serve_forever()
