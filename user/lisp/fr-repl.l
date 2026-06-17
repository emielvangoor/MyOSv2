;;; fr-repl.l -- the REPL: per-buffer state, prompt, eval, input history.

;; REPL state, kept PER BUFFER so each split window is an independent REPL.
;; "Being a REPL" is no longer a list-membership flag: a buffer is a REPL when
;; its major-mode is repl-mode (see modes.l). Where each REPL's prompt left off
;; is the buffer-local `repl-start` (read/written via cur-start/set-cur-start).
;; Input history is shared across REPLs (one command history), newest-first;
;; repl-hist-idx is the position being shown (-1 = the live line).
(setq repl-history nil)
(setq repl-hist-idx -1)

(defun cur-start ()
  "Where the SELECTED buffer's REPL input begins (0 if it has no prompt yet)."
  (buffer-local-get (current-buffer) 'repl-start 0))

(defun set-cur-start (pos)
  "Record the current buffer's input start."
  (buffer-local-set (current-buffer) 'repl-start pos))

(defun prompt ()
  "Show a fresh prompt and remember where input starts (in THIS buffer)."
  (insert "lisp> ")
  (set-cur-start (buffer-length)))

(defun repl-eval ()
  "Evaluate the text after the last prompt; print its value; re-prompt.
The whole REPL in one function: buffer-substring lifts the input out of
the buffer, read-string turns text into a form, eval runs it."
  (let ((src (buffer-substring (cur-start) (buffer-length))))
    ;; Remember non-empty input, and leave history-navigation mode: the next
    ;; Up starts again from the most recent entry.
    (if (> (string-length src) 0)
        (setq repl-history (cons src repl-history)) nil)
    (setq repl-hist-idx -1)
    ;; Point may be mid-input after cursor motion (C-a, C-b, ...); the result
    ;; and next prompt must go at the END, below the input, not wherever the
    ;; cursor happens to sit -- otherwise we splice them into the line.
    (goto-char (buffer-length))
    (insert "\n")
    (let ((form (read-string src)))
      ;; A REPL is a process in a buffer: (exit) (or bare `exit`) CLOSES this
      ;; buffer instead of evaluating -- like typing `exit` in a shell. (The
      ;; eval'd `exit` primitive is separately guarded to never exit the frame
      ;; process; this comint-style close is the REPL-layer behavior.)
      (if (or (eq form 'exit) (and (consp form) (eq (car form) 'exit)))
          (if (kill-buffer)
              (redisplay)                       ; closed -> window shows what's left
            (progn (insert "exit: this is the last buffer\n") (prompt)))
        (let ((val (eval form)))
          (flush-output)              ; princ'd text lands HERE, before the value
          (insert (prin1-to-string val))
          (insert "\n")
          (prompt))))))

(defun repl-set-input (s)
  "Replace the current REPL input (everything after the prompt) with S."
  (let ((n (- (buffer-length) (cur-start))))
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

(defun new-repl ()
  "Turn the selected window's buffer into a FRESH REPL buffer in repl-mode."
  (interactive)
  (let ((b (make-buffer "*repl*")))
    (set-major-mode b 'repl-mode)                         ; prompts via repl-setup
    b))

(defun repl ()
  "M-x repl: open a fresh REPL in its own *repl* buffer (Emacs-style command).
In the frame this shadows system.l's blocking demo `repl`, whose read loop
would freeze the event loop."
  (interactive)
  (new-repl))

(defun repl-here ()
  "C-x r: turn the current window's buffer into a fresh REPL (clear + prompt)."
  (interactive)
  (goto-char (buffer-length)) (delete-char (buffer-length))   ; clear the buffer
  (set-major-mode (current-buffer) 'repl-mode))

(defun split-repl-below ()
  "C-x 2: split, and give the new (lower) window its own fresh REPL."
  (interactive)
  (split-below) (other-window) (new-repl) (redisplay))

(defun split-repl-right ()
  "C-x 3: split side by side, fresh REPL in the new (right) window."
  (interactive)
  (split-right) (other-window) (new-repl) (redisplay))
