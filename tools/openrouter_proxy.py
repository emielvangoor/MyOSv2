#!/usr/bin/env python3
"""openrouter_proxy.py -- a tiny host-side TLS terminator for the MyOSv2 agent.

The OS speaks plaintext HTTP to 127.0.0.1:PORT; this forwards it over TLS to
openrouter.ai:443. From the guest, the host is 10.0.2.2 (QEMU user-net), so the
agent's default `*assistant-endpoint-host*`/`*assistant-endpoint-port*` of
10.0.2.2:8787 reach this. Run it in the background, then `M-x emiel`.

    python3 tools/openrouter_proxy.py            # listens on 127.0.0.1:8787
    python3 tools/openrouter_proxy.py 9000        # custom port
"""

import socket
import ssl
import sys
import threading

HOST = "openrouter.ai"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8787


def _pipe(a, b):
    try:
        while True:
            data = a.recv(65536)
            if not data:
                break
            b.sendall(data)
    except Exception:
        pass
    try:
        b.shutdown(socket.SHUT_WR)
    except Exception:
        pass


def _handle(client):
    try:
        ctx = ssl.create_default_context()
        upstream = socket.create_connection((HOST, 443), timeout=15)
        tls = ctx.wrap_socket(upstream, server_hostname=HOST)
        threading.Thread(target=_pipe, args=(client, tls), daemon=True).start()
        _pipe(tls, client)
    except Exception as e:
        print("proxy error:", e, flush=True)
    finally:
        try:
            client.close()
        except Exception:
            pass


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(16)
    print(f"openrouter_proxy: 127.0.0.1:{PORT} -> {HOST}:443 "
          f"(guest sees 10.0.2.2:{PORT})", flush=True)
    while True:
        client, _ = srv.accept()
        threading.Thread(target=_handle, args=(client,), daemon=True).start()


if __name__ == "__main__":
    main()
