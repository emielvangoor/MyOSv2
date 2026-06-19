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
    """Read headers + any Content-Length body. Returns (head_bytes, body_bytes)."""
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
    return head, body


def _chunked(payload):
    """Frame PAYLOAD (a str) as an HTTP chunked text/event-stream, split into
    ~40-byte chunks so the decoder is exercised across chunk boundaries."""
    out = (b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
           b"Transfer-Encoding: chunked\r\n\r\n")
    data = payload.encode()
    i = 0
    while i < len(data):
        piece = data[i:i + 40]
        out += ("%x\r\n" % len(piece)).encode() + piece + b"\r\n"
        i += 40
    out += b"0\r\n\r\n"
    return out


def _openai_tool_payload():
    return (
      'data: {"choices":[{"index":0,"delta":{"role":"assistant","content":null,'
      '"tool_calls":[{"index":0,"id":"call_1","type":"function",'
      '"function":{"name":"eval_lisp","arguments":""}}]}}]}\r\n\r\n'
      'data: {"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,'
      '"function":{"arguments":"{\\"code\\":\\"(+ 1 2)\\"}"}}]}}]}\r\n\r\n'
      'data: {"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]}\r\n\r\n'
      'data: [DONE]\r\n\r\n')


def _openai_final_payload():
    return (
      'data: {"choices":[{"index":0,"delta":{"content":"The"}}]}\r\n\r\n'
      'data: {"choices":[{"index":0,"delta":{"content":" answer is 3."}}]}\r\n\r\n'
      'data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\r\n\r\n'
      'data: [DONE]\r\n\r\n')


def _serve_conn(conn):
    try:
        head, body = _read_request(conn)
        path = head.split(b"\r\n", 1)[0].split(b" ")[1] if head else b""
        if path == b"/chunked":               # chunked-decoding test (Anthropic SSE)
            payload = "".join(_sse(p) for p in REPLY_PIECES)
            conn.sendall(_chunked(payload))
        elif b"chat/completions" in path:      # OpenRouter / OpenAI, chunked
            if b'"role":"tool"' in body or b'"role": "tool"' in body:
                conn.sendall(_chunked(_openai_final_payload()))
            else:
                conn.sendall(_chunked(_openai_tool_payload()))
        else:                                 # any other path -> a plain SSE greeting
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
