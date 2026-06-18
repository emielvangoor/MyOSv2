#!/usr/bin/env python3
"""
redirect_check.py -- busybox sh I/O redirection works end to end.

This guards the syscalls + the console-as-file model that make a shell usable:
  - dup3 / pipe2 (musl's dup2()/pipe()) for the redirect plumbing;
  - fcntl(F_DUPFD_CLOEXEC) returning a REAL fd so ash can SAVE a redirected fd
    and RESTORE it afterwards (a bogus 0 left `< file` stuck on the file -> the
    shell read EOF and exited);
  - a console-backed file so the NULL stdin/stdout/stderr slots can be dup'd;
  - openat O_APPEND so `>>` appends instead of overwriting from offset 0;
  - unlinkat so `rm` removes the scratch files.

Drives an interactive busybox sh and asserts: output redirection writes the
file, append accumulates (wc -l == 2), input redirection works AND the shell
survives it, stderr redirection captures the error, and NO "Bad file
descriptor" flood appears. Run from the repo root: python3 tools/redirect_check.py
"""
import re
import select
import sys
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu


def drain(q, window=2.2, cap=200000):
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
            "echo hello > /r.txt && echo R_WROTE",   # > output redirection
            "cat /r.txt",                            # -> hello
            "echo world >> /r.txt && echo R_APPEND",  # >> append (O_APPEND)
            "wc -l < /r.txt",                        # < input redirection -> 2
            "ls /nope 2>/r.err; cat /r.err",         # 2> stderr redirection
            "echo STILL_ALIVE",                      # shell survived input redirect?
            "rm /r.txt /r.err && echo CLEANED",      # unlinkat
        ]
        for c in cmds:
            q.send_line(c)
            buf += drain(q)
    finally:
        q.kill()

    txt = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", buf.decode("utf-8", "replace"))

    def need(token, label):
        if token not in txt:
            print(f"FAIL: {label} (missing {token!r})")
            print("---- transcript ----")
            print(txt[-800:])
            return False
        return True

    if txt.count("Bad file descriptor"):
        print("FAIL: 'Bad file descriptor' flood -- console fd not dup-able")
        return 1
    ok = all([
        need("R_WROTE", "> redirection"),
        need("hello", "file contents written by >"),
        need("R_APPEND", ">> append"),
        # wc -l on a 2-line file; tolerate surrounding whitespace/ANSI.
        bool(re.search(r"(^|\s)2(\s|$)", txt)) or need("__wc2__", "wc -l < returned 2"),
        need("STILL_ALIVE", "shell survived input redirection"),
        need("No such file", "2> captured stderr"),
        need("CLEANED", "rm via unlinkat"),
    ])
    if ok:
        print("REDIRECT OK")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
