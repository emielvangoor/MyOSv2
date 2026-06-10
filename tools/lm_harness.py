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

import json
import os
import select
import socket
import subprocess
import time

# The guest serves on 7777 (not 7000: macOS's AirPlay Receiver squats on
# 7000). The HOST side of the test forward is 17777, so these checks can run
# while an interactive `make run` (which forwards host 7777) is also up.
GUEST_PORT = 7777
HOST_PORT = 17777
QMP_SOCK = f"/tmp/myosv2-qmp-{os.getpid()}.sock"

QEMU_CMD = [
    "qemu-system-aarch64",
    "-machine", "virt", "-cpu", "cortex-a72", "-m", "256M", "-display", "none",
    "-chardev", "stdio,id=ch0,signal=off", "-serial", "chardev:ch0",
    "-kernel", "build/kernel.elf",
    "-global", "virtio-mmio.force-legacy=false",
    # locking=off + -snapshot: a test boot must coexist with an interactive
    # `make run` that holds the image's write lock -- snapshot mode sends all
    # writes to a throwaway overlay, so skipping the lock is safe.
    "-drive", "file=build/disk.img,file.locking=off,if=none,format=raw,id=hd0",
    "-device", "virtio-blk-device,drive=hd0",
    "-snapshot",
    # The same user-mode net as `make run`, with the REPL forward on a
    # test-private host port.
    "-netdev", f"user,id=net0,hostfwd=tcp::{HOST_PORT}-:{GUEST_PORT}",
    "-device", "virtio-net-device,netdev=net0",
    # Keyboard + tablet + display (same as make run since Phase 25) and a QMP
    # socket so checks can inject input events and take screendumps.
    "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
    "-device", "virtio-gpu-device",
    "-qmp", f"unix:{QMP_SOCK},server=on,wait=off",
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


def boot_to_lisp() -> Qemu:
    """Boot to the Lisp REPL. Since 24.4 init IS /bin/lisp, so the machine
    lands there by itself; raise if it doesn't."""
    q = Qemu()
    if not q.expect(b"lisp> ", 30):
        q.kill()
        raise RuntimeError("never saw the Lisp prompt")
    return q


def boot_to_serve() -> Qemu:
    """Boot, then start the network REPL as a child of init:
    (run "lisp" "-serve"). Raise if either step fails."""
    q = boot_to_lisp()
    q.send_line('(run "lisp" "-serve")')
    if not q.expect(b"serving on port %d" % GUEST_PORT, 15):
        q.kill()
        raise RuntimeError("lisp -serve did not report it is listening")
    time.sleep(0.5)   # let the guest actually sit in accept()
    return q


def qmp(cmd: str, args: dict | None = None) -> dict:
    """One QMP command over the UNIX socket. A fresh handshake per call --
    simple beats stateful for a test harness."""
    s = socket.socket(socket.AF_UNIX)
    s.connect(QMP_SOCK)
    f = s.makefile("rw")
    f.readline()                                  # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush()
    f.readline()
    f.write(json.dumps({"execute": cmd, "arguments": args or {}}) + "\n"); f.flush()
    resp = json.loads(f.readline())
    s.close()
    return resp


def qmp_key(qcode: str):
    """Press and release one key on the virtio keyboard."""
    for down in (True, False):
        qmp("input-send-event", {"events": [{"type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}]})


def qmp_tablet(x: int, y: int):
    """Move the absolute pointer (0..32767 in each axis)."""
    qmp("input-send-event", {"events": [
        {"type": "abs", "data": {"axis": "x", "value": x}},
        {"type": "abs", "data": {"axis": "y", "value": y}}]})


_QCODE = {  # ascii -> (qcode, needs_shift)
    **{c: (c, False) for c in "abcdefghijklmnopqrstuvwxyz0123456789"},
    **{c.upper(): (c, True) for c in "abcdefghijklmnopqrstuvwxyz"},
    " ": ("spc", False), "\n": ("ret", False),
    "(": ("9", True), ")": ("0", True), "+": ("equal", True),
    "-": ("minus", False), "=": ("equal", False), "*": ("8", True),
    '"': ("apostrophe", True), "'": ("apostrophe", False),
    "/": ("slash", False), ".": ("dot", False), ",": ("comma", False),
    "!": ("1", True), "?": ("slash", True), ";": ("semicolon", False),
    ":": ("semicolon", True), "<": ("comma", True), ">": ("dot", True),
}


def qmp_type(text: str, delay: float = 0.05):
    """Type a whole string on the virtio keyboard, shift and all."""
    for ch in text:
        qcode, shifted = _QCODE[ch]
        if shifted:
            qmp("input-send-event", {"events": [
                {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "shift"}}},
                {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}}]})
            qmp("input-send-event", {"events": [
                {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
                {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}}]})
        else:
            qmp_key(qcode)
        time.sleep(delay)


def qmp_screendump(path: str):
    """Dump the current scanout (display 0) to a binary PPM file."""
    qmp("screendump", {"filename": path})


def ppm_pixel(path: str, x: int, y: int) -> tuple:
    """(r, g, b) at (x, y) of a binary P6 PPM -- QEMU's screendump format."""
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, _h = map(int, line.split())
        f.readline()                              # maxval
        f.seek(3 * (y * w + x), 1)
        r, g, b = f.read(3)
        return (r, g, b)


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
    s = socket.create_connection(("127.0.0.1", HOST_PORT), timeout=5)
    read_until_prompt(s)
    return s


def repl_roundtrip(sock: socket.socket, form: str) -> str:
    """Send one form, return everything up to the next `lisp> ` prompt."""
    sock.sendall(form.encode() + b"\n")
    return read_until_prompt(sock)
