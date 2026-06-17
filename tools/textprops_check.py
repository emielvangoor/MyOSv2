#!/usr/bin/env python3
"""
textprops_check.py -- Emacs text properties + ANSI-color translation, end to end.

Guards the whole feature:
  - buffers carry `face` text properties (put/get-text-property, propertize);
  - the renderer paints each character with its resolved face;
  - ansi-color-apply turns a program's SGR escapes into face properties, the
    escape bytes are stripped, and named faces merge correctly
    (the (ansi-blue ansi-bold) of a busybox directory must render BLUE, not the
    default fg -- the bug the attribute-face merge originally hit).

Drives the graphical frame: open a REPL buffer, run `busybox ls /`, screenshot,
and assert (a) the listing renders with ANSI-blue directory entries (sample the
glyph pixels) and (b) no literal escape bytes leaked into the text (OCR shows
clean names like `bin`/`usr`, never `[1;34m`).

Run from the repo root:  python3 tools/textprops_check.py
"""
import os
import sys
import time
import tempfile

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, CELL_H

# ansi-blue is 0x458588 (decimal 4556168) -> RGB (69, 133, 136).
BLUE = (69, 133, 136)


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def count_blue(path, tol=35):
    w, h, body = read_ppm(path)
    n = 0
    for i in range(0, len(body) - 3, 3):
        if (abs(body[i] - BLUE[0]) < tol and abs(body[i + 1] - BLUE[1]) < tol
                and abs(body[i + 2] - BLUE[2]) < tol):
            n += 1
    return n


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-textprops.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot REPL"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("x"); time.sleep(0.2); qmp_type("r"); time.sleep(0.8)   # C-x r: a REPL buffer
        qmp_type('(run "busybox" "ls" "/")\n'); time.sleep(3.0)       # streams colored output
        qmp_screendump(dump); time.sleep(0.3)

        w, h, body = read_ppm(dump)
        rows = [row_text(font, w, body, r) for r in range(h // CELL_H)]
        text = "\n".join(rows)

        # (a) the listing must be present as clean names (`bin` reliably OCRs;
        # other entries are truncated at the window edge or mis-OCR'd, so we
        # don't over-assert on them)...
        if "bin" not in text:
            print("FAIL: `ls` listing not found in the buffer");
            print("rows:", [r for r in rows if r.strip()]); return 1
        # ...and no raw escape bytes leaked (the bug we are fixing showed `[1;34m`).
        if "[1;34m" in text or "[0;0m" in text or "[1;" in text:
            print("FAIL: literal ANSI escape bytes leaked into the buffer"); return 1

        # (b) directories must render in ANSI blue (the merge (ansi-blue ansi-bold)).
        blue = count_blue(dump)
        if blue < 100:
            print("FAIL: no ANSI-blue directory text rendered (blue px=%d)" % blue); return 1

        # (c) NO COLOR BLEED: a `face` interval must not grow rear-ward and swallow
        # text appended after it. Run a plain `echo` after the colored ls; its
        # output must add ~no blue. (The bug: an interval ending at the insertion
        # point expanded over every subsequent insert -> the whole stream turned
        # blue and stayed blue.)
        qmp_type('(run "busybox" "echo" "ZZPLAINTEXTZZ")\n'); time.sleep(2.0)
        qmp_screendump(dump); time.sleep(0.3)
        blue_after = count_blue(dump)
        if blue_after > blue + 60:
            print("FAIL: color bled into plain `echo` output (blue %d -> %d)"
                  % (blue, blue_after)); return 1

        print("TEXTPROPS OK (ansi-blue px=%d, no bleed, escapes stripped)" % blue)
        return 0
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
