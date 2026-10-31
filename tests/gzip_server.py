import http.server, socketserver, gzip, sys
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = gzip.compress(b"<html>should not be read as text</html>")
        self.send_response(200); self.send_header("Content-Encoding", "gzip"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
S = socketserver.TCPServer(("0.0.0.0", 8090), H); S.allow_reuse_address = True
print("ready", flush=True); S.serve_forever()
