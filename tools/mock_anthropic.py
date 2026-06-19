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


def _read_request(conn):
    """Read headers + any Content-Length body, so we can branch on its content."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(65536)
        if not chunk:
            break
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    clen = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":")[1].strip())
    body = rest
    while len(body) < clen:
        chunk = conn.recv(65536)
        if not chunk:
            break
        body += chunk
    return body


def _tool_use_stream():
    out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
    out += ('event: content_block_start\r\ndata: {"type":"content_block_start",'
            '"index":0,"content_block":{"type":"tool_use","id":"toolu_1",'
            '"name":"eval_lisp","input":{}}}\r\n\r\n')
    out += ('event: content_block_delta\r\ndata: {"type":"content_block_delta",'
            '"index":0,"delta":{"type":"input_json_delta",'
            '"partial_json":"{\\"code\\":\\"(+ 1 2)\\"}"}}\r\n\r\n')
    out += ('event: content_block_stop\r\ndata: {"type":"content_block_stop","index":0}\r\n\r\n')
    out += ('event: message_delta\r\ndata: {"type":"message_delta",'
            '"delta":{"stop_reason":"tool_use"}}\r\n\r\n')
    out += ('event: message_stop\r\ndata: {"type":"message_stop"}\r\n\r\n')
    return out


def _final_stream():
    out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
    for piece in ["The", " answer", " is", " 3", "."]:
        out += ('event: content_block_delta\r\ndata: {"type":"content_block_delta",'
                '"index":0,"delta":{"type":"text_delta","text":"%s"}}\r\n\r\n' % piece)
    out += ('event: message_stop\r\ndata: {"type":"message_stop"}\r\n\r\n')
    return out


def _serve_conn(conn):
    try:
        body = _read_request(conn)
        if b"tool_result" in body:            # second turn -> final prose
            conn.sendall(_final_stream().encode())
        elif b'"tools"' in body:              # tools-enabled turn -> ask for a tool
            conn.sendall(_tool_use_stream().encode())
        else:                                 # plain M1 turn -> the greeting
            out = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
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
