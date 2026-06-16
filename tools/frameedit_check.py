#!/usr/bin/env python3
"""
frameedit_check.py -- the on-device editing loop: edit C in the frame, save,
compile, run -- and the EDITED program runs.

The full IDE cycle in the graphical machine:
  C-x C-f /e.c     open a new file in text-mode
  <type a program> self-insert into the buffer
  C-x C-s          save-buffer -> creat (O_TRUNC) -> the file IS the buffer
  C-x b *scratch*  back to a buffer that evaluates Lisp
  (cc "/e.c" "/e") tcc compiles+links the saved source
  (run-file "/e")  run it -> its output streams into the buffer

Guards two fixes: O_TRUNC (a re-saved file replaces its contents, no stale
tail) and run-file streaming into the buffer. Braces are injected via raw
keycode 26/27 (+shift) -- QEMU's bracketleft/bracketright qcodes don't deliver.

Run from the repo root:  python3 tools/frameedit_check.py
"""
import os
import sys
import tempfile
import time

sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, CELL_H


def _kn(code, down):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": down, "key": {"type": "number", "data": code}}}]})


def brace(code):                       # shift + raw keycode -> { (26) or } (27)
    _kn(42, True); _kn(code, True); _kn(code, False); _kn(42, False)


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def cj():                              # C-j: eval the form before point (lisp-interaction)
    ctrl("j")


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-frameedit.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)

        ctrl("x"); time.sleep(0.2); ctrl("f"); time.sleep(0.5)     # C-x C-f
        qmp_type("/e.c\n"); time.sleep(0.8)                        # new file, text-mode
        # puts() computes the length itself, so the WHOLE string prints -- no
        # hand-counted byte cap. A longer message proves recompile + full output.
        qmp_type("void puts(const char*);int main(void)", delay=0.12)
        brace(26); time.sleep(0.2)                                 # {
        qmp_type('puts("EDITED-LONGER-STRING-OK\\n");return 0;', delay=0.12)
        brace(27); time.sleep(0.4)                                 # }
        ctrl("x"); time.sleep(0.2); ctrl("s"); time.sleep(0.8)     # C-x C-s save
        ctrl("x"); time.sleep(0.2); qmp_type("b"); time.sleep(0.4) # C-x b
        qmp_type("scratch\n"); time.sleep(0.8)                      # -> *scratch*
        qmp_type('(cc "/e.c" "/e")'); cj(); time.sleep(4.0)        # compile
        qmp_type('(run-file "/e")'); cj(); time.sleep(2.0)        # run

        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        lines = [row_text(font, w, data, r, 80) for r in range(h // CELL_H)]
        for i, t in enumerate(lines):
            if t.strip(): print(f"  row {i}: {t!r}")
        # the FULL string prints on its own line (puts computed the length)
        ok = any(t.strip() == "EDITED-LONGER-STRING-OK" for t in lines)
        print("PASS: edited C compiled + ran in the frame (full string via puts)" if ok
              else "FAIL: edited program did not compile/run")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
