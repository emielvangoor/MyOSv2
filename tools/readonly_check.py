#!/usr/bin/env python3
"""
readonly_check.py -- read-only buffers refuse typing but stay navigable (Emacs).

C-h b pops *Help* (special-mode, read-only). Switch into it and try to type:
the keystrokes are refused ("buffer is read-only" in the echo area) and the
typed text never appears. Proves the Emacs read-only model.
"""
import os, sys, tempfile, time
sys.path.insert(0, "tools")
from lm_harness import Qemu, qmp, qmp_type, qmp_screendump
from frame_check import load_font, read_ppm, row_text, inv_row, CELL_H


def ctrl(letter):
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ctrl"}}},
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": letter}}}]})
    qmp("input-send-event", {"events": [
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": letter}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ctrl"}}}]})


def main() -> int:
    font = load_font()
    dump = os.path.join(tempfile.gettempdir(), "myosv2-readonly.ppm")
    q = Qemu()
    try:
        if not q.expect(b"lisp> ", 40):
            print("FAIL: no boot"); return 1
        q.send_line('(run "lisp" "-frame")')
        if not q.expect(b"frame.l loaded", 15):
            print("FAIL: frame did not load"); return 1
        time.sleep(1.0)
        ctrl("h"); time.sleep(0.2); qmp_type("b"); time.sleep(0.8)   # C-h b -> *Help*
        ctrl("x"); time.sleep(0.2); qmp_type("o"); time.sleep(0.6)   # C-x o -> into *Help*
        qmp_type("ZZZQ"); time.sleep(0.5)                            # try to type
        qmp_screendump(dump); time.sleep(0.5)
        w, h, data = read_ppm(dump)
        rows = range(h // CELL_H)
        face0 = [row_text(font, w, data, r) for r in rows]
        inv = [inv_row(font, w, data, r) for r in rows]
        for i in rows:
            if face0[i].strip() or "Special" in inv[i]:
                print(f"  row {i}: {face0[i]!r}  inv={inv[i]!r}")
        refused = any("read-only" in t for t in face0)
        not_typed = not any("ZZZQ" in t for t in face0)
        special = any("(Special)" in t for t in inv)
        ok = refused and not_typed and special
        print(f"PASS: *Help* read-only (refused typing, mode (Special))" if ok
              else f"FAIL: refused={refused} not_typed={not_typed} special={special}")
        return 0 if ok else 1
    finally:
        q.kill()


if __name__ == "__main__":
    sys.exit(main())
