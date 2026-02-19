#!/usr/bin/env python3
"""
Test server for ESP32 HTTP/WebSocket smoke tests.

HTTP on :8080
  GET  /hello  → {"message":"hello"}
  POST /echo   → echoes request body

WebSocket on :8081
  Echoes all messages back.

Usage:
  pip install websockets
  python3 test_server.py
"""

import asyncio
import json
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

# ── HTTP server ──────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/hello":
            body = json.dumps({"message": "hello"}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path == "/echo":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            self.send_response(200)
            ct = self.headers.get("Content-Type", "application/octet-stream")
            self.send_header("Content-Type", ct)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def log_message(self, fmt, *args):
        print(f"[HTTP] {fmt % args}")

# ── WebSocket server ─────────────────────────────────────────────────

async def ws_handler(websocket):
    remote = websocket.remote_address
    print(f"[WS] Client connected from {remote}")
    try:
        async for message in websocket:
            print(f"[WS] Echo: {message!r}")
            await websocket.send(message)
    except Exception as e:
        print(f"[WS] Connection closed: {e}")

async def run_ws_server():
    import websockets
    async with websockets.serve(ws_handler, "0.0.0.0", 8081):
        print("[WS] Listening on :8081")
        await asyncio.Future()  # run forever

# ── Main ─────────────────────────────────────────────────────────────

def run_http_server():
    server = HTTPServer(("0.0.0.0", 8080), Handler)
    print("[HTTP] Listening on :8080")
    server.serve_forever()

if __name__ == "__main__":
    # HTTP in a thread, WS in asyncio event loop
    http_thread = threading.Thread(target=run_http_server, daemon=True)
    http_thread.start()

    try:
        asyncio.run(run_ws_server())
    except KeyboardInterrupt:
        print("\nShutting down.")
