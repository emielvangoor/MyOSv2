#!/usr/bin/env python3
"""
lisp_serve_check.py -- end-to-end check for Phase 24.1b (`lisp -serve`).

Boots the OS under QEMU, starts the Lisp TCP REPL from the shell, then talks to
it from the host over the forwarded port. This is the "boot and observe"
integration test the KTEST suite can't cover (it is inherently userland +
over-the-wire).

Why the serial input is paced char-by-char: the PL011 UART model has a 16-byte
RX FIFO and the guest's console reader drains it at interrupt speed but the
line discipline runs at process speed. Pasting a whole line at once overflows
the FIFO and drops characters (including, fatally, the newline). ~12 ms per
character is comfortably slower than the guest drains and matches human typing.

Checks performed:
  1. boot reaches the shell prompt `$ `
  2. `lisp -serve` reports it is listening
  3. host connects to localhost:7777, evals (+ 1 2) -> 3
  4. a defun made on one connection survives into the next one
     (the image persists across connections -- the whole point)

Exit code 0 = all green. Run from the repo root:  python3 tools/lisp_serve_check.py
"""

import select
import socket
import subprocess
import sys
import time

PORT = 7777
QEMU_CMD = [
    "qemu-system-aarch64",
    "-machine", "virt", "-cpu", "cortex-a72", "-m", "256M", "-display", "none",
    "-chardev", "stdio,id=ch0,signal=off", "-serial", "chardev:ch0",
    "-kernel", "build/kernel.elf",
    "-global", "virtio-mmio.force-legacy=false",
    "-drive", "file=build/disk.img,if=none,format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    # The same user-mode net as `make run`, with the 7777 forward under test.
    "-netdev", f"user,id=net0,hostfwd=tcp::{PORT}-:{PORT}",
    "-device", "virtio-net-device,netdev=net0",
]

class Qemu:
    def __init__(self):
        self.proc = subprocess.Popen(
            QEMU_CMD, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        self.buf = b""

    def expect(self, pattern: bytes, timeout: float = 20.0) -> bool:
        """Read serial output until `pattern` appears (or timeout)."""
        deadline = time.time() + timeout
        while pattern not in self.buf:
            remaining = deadline - time.time()
            if remaining <= 0:
                return False
            r, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not r:
                return False
            chunk = self.proc.stdout.read1(4096)
            if not chunk:
                return False
            self.buf += chunk
        # Drop everything up to and including the match so the next expect()
        # can't re-match stale output.
        self.buf = self.buf.split(pattern, 1)[1]
        return True

    def send_line(self, line: str):
        """Type a command char-by-char with ~12 ms gaps (see module docstring)."""
        for ch in line + "\r":
            self.proc.stdin.write(ch.encode())
            self.proc.stdin.flush()
            time.sleep(0.012)

    def kill(self):
        self.proc.kill()
        self.proc.wait()


def read_until_prompt(sock: socket.socket, timeout: float = 5.0) -> str:
    """Read until the next `lisp> ` prompt (or EOF)."""
    sock.settimeout(timeout)
    out = b""
    while not out.endswith(b"lisp> "):
        chunk = sock.recv(4096)
        if not chunk:
            break
        out += chunk
    return out.decode(errors="replace")


def connect_repl() -> socket.socket:
    """Connect and consume the banner + first prompt, so each subsequent
    roundtrip's output is delimited by exactly one prompt."""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    read_until_prompt(s)
    return s


def repl_roundtrip(sock: socket.socket, form: str) -> str:
    """Send one form, return everything up to the next `lisp> ` prompt."""
    sock.sendall(form.encode() + b"\n")
    return read_until_prompt(sock)


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"$ ", 30):
            print("FAIL: never saw the shell prompt"); return 1
        print("ok: shell prompt")

        q.send_line("lisp -serve")
        if not q.expect(b"serving on port 7777", 15):
            print("FAIL: lisp -serve did not report it is listening"); return 1
        print("ok: server reports listening")

        # Give the guest a beat to actually sit in accept().
        time.sleep(0.5)

        # --- connection 1: eval + defun ---
        s1 = connect_repl()
        out = repl_roundtrip(s1, "(+ 1 2)")
        if "3" not in out:
            print(f"FAIL: (+ 1 2) over TCP, got: {out!r}"); return 1
        print("ok: (+ 1 2) -> 3 over TCP")

        out = repl_roundtrip(s1, "(defun twice (x) (* 2 x))")
        if "twice" not in out:
            print(f"FAIL: defun reply, got: {out!r}"); return 1

        # An error must come back over the SOCKET (lm_error -> lm_cur_out),
        # not vanish onto the guest's serial console.
        out = repl_roundtrip(s1, "(nosuchfunction 1)")
        if "ERROR" not in out:
            print(f"FAIL: error not reported to the socket, got: {out!r}"); return 1
        s1.close()
        print("ok: defined twice, saw remote ERROR, disconnected")

        # --- connection 2: the image must have survived ---
        time.sleep(0.5)
        s2 = connect_repl()
        out = repl_roundtrip(s2, "(twice 21)")
        if "42" not in out:
            print(f"FAIL: image did not persist across connections, got: {out!r}")
            return 1
        s2.close()
        print("ok: image persisted across reconnect ((twice 21) -> 42)")

        print("PASS: 24.1b TCP REPL verified")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
