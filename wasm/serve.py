#!/usr/bin/env python3
"""Local dev server for POM2 WASM with COOP+COEP headers.

Usage:  python3 serve.py [port]   # defaults to 8080
"""
import http.server, sys, os
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
os.chdir(os.path.dirname(os.path.abspath(__file__)))
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy",   "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()
print(f"POM2 WASM dev server: http://127.0.0.1:{PORT}/")
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
