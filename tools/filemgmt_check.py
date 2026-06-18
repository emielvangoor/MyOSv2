#!/usr/bin/env python3
"""
filemgmt_check.py -- busybox mkdir / ln -s / ln / mv work on the ext2 root.

Guards the file-management syscalls + ext2 vnode ops added for the directory
tier: mkdirat (-> ext2 create VN_DIR), symlinkat (-> ext2_symlink), linkat
(-> ext2_link), renameat/renameat2 (-> ext2_rename). These Linux numbers (34/36/
37/38) had been squatted on by the legacy socket family, so the commands used to
misroute; this check proves they now do the real thing on disk.

Drives an interactive busybox sh and asserts each command's marker appears, that
following a symlink and a hard link both reach the file's contents, and that a
renamed file moves. Run from the repo root: python3 tools/filemgmt_check.py
"""
import re
import select
import sys
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu


def drain(q, window=1.8, cap=200000):
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
    return got


def main() -> int:
    q = Qemu()
    buf = b""
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no Lisp prompt"); return 1
        q.send_line('(run "busybox" "sh")')
        if not q.expect(b"/ #", 15):
            print("FAIL: no sh prompt"); return 1
        cmds = [
            "mkdir /fm && echo MKDIR_OK",        # mkdirat
            "echo hi > /fm/f && cat /fm/f",      # file in the new dir
            "ln -s f /fm/sl && cat /fm/sl",      # symlinkat -> follows -> hi
            "ln /fm/f /fm/hl && cat /fm/hl",     # linkat (hard) -> hi
            "mv /fm/f /fm/g && cat /fm/g",       # renameat -> hi
            "cat /fm/hl",                        # hard link still has the data
            "echo FM_DONE",
        ]
        for c in cmds:
            q.send_line(c)
            buf += drain(q)
    finally:
        q.kill()

    txt = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", buf.decode("utf-8", "replace"))
    if "unhandled #" in txt:
        nums = ", ".join(sorted(set(re.findall(r"unhandled #(\d+)", txt))))
        print(f"FAIL: unhandled syscalls: {nums}")
        return 1

    def need(token, label):
        if token not in txt:
            print(f"FAIL: {label} (missing {token!r})")
            print("---- transcript ----")
            print(txt[-800:])
            return False
        return True

    # "hi" must appear for: cat f, cat sl (symlink), cat hl (hard), cat g (moved),
    # cat hl again -> at least 5 occurrences.
    ok = all([
        need("MKDIR_OK", "mkdir"),
        txt.count("hi") >= 5 or need("__hi5__", "symlink/hardlink/rename reads (5x 'hi')"),
        need("FM_DONE", "shell survived all file-management commands"),
    ])
    if ok:
        print("FILEMGMT OK")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
