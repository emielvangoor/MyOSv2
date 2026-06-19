#!/usr/bin/env python3
"""mock_anthropic.py -- a canned Anthropic Messages SSE endpoint for hermetic
tests. POST anything; it streams a fixed assistant reply as text_delta frames
and closes the connection (no chunked encoding, Connection: close)."""

import socket
import threading

REPLY_PIECES = ["Hello", " from", " the", " mock", "."]   # assembles to the line below
REPLY_TEXT = "".join(REPLY_PIECES)                         # "Hello from the mock."


def _sse(piece):
    body = ('{"type":"content_block_delta","index":0,'
            '"delta":{"type":"text_delta","text":"%s"}}' % piece)
    return "event: content_block_delta\r\ndata: " + body + "\r\n\r\n"


def _serve_conn(conn):
    try:
        conn.recv(65536)                       # drain the request; we ignore it
        out = "HTTP/1.1 200 OK\r\n"
        out += "Content-Type: text/event-stream\r\n"
        out += "Connection: close\r\n\r\n"
        out += "event: message_start\r\ndata: {\"type\":\"message_start\"}\r\n\r\n"
        for p in REPLY_PIECES:
            out += _sse(p)
        out += "event: message_stop\r\ndata: {\"type\":\"message_stop\"}\r\n\r\n"
        conn.sendall(out.encode())
    finally:
        conn.close()


def start(port=0):
    """Start the mock on 127.0.0.1:PORT (0 = pick a free port). Returns the
    bound port; serves forever on a daemon thread."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    bound = srv.getsockname()[1]

    def loop():
        while True:
            conn, _ = srv.accept()
            threading.Thread(target=_serve_conn, args=(conn,), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()
    return bound
