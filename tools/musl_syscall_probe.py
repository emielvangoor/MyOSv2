#!/usr/bin/env python3
"""
musl_syscall_probe.py -- run busybox applets under sh and report every
`[syscall] unhandled #N (...)` line the kernel emits, attributed to the command
that triggered it. Diagnostic, not a pass/fail gate.

Robust against floods: each command's output is drained for a bounded window
with a hard byte cap, so a binary that spins retrying an ENOSYS syscall cannot
hang the probe. Run from the repo root:  python3 tools/musl_syscall_probe.py
"""
import re
import select
import sys
import time
from collections import Counter

sys.path.insert(0, "tools")
from lm_harness import Qemu

CMDS = [
    "ls -la /",
    "cat /hello.c",
    "pwd",
    "grep include /hello.c",
    "wc -l /hello.c",
    "head -2 /hello.c",
    "date",
    "uname -a",
    "df",
    "find /lib -name '*.l'",
    "ls /usr/include",
    "env",
    "true && echo AND_OK",
    "test -f /hello.c && echo TEST_OK",
    "mkdir /pd && echo MKDIR_OK",
    "touch /pd/f && echo TOUCH_OK",
    "rm /pd/f && echo RM_OK",
    "ln -s /hello.c /pd/l && echo LN_OK",
    "readlink /pd/l && echo READLINK_OK",
    "chmod 644 /hello.c && echo CHMOD_OK",
    "cp /hello.c /pd/h && echo CP_OK",
    "mv /pd/h /pd/h2 && echo MV_OK",
    "stat /hello.c",
    "sleep 1 && echo SLEEP_OK",
    "dmesg 2>/dev/null | head -1; echo DMESG_DONE",
]

CAP = 200_000      # max bytes to read per command before moving on (flood guard)
WINDOW = 1.2       # seconds to wait for a command's output


def drain(q, window, cap):
    """Read serial for up to `window` seconds, at most `cap` bytes. Returns the
    chunk read so we can attribute floods to the command that caused them."""
    start = time.time()
    got = b""
    while time.time() - start < window and len(got) < cap:
        r, _, _ = select.select([q.proc.stdout], [], [], 0.2)
        if not r:
            continue
        chunk = q.proc.stdout.read1(8192)
        if not chunk:
            break
        got += chunk
    q.buf += got
    return got


def main() -> int:
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no Lisp prompt"); return 1
        q.send_line('(run "busybox" "sh")')
        if not q.expect(b"/ #", 15):
            print("FAIL: no sh prompt"); return 1

        per_cmd = []   # (cmd, list-of-unhandled-numbers, flooded?)
        for c in CMDS:
            q.send_line(c)
            chunk = drain(q, WINDOW, CAP)
            nums = re.findall(rb"\[syscall\] unhandled #(\d+)", chunk)
            flooded = len(chunk) >= CAP
            per_cmd.append((c, [int(n) for n in nums], flooded))
            if flooded:
                # A flood means the binary is spinning on an ENOSYS retry. Stop
                # the runaway and re-establish a clean prompt for the next cmd.
                q.send_line("")           # nudge
                for _ in range(8):        # bounded: drain until a quiet window
                    if not drain(q, 0.6, CAP):
                        break

        total = Counter()
        print("=== per-command unhandled syscalls ===")
        for c, nums, flooded in per_cmd:
            if nums or flooded:
                cnt = Counter(nums)
                tag = "  [FLOOD]" if flooded else ""
                detail = ", ".join(f"#{n}x{k}" for n, k in sorted(cnt.items()))
                print(f"  {c!r}: {detail or '(none captured)'}{tag}")
                total.update(nums)
        print("\n=== distinct unhandled syscall numbers ===")
        if not total:
            print("  NONE")
        for n, k in sorted(total.items()):
            print(f"  #{n:<5} x{k}")
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
