#!/usr/bin/env python3
"""
autostart_check.py -- a fresh persistent disk boots into the frame (Phase 27.11).

tools/mkdisk.py seeds a brand-new disk.img with /init.l = (run-bg "lisp"
"-frame"). This boots that exact image and confirms PID 1 loads init.l and the
graphical frame comes up by itself -- so a reset disk no longer drops you at
the bare serial REPL + splash.

Run from the repo root:  python3 tools/autostart_check.py
"""

import os
import select
import subprocess
import sys
import tempfile
import time

DISK = os.path.join(tempfile.gettempdir(), "myosv2-autostart.img")


def main() -> int:
    subprocess.run([sys.executable, "tools/mkdisk.py", DISK], check=True)
    cmd = [
        "qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72",
        "-m", "256M", "-display", "none",
        "-chardev", "stdio,id=ch0,signal=off", "-serial", "chardev:ch0",
        "-kernel", "build/kernel.elf",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={DISK},if=none,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-netdev", "user,id=net0", "-device", "virtio-net-device,netdev=net0",
        "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
        "-device", "virtio-gpu-device",
    ]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = b""
    deadline = time.time() + 90          # self-tests are slow under TCG
    try:
        while time.time() < deadline:
            r, _, _ = select.select([p.stdout], [], [], deadline - time.time())
            if not r:
                break
            chunk = p.stdout.read1(4096)
            if not chunk:
                break
            buf += chunk
            if b"frame.l loaded" in buf:
                if b"init.l loaded" not in buf:
                    print("FAIL: frame loaded but init.l was not the trigger")
                    return 1
                print("PASS: 27.11 fresh disk autostarts the graphical frame")
                return 0
            if b"SELF-TESTS FAILED" in buf:
                print("FAIL: self-tests failed at boot"); return 1
        print("FAIL: frame did not autostart within the deadline")
        print(buf.decode(errors="replace")[-400:])
        return 1
    finally:
        p.kill(); p.wait()


if __name__ == "__main__":
    sys.exit(main())
