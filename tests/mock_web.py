#!/usr/bin/env python3
"""Tiny web server for the web_fetch / web_search tests. Usage: mock_web.py PORT"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port = int(sys.argv[1])
PAGE = """<html><head><title>T</title><style>body{x:y}</style><script>var a=1;</script></head><body><h1>Hello &amp; welcome</h1>
<p>First paragraph with <b>bold</b> and a&nbsp;space.</p><ul><li>one</li><li>two &#8364; &#x41;</li></ul><!-- hidden --></body></html>"""
SEARCH = """<html><body>
<div class="result"><h2><a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fdoc.example.org%2Fapi%3Fa%3D1&amp;rut=abc">Example <b>API</b> docs</a></h2>
<a class="result__snippet" href="x">How to use the <b>API</b> &amp; more</a></div>
<div class="result"><a class="result__a" href="https://other.example.com/page">Other page</a><a class="result__snippet" href="y">Second snippet</a></div>
<div class="result"><a class="result__a" href="https://duckduckgo.com/y.js?ad=1">Ad</a></div>
</body></html>"""
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, ctype, body, extra=None):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/page": self.send(200, "text/html; charset=utf-8", PAGE.encode())
        elif p == "/json": self.send(200, "application/json", b'{"ok": true}')
        elif p == "/bin": self.send(200, "image/png", b"\x89PNG\0\0")
        elif p == "/big": self.send(200, "text/plain", b"abc " * 50000)
        elif p == "/redir": self.send(302, "text/plain", b"", {"Location": "/page"})
        elif p == "/search": self.send(200, "text/html", SEARCH.encode())
        elif p == "/ua": self.send(200, "text/plain", self.headers.get("User-Agent", "").encode())
        else: self.send(404, "text/plain", b"nope")
HTTPServer(("127.0.0.1", port), H).serve_forever()
