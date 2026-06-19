;;; fr-term.l -- M-x vterm: a Unix terminal in a buffer (the escape hatch).
;;;
;;; MyOSv2 is a Lisp machine, so a foreign full-screen program (vi, less, a
;;; shell) runs in a *buffer*, the Emacs way -- not a separate seat. The messy
;;; POSIX half (a pseudo-terminal + the VT100 screen model) lives in a tiny C
;;; helper, /bin/vterm, which links libvterm. This file is the pure-Lisp half:
;;; it starts the helper, mirrors the screen grid it streams into a buffer (text
;;; + ANSI faces, cursor = point), and forwards keystrokes back to it. No
;;; foreign C is linked into the frame -- we just talk to a process over pipes.
;;;
;;; Protocol (see user/musl/vterm.c):
;;;   us -> helper:  "u <codepoint> <mod>" / "k <keynum> <mod>" / "s <r> <c>" / "q"
;;;   helper -> us:  "z <r> <c>"  geometry        "c <r> <c> <vis>"  cursor
;;;                  "p <r> <c> <attrs> <fg> <bg> <text>"  a run of cells
;;;                  "f"  end of a damage batch  "x"  the shell exited

(setq *vterms* nil)        ; buffer handles of live vterms, polled each tick

;; ---- small numeric/string helpers (this Lisp has no number->string) --------
(defun n->s (n)
  "A non-negative or negative integer as a decimal string."
  (if (< n 0) (string-concat "-" (n->s (- 0 n)))
    (if (< n 10) (string-from-char (+ 48 n))
      (string-concat (n->s (/ n 10)) (string-from-char (+ 48 (% n 10)))))))

(defun vt-read-int (s i)
  "Skip spaces from index I in S, read an optional '-' and digits; return
(value . next-index)."
  (let ((n (string-length s)))
    (while (and (< i n) (= (string-ref s i) 32)) (setq i (+ i 1)))
    (let ((neg nil) (v 0))
      (if (and (< i n) (= (string-ref s i) 45)) (progn (setq neg t) (setq i (+ i 1))) nil)
      (while (and (< i n) (let ((c (string-ref s i))) (and (>= c 48) (<= c 57))))
        (setq v (+ (* v 10) (- (string-ref s i) 48)))
        (setq i (+ i 1)))
      (cons (if neg (- 0 v) v) i))))

(defun make-spaces (n)
  (let ((s "") (i 0)) (while (< i n) (setq s (string-concat s " ")) (setq i (+ i 1))) s))

;; ---- starting the helper ---------------------------------------------------
(defun vterm-spawn (rows cols)
  "Fork /bin/vterm wired to two pipes; return (pid out-read-fd in-write-fd).
out = helper's stdout (screen updates we read); in = helper's stdin (keys we
write)."
  (let ((out (pipe)) (inp (pipe)))
    (let ((pid (fork)))
      (if (= pid 0)
          (progn
            (dup2 (car inp) 0)               ; helper stdin  = our input pipe
            (dup2 (cdr out) 1)               ; helper stdout = our output pipe
            (close (car out)) (close (cdr out))
            (close (car inp)) (close (cdr inp))
            (exec "/bin/vterm" (list "vterm" (n->s rows) (n->s cols)))
            (exit 127))
        (progn
          (close (car inp))                  ; we only WRITE the helper's stdin
          (close (cdr out))                  ; and only READ its stdout
          (list pid (car out) (cdr inp)))))))

(defun vterm-setup ()
  "vterm-mode's setup-fn (run with the new buffer selected): start the helper
and register the buffer for per-tick polling."
  (let ((b (current-buffer)))
    (buffer-local-set b 'vt-rows 24)
    (buffer-local-set b 'vt-cols 80)
    (buffer-local-set b 'vt-frag "")
    (buffer-local-set b 'vt-char t)          ; char-mode: keys go to the program
    (let ((s (vterm-spawn 24 80)))
      (buffer-local-set b 'vt-pid (nth 0 s))
      (buffer-local-set b 'vt-out (nth 1 s))
      (buffer-local-set b 'vt-in  (nth 2 s)))
    (setq *vterms* (cons b *vterms*))
    (echo "vterm (char mode) -- C-x o leaves, M-x vterm-copy-mode to scroll")))

(defun vterm ()
  "M-x vterm: open a Unix terminal (sh, then vi/less/...) in a buffer."
  (interactive)
  (let ((b (make-buffer "*vterm*")))
    (set-major-mode b 'vterm-mode)))

;; ---- faces: an ANSI colour index + attr bits -> the frame's named faces ----
(defun vt-bit (attrs b) (= 1 (% (/ attrs b) 2)))           ; b = 1 bold, 2 ul, 4 inv
(defun vt-fg-face (idx) (if (< idx 8) (nth idx *ansi-colors*) (nth (- idx 8) *ansi-bright*)))
(defun vt-bg-face (idx) (if (< idx 8) (nth idx *ansi-bg-colors*) (nth (- idx 8) *ansi-bg-bright*)))
(defun vterm-face (attrs fg bg)
  "The list of face symbols for a cell run, or nil for the default style."
  (let ((fs nil))
    (if (vt-bit attrs 4) (setq fs (cons 'ansi-inverse fs)) nil)
    (if (vt-bit attrs 2) (setq fs (cons 'ansi-underline fs)) nil)
    (if (vt-bit attrs 1) (setq fs (cons 'ansi-bold fs)) nil)
    (if (>= bg 0) (setq fs (cons (vt-bg-face bg) fs)) nil)
    (if (>= fg 0) (setq fs (cons (vt-fg-face fg) fs)) nil)
    fs))

;; ---- applying one protocol line to the buffer ------------------------------
;; The buffer mirrors the screen: ROWS lines of COLS chars (+ newline). Cell
;; (r,c) sits at r*(COLS+1)+c. We overwrite a run in place (del-fwd then insert),
;; never touching the line's trailing newline -- the helper keeps runs within a
;; row, so the grid stays fixed-size and positions stay valid.
(defun vterm-init-grid (b rows cols)
  (set-buffer b)
  (buffer-local-set b 'vt-rows rows)
  (buffer-local-set b 'vt-cols cols)
  (goto-char 0) (del-fwd (buffer-length))
  ;; Build the whole blank grid as ONE string and insert it in a single edit:
  ;; many small inserts per tick overrun the redisplay's damage-rect budget.
  (let ((row (string-concat (make-spaces cols) (string-from-char 10))) (all "") (r 0))
    (while (< r rows) (setq all (string-concat all row)) (setq r (+ r 1)))
    (insert all)))

(defun vterm-put (b row col attrs fg bg text)
  (set-buffer b)
  (let ((C (buffer-local-get b 'vt-cols 80)) (len (string-length text)))
    (if (> len 0)
        (let ((pos (+ (* row (+ C 1)) col)))
          (goto-char pos) (del-fwd len)
          (goto-char pos) (insert text)
          (let ((fs (vterm-face attrs fg bg)))
            (if fs (set-text-properties pos (+ pos len) (list 'face fs))
              nil)))                            ; default style: inserted text has no face
      nil)))

(defun vterm-cursor (b row col)
  (set-buffer b)
  (let ((C (buffer-local-get b 'vt-cols 80)))
    (goto-char (+ (* row (+ C 1)) col))))

(defun vterm-died (b)
  "The helper/shell exited: unregister, close pipes, leave a notice."
  (setq *vterms* (filter (lambda (x) (not (= x b))) *vterms*))
  (let ((o (buffer-local-get b 'vt-out -1)) (i (buffer-local-get b 'vt-in -1)))
    (if (>= o 0) (close o) nil) (if (>= i 0) (close i) nil))
  (buffer-local-set b 'vt-in -1)
  (buffer-local-set b 'vt-char nil)
  (set-buffer b) (goto-char (buffer-length)) (insert "\n[vterm: exited]\n"))

(defun vterm-apply-line (b s)
  (if (= (string-length s) 0) nil
    (let ((k (string-ref s 0)) (n (string-length s)))
      (cond
        ((= k 112)                                   ; 'p' -- a cell run
         (let ((r (vt-read-int s 1)))
           (let ((c (vt-read-int s (cdr r))))
             (let ((a (vt-read-int s (cdr c))))
               (let ((fg (vt-read-int s (cdr a))))
                 (let ((bg (vt-read-int s (cdr fg))))
                   (vterm-put b (car r) (car c) (car a) (car fg) (car bg)
                              (substring s (+ (cdr bg) 1) n))))))))
        ((= k 99)                                    ; 'c' -- cursor
         (let ((r (vt-read-int s 1)))
           (vterm-cursor b (car r) (car (vt-read-int s (cdr r))))))
        ((= k 122)                                   ; 'z' -- geometry
         (let ((r (vt-read-int s 1)))
           (vterm-init-grid b (car r) (car (vt-read-int s (cdr r))))))
        ((= k 120) (vterm-died b))                   ; 'x' -- shell exited
        (t nil)))))                                  ; 'f' / unknown: nothing

;; ---- per-tick polling ------------------------------------------------------
;; CRUCIAL: string-concat caps its result at ~2048 bytes (a fixed C buffer), so
;; we must NEVER build a large accumulator string. Instead we read SMALL chunks
;; and apply their complete lines immediately, carrying only the trailing partial
;; line (well under one line) as 'vt-frag. frag + chunk therefore stays far below
;; 2048, so nothing is ever truncated, and a big repaint is just many small reads
;; in the loop below -- all applied this tick.
(defun vterm-feed (b chunk)
  "Apply the complete lines in vt-frag + CHUNK; keep the trailing partial line."
  (let ((s (string-concat (buffer-local-get b 'vt-frag "") chunk)))
    (let ((n (string-length s)) (start 0) (i 0))
      (while (< i n)
        (if (= (string-ref s i) 10)
            (progn (vterm-apply-line b (substring s start i)) (setq start (+ i 1)))
          nil)
        (setq i (+ i 1)))
      (buffer-local-set b 'vt-frag (substring s start n)))))

(defun vterm-poll (b)
  "Drain the helper's pipe this tick in small reads, applying each immediately."
  (let ((fd (buffer-local-get b 'vt-out -1)))
    (while (and (>= fd 0) (poll-fd fd 0))
      (let ((chunk (fd-read fd 1024)))
        (if chunk (vterm-feed b chunk)
          (progn (vterm-died b) (setq fd -1)))))))

(defun vterm-poll-all ()
  "Poll every live vterm, restoring the user's selected buffer afterward so a
background terminal's updates never steal the view."
  (if *vterms*
      (let ((orig (current-buffer)))
        (foreach (lambda (b) (vterm-poll b)) *vterms*)
        (set-buffer orig))
    nil))

;; ---- keystrokes -> the helper ----------------------------------------------
(defun vt-send (fd cmd) (fd-write fd (string-concat cmd (string-from-char 10))))

(defun vterm-feed-event (ev)
  "Forward one cooked key event to the focused vterm's helper as a protocol
command. Named keys (Enter/Tab/Backspace/Esc) use libvterm key numbers; ctrl/
meta become a unichar with the matching modifier bit."
  (let ((fd (buffer-local-get (current-buffer) 'vt-in -1)) (k (car ev)) (code (cadr ev)))
    (if (< fd 0) nil
      (cond
        ((eq k 'char)
         (cond ((= code 10) (vt-send fd "k 1 0"))           ; Enter
               ((= code 9)  (vt-send fd "k 2 0"))           ; Tab
               ((= code 8)  (vt-send fd "k 3 0"))           ; Backspace
               ((= code 27) (vt-send fd "k 4 0"))           ; Esc
               (t (vt-send fd (string-concat "u " (string-concat (n->s code) " 0"))))))
        ((eq k 'ctrl) (vt-send fd (string-concat "u " (string-concat (n->s code) " 4"))))
        ((eq k 'meta) (vt-send fd (string-concat "u " (string-concat (n->s code) " 2"))))
        (t nil)))))

(defun vterm-active-p ()
  "t when the current buffer is a live vterm in char mode (keys -> program)."
  (let ((b (current-buffer)))
    (and (eq (buffer-mode b) 'vterm-mode)
         (buffer-local-get b 'vt-char nil)
         (>= (buffer-local-get b 'vt-in -1) 0))))

(defun vterm-frame-key-p (ev)
  "Keys the FRAME keeps even in char mode, so you can always escape: C-x
(command prefix) and C-g (quit)."
  (and (eq (car ev) 'ctrl) (or (= (cadr ev) 120) (= (cadr ev) 103))))

(defun vterm-copy-mode ()
  "M-x vterm-copy-mode: stop forwarding keys, so the vterm buffer behaves like
ordinary read-only text you can scroll/search/yank. M-x vterm-char-mode returns."
  (interactive)
  (buffer-local-set (current-buffer) 'vt-char nil)
  (echo "vterm: copy mode (M-x vterm-char-mode to type)"))

(defun vterm-char-mode ()
  "M-x vterm-char-mode: resume sending keys to the program."
  (interactive)
  (buffer-local-set (current-buffer) 'vt-char t)
  (echo "vterm: char mode"))

;; ---- the major mode --------------------------------------------------------
;; Parent special-mode -> read-only/navigable: in copy mode you move point and
;; read; in char mode the dispatch hook (see fr-keys.l) forwards keys instead,
;; so the buffer is never edited directly.
(register-mode 'vterm-mode
  (list 'special-mode nil nil nil 'vterm-setup
        "Vterm: a Unix terminal in a buffer. In char mode keys go to the program
(C-x stays the frame's prefix so C-x o always escapes); M-x vterm-copy-mode
turns the buffer into ordinary scrollable text."
        "Vterm"))

(princ "fr-term.l loaded.")
(terpri)
