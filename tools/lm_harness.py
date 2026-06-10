"""
lm_harness.py -- shared plumbing for the Phase-24 boot-and-observe checks.

The KTEST suite (`make test`) red-greens the portable Lisp core inside the
kernel; everything that is inherently userland or over-the-wire (the TCP REPL,
the system-call primitives, the Lisp shell, the init flip) is verified by
booting the real OS under QEMU and observing it. This module is that harness:
boot, drive the serial console, talk to the Lisp REPL over the forwarded port.

Serial input is paced char-by-char (~12 ms gaps): the PL011 model has a 16-byte
RX FIFO and pasting a whole line at once overflows it, dropping characters --
including, fatally, the newline. The TCP path has no such limit.
"""

import select
import socket
import subprocess
import time

PORT = 7777   # not 7000: macOS's AirPlay Receiver squats on 7000

QEMU_CMD = [
    "qemu-system-aarch64",
    "-machine", "virt", "-cpu", "cortex-a72", "-m", "256M", "-display", "none",
    "-chardev", "stdio,id=ch0,signal=off", "-serial", "chardev:ch0",
    "-kernel", "build/kernel.elf",
    "-global", "virtio-mmio.force-legacy=false",
    "-drive", "file=build/disk.img,if=none,format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    # The same user-mode net as `make run`, including the REPL forward.
    "-netdev", f"user,id=net0,hostfwd=tcp::{PORT}-:{PORT}",
    "-device", "virtio-net-device,netdev=net0",
]


class Qemu:
    """A booted guest with an expect/send interface on its serial console."""

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


def boot_to_serve() -> Qemu:
    """Boot to the shell and start `lisp -serve`; raise if either step fails."""
    q = Qemu()
    if not q.expect(b"$ ", 30):
        q.kill()
        raise RuntimeError("never saw the shell prompt")
    q.send_line("lisp -serve")
    if not q.expect(b"serving on port %d" % PORT, 15):
        q.kill()
        raise RuntimeError("lisp -serve did not report it is listening")
    time.sleep(0.5)   # let the guest actually sit in accept()
    return q


def read_until_prompt(sock: socket.socket, timeout: float = 10.0) -> str:
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
