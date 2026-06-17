;;; frame.l -- the graphical machine's event loop and REPL (Phase 25.4).
;;;
;;; Everything a key DOES is decided here, in Lisp -- C only reports what the
;;; key IS ((char 97), (ctrl 120), (mouse x y)). This is the Emacs deal: the
;;; redisplay engine and the input cooker are C machinery; the personality of
;;; the machine -- keymaps, the REPL, what mouse-1 means -- is live Lisp you
;;; can redefine from this very REPL (or over the network).
;;;
;;; Loaded by `lisp -frame` after bootstrap.l and system.l.
;;;
;;; The machine's personality is split across focused pieces, loaded here.
;;; (Load order does not matter for symbol references -- defuns resolve at call
;;; time -- but this order reads top-down; register-mode/register-minor-mode are
;;; defined in modes.l, loaded before frame.l, so fr-modes.l's registrations
;;; work.) The running-programs machinery (surface/teapot/pipelines/event loop)
;;; stays in this file, below.
(load "/lib/fr-repl.l")
(load "/lib/fr-edit.l")
(load "/lib/fr-modes.l")
(load "/lib/fr-keys.l")
(load "/lib/fr-files.l")
(load "/lib/fr-mini.l")
(load "/lib/fr-help.l")

;; ---- ANSI SGR escapes -> face text properties (the ansi-color.el analog) --
;;
;; Programs like `ls` style their output with ESC[..m (SGR) sequences. Emacs
;; turns those into `face` text properties via ansi-color.el; we do the same:
;; ansi-color-apply strips the escape bytes and returns the text split into
;; "pieces" -- plain strings, or propertized strings carrying the active ANSI
;; faces. The frame's output streamer inserts the pieces, so the renderer paints
;; them in color (Component 4 of the text-properties design). State is carried
;; across chunks: a slow child's output arrives 1024 bytes at a time and an
;; escape can straddle a chunk boundary.

(defun ansi-setup-faces ()
  "Register the themeable ANSI palette as named faces. Run AFTER frame-init (the
face registry must exist). Colors are 0x00RRGGBB as decimals (the reader has no
hex); recolor any of these with (set-face-attribute 'ansi-blue ':foreground N)."
  (defface 'ansi-black   ':foreground 2631720)   (defface 'ansi-red     ':foreground 13378589)
  (defface 'ansi-green   ':foreground 10000154)  (defface 'ansi-yellow  ':foreground 14129441)
  (defface 'ansi-blue    ':foreground 4556168)   (defface 'ansi-magenta ':foreground 11625094)
  (defface 'ansi-cyan    ':foreground 6856042)   (defface 'ansi-white   ':foreground 11049348)
  (defface 'ansi-bright-black   ':foreground 9601908)   (defface 'ansi-bright-red     ':foreground 16468276)
  (defface 'ansi-bright-green   ':foreground 12106534)  (defface 'ansi-bright-yellow  ':foreground 16432431)
  (defface 'ansi-bright-blue    ':foreground 8627608)   (defface 'ansi-bright-magenta ':foreground 13862555)
  (defface 'ansi-bright-cyan    ':foreground 9355388)   (defface 'ansi-bright-white   ':foreground 15457202)
  (defface 'ansi-bold ':bold t) (defface 'ansi-underline ':underline t) (defface 'ansi-inverse ':inverse t))

;; Active style state (carried across chunks). fg is a single face symbol.
(setq *ansi-fg* nil) (setq *ansi-bold* nil) (setq *ansi-ul* nil) (setq *ansi-inv* nil)
(setq *ansi-frag* "")        ; an incomplete trailing escape, prepended to the next chunk
(setq *ansi-colors*
  (list 'ansi-black 'ansi-red 'ansi-green 'ansi-yellow
        'ansi-blue 'ansi-magenta 'ansi-cyan 'ansi-white))
(setq *ansi-bright*
  (list 'ansi-bright-black 'ansi-bright-red 'ansi-bright-green 'ansi-bright-yellow
        'ansi-bright-blue 'ansi-bright-magenta 'ansi-bright-cyan 'ansi-bright-white))

(defun ansi-reset ()
  (setq *ansi-fg* nil) (setq *ansi-bold* nil) (setq *ansi-ul* nil) (setq *ansi-inv* nil))

(defun ansi-apply-code (n)
  "Update the active style for one SGR parameter N (bg/256-color codes ignored)."
  (cond ((= n 0) (ansi-reset))
        ((= n 1) (setq *ansi-bold* t))
        ((= n 4) (setq *ansi-ul* t))
        ((= n 7) (setq *ansi-inv* t))
        ((= n 22) (setq *ansi-bold* nil))
        ((= n 24) (setq *ansi-ul* nil))
        ((= n 27) (setq *ansi-inv* nil))
        ((= n 39) (setq *ansi-fg* nil))
        ((and (>= n 30) (<= n 37)) (setq *ansi-fg* (nth (- n 30) *ansi-colors*)))
        ((and (>= n 90) (<= n 97)) (setq *ansi-fg* (nth (- n 90) *ansi-bright*)))
        (t nil)))

(defun ansi-faces ()
  "The list of active ANSI face symbols (fg first, then attributes), or nil."
  (let ((fs nil))
    (if *ansi-inv* (setq fs (cons 'ansi-inverse fs)) nil)
    (if *ansi-ul*  (setq fs (cons 'ansi-underline fs)) nil)
    (if *ansi-bold* (setq fs (cons 'ansi-bold fs)) nil)
    (if *ansi-fg*  (setq fs (cons *ansi-fg* fs)) nil)
    fs))

(defun ansi-piece (txt)
  "TXT wrapped in the current face(s), or plain TXT when no style is active."
  (let ((fs (ansi-faces)))
    (if fs (propertize txt 'face fs) txt)))

(defun ansi-parse-params (p)
  "Parse a `;`-separated decimal SGR parameter string and apply each code.
An empty string (ESC[m) means reset, code 0."
  (let ((n (string-length p)) (i 0) (cur 0) (have nil) (saw nil))
    (while (< i n)
      (let ((c (string-ref p i)))
        (cond ((= c 59)                                   ; ';' -> end of a param
               (ansi-apply-code (if have cur 0)) (setq cur 0) (setq have nil) (setq saw t))
              ((and (>= c 48) (<= c 57))                  ; digit
               (setq cur (+ (* cur 10) (- c 48))) (setq have t))
              (t nil)))
      (setq i (+ i 1)))
    (if (or have saw) (ansi-apply-code (if have cur 0)) (ansi-apply-code 0))))

(defun ansi-color-apply (chunk)
  "Translate the SGR escapes in CHUNK into face text properties. Returns a LIST
of pieces (plain or propertized strings) to insert in order; escape bytes are
stripped, and an incomplete trailing escape is stashed for the next call."
  (let ((s (string-concat *ansi-frag* chunk)))
    (setq *ansi-frag* "")
    (let ((n (string-length s)) (i 0) (run 0) (pieces nil))
      (while (< i n)
        (if (= (string-ref s i) 27)                       ; ESC
            (progn
              (if (> i run)                                ; flush the literal run before ESC
                  (setq pieces (cons (ansi-piece (substring s run i)) pieces)) nil)
              (if (>= (+ i 1) n)
                  (progn (setq *ansi-frag* (substring s i n)) (setq run n) (setq i n))
                (if (= (string-ref s (+ i 1)) 91)          ; '[' -> a CSI sequence
                    (let ((j (+ i 2)))
                      (while (and (< j n)
                                  (let ((cc (string-ref s j))) (or (< cc 64) (> cc 126))))
                        (setq j (+ j 1)))
                      (if (>= j n)                          ; final byte not arrived yet
                          (progn (setq *ansi-frag* (substring s i n)) (setq run n) (setq i n))
                        (progn
                          (if (= (string-ref s j) 109)      ; 'm' -> SGR; else non-style CSI
                              (ansi-parse-params (substring s (+ i 2) j)) nil)
                          (setq i (+ j 1)) (setq run i))))
                  (progn (setq i (+ i 2)) (setq run i)))))  ; ESC not '[': drop both bytes
          (setq i (+ i 1))))
      (if (> i run) (setq pieces (cons (ansi-piece (substring s run i)) pieces)) nil)
      (reverse pieces))))

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
                 (dup2 (cdr out) 2)          ; and stderr too, so errors/usage
                                             ; land in the buffer, not the serial
                                             ; port (busybox prints help to fd 2)
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
          (ansi-reset) (setq *ansi-frag* "") ; fresh ANSI style state per program
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
                        ;; Translate the child's ANSI color escapes into face
                        ;; text properties as we stream it into the buffer.
                        (progn (foreach (lambda (pc) (insert pc)) (ansi-color-apply chunk))
                               (redisplay))
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
  "Acquire the screen, then open *scratch* in lisp-interaction-mode (Emacs-style)."
  (if (frame-init)
      (progn
        (ansi-setup-faces)                                ; register the ANSI palette
        (let ((sb (make-buffer "*scratch*")))
          (set-major-mode sb 'lisp-interaction-mode))     ; selects + banner
        (echo "ready")
        (redisplay)
        t)
    (progn (princ "frame: no display\n") nil)))

;; When t, the event loop polls input at ~25Hz instead of blocking, so
;; surface buffers with live renderers (the teapot!) keep repainting.
(setq animate nil)

(defun make-surface (name w h)
  "Make a surface (pixel-canvas) buffer in surface-mode. The Lisp face of the
make-surface-buffer C primitive: it allocates the shm canvas; we put the buffer
in surface-mode so keys are inert and the mode line reads (Surface)."
  (let ((b (make-surface-buffer name w h)))
    (if b (set-major-mode b 'surface-mode) nil)
    b))

(defun teapot ()
  "The Utah teapot, spinning in a buffer: split the window, give the new
half a surface canvas, and run /bin/teapot (TinyGL) into it."
  (interactive)
  (split-below)
  (other-window)
  (let ((sb (make-surface "*teapot*" 480 360)))
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
        (progn (insert pending) (insert "\n")
               (if (eq (buffer-mode (current-buffer)) 'repl-mode) (prompt) nil)
               (redisplay))
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
