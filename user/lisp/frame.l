;;; frame.l -- the graphical machine's event loop and REPL (Phase 25.4).
;;;
;;; Everything a key DOES is decided here, in Lisp -- C only reports what the
;;; key IS ((char 97), (ctrl 120), (mouse x y)). This is the Emacs deal: the
;;; redisplay engine and the input cooker are C machinery; the personality of
;;; the machine -- keymaps, the REPL, what mouse-1 means -- is live Lisp you
;;; can redefine from this very REPL (or over the network).
;;;
;;; Loaded by `lisp -frame` after bootstrap.l and system.l.

;; Where the current REPL input begins (just after the last prompt).
(setq repl-start 0)

(defun prompt ()
  "Show a fresh prompt and remember where input starts."
  (insert "lisp> ")
  (setq repl-start (buffer-length)))

(defun repl-eval ()
  "Evaluate the text after the last prompt; print its value; re-prompt.
The whole REPL in one function: buffer-substring lifts the input out of
the buffer, read-string turns text into a form, eval runs it."
  (let ((src (buffer-substring repl-start (buffer-length))))
    (insert "\n")
    (insert (prin1-to-string (eval (read-string src))))
    (insert "\n")
    (prompt)))

;; C-x prefix state: nil normally, t after a C-x.
(setq pending-cx nil)

(defun handle-cx (ch)
  "The C-x map: 2/3 split, 0 delete, o other-window -- the Emacs chords."
  (setq pending-cx nil)
  (cond
    ((= ch 50) (split-below))          ; C-x 2
    ((= ch 51) (split-right))          ; C-x 3
    ((= ch 48) (delete-window))        ; C-x 0
    ((= ch 111) (other-window))        ; C-x o
    (t (echo "unbound C-x key"))))

(defun dispatch (ev)
  "Route one cooked input event. The keymap is a cond -- live data."
  (cond
    ((null ev) nil)
    ((eq (car ev) 'mouse)
     (select-window-at (cadr ev) (caddr ev)))
    ((eq (car ev) 'ctrl)
     (cond ((= (cadr ev) 120) (setq pending-cx t))       ; C-x: await suffix
           ((= (cadr ev) 103) (setq pending-cx nil)      ; C-g: never mind
                              (echo "quit"))
           (t nil)))
    ((eq (car ev) 'char)
     (let ((ch (cadr ev)))
       (cond
         (pending-cx (handle-cx ch))
         ((= ch 10) (repl-eval))                          ; RET
         ((= ch 8)                                        ; backspace
          (if (> (buffer-length) repl-start) (delete-char 1) nil))
         (t (insert (string-from-char ch))))))            ; self-insert
    (t nil)))

(defun spawn-vm ()
  "Start ANOTHER complete Lisp machine on its own seat: fork, and the child
becomes a fresh `lisp -frame` (its own image, window tree and framebuffer).
Switch with Ctrl-Alt-F1..F4 or (switch-seat n). Returns the child's pid."
  (let ((pid (fork)))
    (if (= pid 0)
        (progn (exec "/bin/lisp" (list "lisp" "-frame")) (exit 127))
      pid)))

(defun frame-loop ()
  "read-event -> dispatch -> redisplay, forever. The machine's heartbeat.
read-event BLOCKS in the kernel (no polling); redisplay paints only what
changed (the glyph-matrix diff), so an idle machine costs nothing."
  (while t
    (dispatch (read-event))
    (redisplay)))

(defun frame-main ()
  "Boot the graphical machine: acquire the screen, greet, loop."
  (if (frame-init)
      (progn
        (insert "MyOSv2 Graphical Lisp Machine\n")
        (insert "C-x 2/3 split  C-x 0 delete  C-x o next  mouse-1 select\n\n")
        (prompt)
        (echo "ready")
        (redisplay)
        (frame-loop))
    (princ "frame: no display\n")))

(princ "frame.l loaded.")
(terpri)
