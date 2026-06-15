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

;; Input history: submitted forms, most-recent-first, walked with Up/Down
;; (which the kernel cooks into C-p/C-n). repl-hist-idx is the position being
;; shown -- -1 means "not navigating; the live line is the user's own".
(setq repl-history nil)
(setq repl-hist-idx -1)

(defun prompt ()
  "Show a fresh prompt and remember where input starts."
  (insert "lisp> ")
  (setq repl-start (buffer-length)))

(defun repl-eval ()
  "Evaluate the text after the last prompt; print its value; re-prompt.
The whole REPL in one function: buffer-substring lifts the input out of
the buffer, read-string turns text into a form, eval runs it."
  (let ((src (buffer-substring repl-start (buffer-length))))
    ;; Remember non-empty input, and leave history-navigation mode: the next
    ;; Up starts again from the most recent entry.
    (if (> (string-length src) 0)
        (setq repl-history (cons src repl-history)) nil)
    (setq repl-hist-idx -1)
    (insert "\n")
    (let ((val (eval (read-string src))))
      (flush-output)                ; princ'd text lands HERE, before the value
      (insert (prin1-to-string val))
      (insert "\n")
      (prompt))))

(defun repl-set-input (s)
  "Replace the current REPL input (everything after the prompt) with S."
  (let ((n (- (buffer-length) repl-start)))
    (if (> n 0) (delete-char n) nil))     ; erase the old line back to the prompt
  (insert s))

(defun repl-recall ()
  "Show the history entry at repl-hist-idx (or an empty line at -1)."
  (repl-set-input (if (>= repl-hist-idx 0) (nth repl-hist-idx repl-history) "")))

(defun repl-history-prev ()
  "Up / C-p: step to an OLDER entry, if there is one."
  (if (< (+ repl-hist-idx 1) (length repl-history))
      (progn (setq repl-hist-idx (+ repl-hist-idx 1)) (repl-recall)) nil))

(defun repl-history-next ()
  "Down / C-n: step to a NEWER entry, or back to the empty line."
  (if (>= repl-hist-idx 0)
      (progn (setq repl-hist-idx (- repl-hist-idx 1)) (repl-recall)) nil))

;; C-x prefix state: nil normally, t after a C-x.
(setq pending-cx nil)

;; The REPL's buffer handle, and the alist of (buffer . file path) for buffers
;; visiting a file. editing-p tells dispatch which keymap to use: the same RET
;; means "evaluate" in the REPL and "insert a newline" in a file buffer.
(setq repl-buf 0)
(setq file-of nil)

(defun editing-p ()
  "t when the selected buffer is a plain editable buffer, not the REPL."
  (not (= (current-buffer) repl-buf)))

(defun edit-key (ch)
  "Self-insert editing for a file buffer: backspace deletes before point,
everything else (RET included -> a newline) inserts at point."
  (cond
    ((= ch 8) (if (> (point) 0) (delete-char 1) nil))    ; backspace
    (t (insert (string-from-char ch)))))                 ; incl. RET -> newline

(defun handle-cx (ev)
  "The C-x map: 2/3 split, 0 delete, o other-window, C-s save -- note C-x C-s
is two CTRL events, so this dispatches on the whole event, not a bare char."
  (setq pending-cx nil)
  (cond
    ((and (eq (car ev) 'char) (= (cadr ev) 50)) (split-below))    ; C-x 2
    ((and (eq (car ev) 'char) (= (cadr ev) 51)) (split-right))    ; C-x 3
    ((and (eq (car ev) 'char) (= (cadr ev) 48)) (delete-window))  ; C-x 0
    ((and (eq (car ev) 'char) (= (cadr ev) 111)) (other-window))  ; C-x o
    ((and (eq (car ev) 'ctrl) (= (cadr ev) 115)) (save-buffer))   ; C-x C-s
    (t (echo "unbound C-x key"))))

(defun dispatch (ev)
  "Route one cooked input event. The keymap is a cond -- live data."
  (cond
    ((null ev) nil)
    (mb-action (mb-dispatch ev))                          ; minibuffer owns input
    (pending-cx (handle-cx ev))                           ; C-x <key> (char OR ctrl)
    ((eq (car ev) 'mouse)
     (select-window-at (cadr ev) (caddr ev)))
    ((eq (car ev) 'meta)
     (if (= (cadr ev) 120) (mb-start 'mx) nil))           ; M-x
    (pending-ch
     (setq pending-ch nil)
     (if (and (eq (car ev) 'char) (= (cadr ev) 102))      ; C-h f
         (mb-start 'describe)
       (echo "unbound C-h key")))
    ((eq (car ev) 'ctrl)
     (cond ((= (cadr ev) 120) (setq pending-cx t))       ; C-x: await suffix
           ((= (cadr ev) 104) (setq pending-ch t))       ; C-h: await suffix
           ((= (cadr ev) 98)                             ; C-b / Left: back a char
            ;; In the REPL, stop at the prompt (repl-start); in a file buffer,
            ;; at the start of the buffer.
            (let ((lo (if (editing-p) 0 repl-start)))
              (if (> (point) lo) (goto-char (- (point) 1)) nil)))
           ((= (cadr ev) 102)                            ; C-f / Right: forward
            (if (< (point) (buffer-length)) (goto-char (+ (point) 1)) nil))
           ((= (cadr ev) 112)                            ; C-p / Up: history (REPL)
            (if (editing-p) nil (repl-history-prev)))
           ((= (cadr ev) 110)                            ; C-n / Down: history (REPL)
            (if (editing-p) nil (repl-history-next)))
           ((= (cadr ev) 103) (setq pending-cx nil)      ; C-g: never mind
                              (echo "quit"))
           (t nil)))
    ((eq (car ev) 'char)
     (if (editing-p)
         (edit-key (cadr ev))                             ; file buffer: plain edit
       (let ((ch (cadr ev)))
         (cond
           ((= ch 10) (repl-eval))                        ; RET: evaluate
           ((= ch 8)                                      ; backspace: char before
            (if (> (point) repl-start) (delete-char 1) nil))  ; point, not the prompt
           (t (insert (string-from-char ch)))))))         ; self-insert (at point)
    (t nil)))

;; ---- visiting files: open, edit, save (Phase 27.3) ----------------------

(defun find-file (path)
  "Open PATH in a split window below; the file shows there and the buffer
remembers its path (so C-x C-s knows where to write). A missing file starts
empty. We return selection to the REPL before finishing -- like (teapot) --
so the REPL's own 'print the value and re-prompt' lands in the REPL, not in
the file buffer. Switch to the file with C-x o to start editing."
  (split-below)
  (other-window)                           ; into the new (bottom) window
  (let ((b (make-buffer path)))
    (set-buffer b)                         ; show the file there
    (setq file-of (cons (cons b path) file-of))
    (let ((fd (open path)))
      (if (>= fd 0)
          (progn
            (let ((chunk (fd-read fd 1024)))
              (while chunk (insert chunk) (setq chunk (fd-read fd 1024))))
            (close fd))
        nil))                              ; nonexistent: a fresh empty buffer
    (goto-char 0)
    (other-window)                         ; back to the REPL (see docstring)
    (redisplay)
    b))

(defun save-buffer ()
  "Write the current buffer back to the file it is visiting (C-x C-s)."
  (let ((path (cdr (assoc (current-buffer) file-of))))
    (if path
        (let ((fd (creat path)))
          (if (>= fd 0)
              (progn (fd-write fd (buffer-substring 0 (buffer-length)))
                     (close fd)
                     (echo (string-concat "saved " path)))
            (echo "save failed")))
      (echo "no file for this buffer"))))

;; ---- the minibuffer: vertico-style command selection -------------------
;;
;; The echo area grows into a live-narrowing candidate list: type to filter,
;; C-n/C-p (or arrows) to move the selection bar, RET to commit, C-g to
;; cancel, TAB to adopt the selection as input. M-x completes over every
;; symbol with a function -- the symbol table IS the command palette.

(setq mb-action nil)        ; nil = inactive; 'mx or 'describe
(setq mb-input "")
(setq mb-matches nil)
(setq mb-sel 0)

(defun command-symbols ()
  "Every symbol whose function slot holds something runnable."
  (filter (lambda (s) (function-info s)) (all-symbols)))

(defun mb-filter ()
  "Narrow the candidates to those containing mb-input."
  (setq mb-matches
        (filter (lambda (s)
                  (or (= (string-length mb-input) 0)
                      (string-search mb-input (symbol-name s))))
                (command-symbols)))
  (setq mb-sel 0))

(defun take (n lst)
  (if (or (= n 0) (null lst)) nil
    (cons (car lst) (take (- n 1) (cdr lst)))))

(defun mb-render ()
  "Paint the minibuffer: prompt+input on top, up to 8 candidates below,
the selected one highlighted (echo-select drives the face-2 bar)."
  (let ((txt (string-concat (if (eq mb-action 'mx) "M-x " "Describe: ") mb-input)))
    (foreach (lambda (s) (setq txt (string-concat txt "\n  " (symbol-name s))))
             (take 8 mb-matches))
    (echo txt)
    (echo-select (+ mb-sel 1))))

(defun mb-start (action)
  (setq mb-action action)
  (setq mb-input "")
  (mb-filter)
  (mb-render))

(defun mb-stop ()
  (setq mb-action nil)
  (echo "ready"))

(defun describe-function-text (sym)
  "The living object, not documentation about it: for a defun this prints
the very lambda the machine runs -- which you can redefine."
  (let ((info (function-info sym)))
    (cond
      ((null info) (string-concat (symbol-name sym) " is not a function"))
      ((eq (car info) 'primitive)
       (string-concat (symbol-name sym) " is a C primitive taking "
                      (caddr info) ".." (caddr (cdr info)) " args"))
      (t (string-concat (symbol-name sym) " is a " (symbol-name (car info))
                        "\n\nparameters: " (prin1-to-string (cadr info))
                        "\n\nsource:\n" (prin1-to-string (caddr info)))))))

(setq help-buf nil)

(defun show-help (txt)
  "Pop *Help* in the other window (splitting on first use), Emacs-style."
  (if help-buf nil
    (progn (setq help-buf (make-buffer "*Help*"))
           (split-below)))
  (other-window)
  (set-buffer help-buf)
  (goto-char (buffer-length))
  (delete-char (buffer-length))
  (insert txt)
  (other-window))

(defun mb-commit ()
  (let ((sym (if mb-matches (nth mb-sel mb-matches)
               (read-string mb-input)))
        (action mb-action))
    (mb-stop)
    (cond
      ((eq action 'mx) (let ((val (eval (list sym))))
                          (flush-output)
                          (insert (prin1-to-string val))
                          (insert "\n") (prompt)))
      ((eq action 'describe) (show-help (describe-function-text sym))))))

(defun mb-dispatch (ev)
  "The minibuffer keymap -- live data, like everything else here."
  (cond
    ((eq (car ev) 'ctrl)
     (cond ((= (cadr ev) 110)                              ; C-n: down
            (if (< (+ mb-sel 1) (length (take 8 mb-matches)))
                (setq mb-sel (+ mb-sel 1)) nil)
            (mb-render))
           ((= (cadr ev) 112)                              ; C-p: up
            (if (> mb-sel 0) (setq mb-sel (- mb-sel 1)) nil)
            (mb-render))
           ((= (cadr ev) 103) (mb-stop))                   ; C-g: cancel
           (t nil)))
    ((eq (car ev) 'char)
     (let ((ch (cadr ev)))
       (cond
         ((= ch 10) (mb-commit))                           ; RET
         ((= ch 9)                                         ; TAB: adopt selection
          (if mb-matches
              (progn (setq mb-input (symbol-name (nth mb-sel mb-matches)))
                     (mb-filter) (mb-render))
            nil))
         ((= ch 8)                                         ; backspace
          (let ((n (string-length mb-input)))
            (if (> n 0) (setq mb-input (substring mb-input 0 (- n 1))) nil))
          (mb-filter) (mb-render))
         (t (setq mb-input (string-concat mb-input (string-from-char ch)))
            (mb-filter) (mb-render)))))
    (t nil)))

;; C-h prefix state (like pending-cx): C-h f = describe-function.
(setq pending-ch nil)

(defun spawn-vm ()
  "Start ANOTHER complete Lisp machine on its own seat: fork, and the child
becomes a fresh `lisp -frame` (its own image, window tree and framebuffer).
Switch with Ctrl-Alt-F1..F4 or (switch-seat n). Returns the child's pid."
  (let ((pid (fork)))
    (if (= pid 0)
        (progn (exec "/bin/lisp" (list "lisp" "-frame")) (exit 127))
      pid)))

;; ---- running programs INSIDE the frame ----------------------------------
;;
;; system.l's run-prog lets a child inherit fd 1 (the serial console). In the
;; frame, output belongs in the BUFFER -- so we redefine run-prog (loading
;; after system.l: the Lisp-machine way to specialize) to pipe the child's
;; stdout into the current buffer, streaming with a redisplay per chunk, and
;; to map C-c on the graphical keyboard to SIGINT for the child.
;; (Interactive stdin still comes from the serial console -- a full in-buffer
;; comint is a later refinement.)

(defun stream-thunk (thunk)
  "Run THUNK in a forked child wired to TWO pipes -- the child's fd 1 streams
into the current buffer (output), and the frame's keyboard streams into the
child's fd 0 (input). The terminal, finally complete inside the frame: while
the child runs, what you type is echoed into the buffer and edited in a COOKED
line (backspace fixes it locally, the program never sees the editing); RET
delivers the line, C-d is end-of-input, and C-c interrupts the whole job. The
general mechanism behind EVERY command in the frame: single programs and whole
pipelines alike.

Before this, a frame child inherited the frame's fd 0 -- the serial console
nobody was typing on -- so anything that READ stdin (wc, a prompt) hung. Now
the input comes from the very keyboard you are using."
  (let ((out (pipe))                ; child stdout -> (car out), read by us
        (inp (pipe)))               ; child stdin  <- (cdr inp), written by us
    (let ((pid (fork)))
      (if (= pid 0)
          (progn (setpgid 0 0)               ; become a JOB leader: programs we
                                             ; fork inherit the group, so one
                                             ; group-kill interrupts them all
                 (setq in-pipeline t)        ; descendants run raw
                 (dup2 (cdr out) 1)          ; our stdout is the output pipe
                 (dup2 (car inp) 0)          ; our stdin is the input pipe
                 (close (car out)) (close (cdr out))
                 (close (car inp)) (close (cdr inp))
                 (let ((v (funcall thunk)))
                   (exit (if (numberp v) v 0))))
        (progn
          (setpgid pid pid)                  ; parent sets it TOO (the classic
                                             ; shell double-set: no race if C-c
                                             ; lands before the child runs)
          (close (cdr out))                  ; we only READ the child's output
          (close (car inp))                  ; and only WRITE its input
          ;; sin = is our stdin write end still open? C-d closes it early; if
          ;; the child exits first we close it after the loop. Closing twice
          ;; would be a bug, so we track it. line = the COOKED input line: a
          ;; canonical terminal buffers a line and lets you edit it (backspace)
          ;; before delivering it on RET, so the program never sees the editing.
          (let ((running t) (sin t) (line ""))
            (while running
              (if (poll-fd (car out) 50)
                  (let ((chunk (fd-read (car out) 1024)))
                    (if chunk
                        (progn (insert chunk) (redisplay))
                      (setq running nil)))
                ;; Quiet 50ms: nothing arrived, but if a live surface is
                ;; animating (the teapot) we must STILL repaint -- redisplay
                ;; is what blits surface canvases, and a slow-printing child
                ;; (ping: one line a second) must not become the frame rate.
                (if animate (redisplay) nil))
              (let ((ev (read-event-nowait)))
                (cond
                  ((null ev) nil)
                  ((and (eq (car ev) 'ctrl) (= (cadr ev) 99))   ; C-c
                   ;; Signal the GROUP (-pid), not the pid: our child is only
                   ;; a Lisp wrapper -- the program doing the work (ping) is
                   ;; its child, and killing the wrapper alone would orphan
                   ;; it, still streaming. SIGINT to -pid reaches them all.
                   (kill (- 0 pid) 2)
                   (echo "C-c: interrupted"))
                  ((and (eq (car ev) 'ctrl) (= (cadr ev) 100))  ; C-d: EOF
                   ;; Canonical EOF: a pending (unsent) line is delivered NOW
                   ;; without a newline; on an empty line we close the write end
                   ;; so the child's next read returns 0 -- end of input. (Real
                   ;; ttys do exactly this: C-d twice ends input mid-line.)
                   (if (> (string-length line) 0)
                       (progn (if sin (fd-write (cdr inp) line) nil) (setq line ""))
                     (if sin (progn (close (cdr inp)) (setq sin nil)) nil)))
                  ((eq (car ev) 'char)
                   (let ((ch (cadr ev)))
                     (cond
                       ((= ch 10)                  ; RET: deliver the cooked line
                        (insert (string-from-char 10)) (redisplay)
                        (if sin (fd-write (cdr inp)
                                          (string-concat line (string-from-char 10)))
                          nil)
                        (setq line ""))
                       ((= ch 8)                   ; backspace: edit locally only
                        (if (> (string-length line) 0)
                            (progn (setq line (substring line 0
                                                          (- (string-length line) 1)))
                                   (delete-char 1) (redisplay))
                          nil))
                       (t                          ; ordinary char: echo + buffer
                        (insert (string-from-char ch)) (redisplay)
                        (setq line (string-concat line (string-from-char ch)))))))
                  (t nil))))
            (close (car out))
            (if sin (close (cdr inp)) nil)   ; child exited before C-d
            (cdr (wait))))))))

(defun run-prog (path argv)
  "Frame edition: stream output into the buffer -- unless we ARE a pipeline
stage, where fds were already plumbed and raw inheritance is the contract."
  (if in-pipeline
      (run-prog-raw path argv)
    (stream-thunk (lambda () (run-prog-raw path argv)))))

(defun pipe-stages (left right)
  "Frame edition: the whole pipeline's output streams into the buffer."
  (if in-pipeline
      (pipe-stages-raw left right)
    (stream-thunk (lambda () (pipe-stages-raw left right)))))

(defun flush-output ()
  "Drain captured image output -- (princ ...) text and ERROR lines -- into
the current buffer, where the person at the frame can actually see it."
  (let ((out (frame-output)))
    (if (> (string-length out) 0)
        (progn (insert out) (redisplay))
      nil)))

(defun frame-setup ()
  "Acquire the screen and greet. Returns nil when there is no display."
  (if (frame-init)
      (progn
        (setq repl-buf (current-buffer))   ; the REPL is whatever frame-init selected
        (insert "MyOSv2 Graphical Lisp Machine\n")
        (insert "C-x 2/3 split  C-x 0 delete  C-x o next  mouse-1 select\n\n")
        (prompt)
        (echo "ready")
        (redisplay)
        t)
    (progn (princ "frame: no display\n") nil)))

;; When t, the event loop polls input at ~25Hz instead of blocking, so
;; surface buffers with live renderers (the teapot!) keep repainting.
(setq animate nil)

(defun teapot ()
  "The Utah teapot, spinning in a buffer: split the window, give the new
half a surface canvas, and run /bin/teapot (TinyGL) into it."
  (split-below)
  (other-window)
  (let ((sb (make-surface-buffer "*teapot*" 480 360)))
    (set-buffer sb)
    (other-window)
    (setq animate t)
    (run-in-buffer sb "teapot")))

(defun frame-tick ()
  "ONE beat of the machine: show anything printed since last time (including
an error from the previous tick -- each tick gets a fresh C-level recovery,
so a typo aborts one keystroke's work, never the loop), handle one event,
show its output, repaint. read-event BLOCKS in the kernel; redisplay paints
only what changed -- an idle machine costs nothing."
  (let ((pending (frame-output)))
    (if (> (string-length pending) 0)
        ;; Output pending at the TOP of a tick means the previous tick was
        ;; aborted mid-eval (a typo): show what it printed -- the ERROR line
        ;; -- and lay a fresh prompt, which also re-anchors repl-start past
        ;; the broken input so the next RET doesn't re-eval it.
        (progn (insert pending) (insert "\n") (prompt) (redisplay))
      nil))
  (if animate
      ;; Animation heartbeat: poll input, repaint at ~25Hz so live surface
      ;; renderers show. (The blocking path below stays the idle default.)
      (progn (let ((ev (read-event-nowait)))
               (if ev (dispatch ev) nil))
             (sleep 40))
    (dispatch (read-event)))
  (flush-output)
  (redisplay))

(princ "frame.l loaded.")
(terpri)
