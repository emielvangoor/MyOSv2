;;; fr-keys.l -- keymaps as DATA, the command defuns, and the dispatcher.

;; ---- keymaps as DATA (the self-documenting heart) -----------------------
;; A binding is (kind code command description). dispatch looks a key up in a
;; keymap and runs its command; C-h b/k/m read the SAME tables -- so the help
;; can never drift from what the keys actually do. Commands are named functions
;; so C-h k can show each one's living source via describe-function-text.

(defun cmd-beginning-of-line () (interactive) (goto-char (bol-pos)))
(defun cmd-end-of-line () (interactive) (goto-char (eol-pos)))
(defun cmd-backward-char ()
  (interactive)
  (let ((lo (edit-lo))) (if (> (point) lo) (goto-char (- (point) 1)) nil)))
(defun cmd-forward-char ()
  (interactive)
  (if (< (point) (buffer-length)) (goto-char (+ (point) 1)) nil))
(defun cmd-delete-forward () (interactive) (if (read-only-p) (echo "buffer is read-only") (del-fwd 1)))
(defun cmd-forward-word () (interactive) (goto-char (fwd-word-pos)))
(defun cmd-backward-word () (interactive) (goto-char (bwd-word-pos)))
(defun cmd-history-prev () (interactive) (repl-history-prev))
(defun cmd-history-next () (interactive) (repl-history-next))
(defun cmd-describe-function () (interactive) (mb-start 'describe))

;; Line motion (C-p/C-n globally): move to the same column on the previous /
;; next line. repl-mode REBINDS C-p/C-n to history in its own keymap, which
;; dispatch consults before the global map -- so history is REPL-only and
;; never leaks into a file buffer.
(defun read-only-p ()
  "t when the current buffer is read-only (special/surface): interactive
modification is refused, but motion still works -- Emacs's model."
  (buffer-local-get (current-buffer) 'read-only nil))

(defun cur-column () (- (point) (bol-pos)))
(defun cmd-previous-line ()
  (interactive)
  (let ((col (cur-column)) (bol (bol-pos)))
    (if (> bol (edit-lo))
        (progn (goto-char (- bol 1))               ; end of the previous line
               (let ((pbol (bol-pos)) (plen (- (eol-pos) (bol-pos))))
                 (goto-char (+ pbol (if (< col plen) col plen)))))
      nil)))
(defun cmd-next-line ()
  (interactive)
  (let ((col (cur-column)) (eol (eol-pos)))
    (if (< eol (buffer-length))
        (progn (goto-char (+ eol 1))               ; start of the next line
               (let ((nbol (point)) (nlen (- (eol-pos) (point))))
                 (goto-char (+ nbol (if (< col nlen) col nlen)))))
      nil)))

(setq global-keymap
  (list
    (list 'ctrl 97  'cmd-beginning-of-line "C-a    beginning of line")
    (list 'ctrl 101 'cmd-end-of-line       "C-e    end of line")
    (list 'ctrl 98  'cmd-backward-char     "C-b    backward char (Left)")
    (list 'ctrl 102 'cmd-forward-char      "C-f    forward char (Right)")
    (list 'ctrl 100 'cmd-delete-forward    "C-d    delete forward char")
    (list 'ctrl 107 'kill-line             "C-k    kill to end of line")
    (list 'ctrl 112 'cmd-previous-line     "C-p    previous line (Up)")
    (list 'ctrl 110 'cmd-next-line         "C-n    next line (Down)")
    (list 'meta 102 'cmd-forward-word      "M-f    forward word")
    (list 'meta 98  'cmd-backward-word     "M-b    backward word")
    (list 'meta 100 'kill-word             "M-d    kill word")))

(defun cmd-delete-other-windows ()
  "C-x 1: the selected window fills the frame again. Any *Help* window is gone,
so forget help-buf and let show-help re-split next time."
  (delete-other-windows)
  (setq help-buf nil)
  (redisplay))

(defun switch-to-buffer ()
  "C-x b: pick a buffer by name in the minibuffer and show it here."
  (interactive)
  (mb-start 'switch))

(setq cx-keymap
  (list
    (list 'char 49  'cmd-delete-other-windows "C-x 1  delete other windows")
    (list 'char 50  'split-repl-below "C-x 2  split below (new REPL)")
    (list 'char 51  'split-repl-right "C-x 3  split right (new REPL)")
    (list 'char 48  'delete-window    "C-x 0  delete window")
    (list 'char 111 'other-window     "C-x o  other window")
    (list 'char 98  'switch-to-buffer "C-x b  switch to buffer")
    (list 'ctrl 98  'list-buffers     "C-x C-b  list buffers")
    (list 'ctrl 102 'cmd-find-file    "C-x C-f  find file")
    (list 'ctrl 115 'save-buffer      "C-x C-s  save buffer")
    (list 'char 114 'repl-here       "C-x r    open a REPL in this window")))

(setq help-keymap
  (list
    (list 'char 102 'cmd-describe-function "C-h f  describe function")
    (list 'char 107 'describe-key          "C-h k  describe key")
    (list 'char 98  'describe-bindings     "C-h b  list key bindings")
    (list 'char 109 'describe-mode         "C-h m  describe mode")))

(defun run-key (ev km)
  "If EV is bound in KM, run its command and return t; otherwise nil."
  (let ((b (key-lookup ev km)))
    (if b (progn (eval (list (nth 2 b))) t) nil)))

(defun handle-cx (ev)
  "The C-x map (data-driven). C-x C-s is two CTRL events, so we dispatch on
the whole event, not a bare char."
  (setq pending-cx nil)
  (if (run-key ev cx-keymap) nil (echo "unbound C-x key")))

(defun minor-maps-lookup (ev lst)
  "First binding for EV among the active minor modes' keymaps (a nil keymap is
inert: key-lookup returns nil for it), or nil if none bind it."
  (cond ((null lst) nil)
        ((key-lookup ev (minor-keymap (car lst))))   ; nil keymap -> key-lookup nil -> nil
        (t (minor-maps-lookup ev (cdr lst)))))

(defun run-mode-or-global (ev)
  "Run EV via active minor-mode maps, then the major-mode keymap chain, then global.
Minor maps take precedence over the major mode (Emacs order)."
  (let ((b (or (minor-maps-lookup ev (active-minor-modes))
               (mode-key-lookup (buffer-mode (current-buffer)) ev))))
    (if b (progn (eval (list (nth 2 b))) t)
      (if (run-key ev global-keymap) t (progn (echo "unbound key") nil)))))

(defun mode-backspace (m)
  "Backspace in mode M: in a REPL never past the prompt; else one char."
  (if (eq m 'repl-mode)
      (if (> (point) (cur-start)) (delete-char 1) nil)
    (if (> (point) 0) (delete-char 1) nil)))

(defun dispatch (ev)
  "Route one cooked input event through the selected buffer's major mode."
  (cond
    ((null ev) nil)
    (mb-action (mb-dispatch ev))                          ; minibuffer owns input
    (pending-cx (handle-cx ev))                           ; C-x <key>
    ((eq (car ev) 'mouse)
     (select-window-at (cadr ev) (caddr ev)))
    ;; A focused vterm in char mode swallows keys and forwards them to its
    ;; program -- EXCEPT C-x / C-g, which stay the frame's escape so you can
    ;; always C-x o away or M-x out. (Mouse handled above; minibuffer/C-x
    ;; sequences handled before this, so they still work.)
    ((and (vterm-active-p) (not (vterm-frame-key-p ev)))
     (vterm-feed-event ev))
    ((eq (car ev) 'meta)
     (cond ((= (cadr ev) 120) (mb-start 'mx))             ; M-x
           ((= (cadr ev) 10)                              ; Shift-RET: literal newline
            (if (read-only-p) (echo "buffer is read-only") (insert (string-from-char 10))))
           (t (run-mode-or-global ev))))
    (pending-ch
     (setq pending-ch nil)
     (if (run-key ev help-keymap) nil (echo "unbound C-h key")))
    ((eq (car ev) 'ctrl)
     (cond ((= (cadr ev) 120) (setq pending-cx t))        ; C-x: await suffix
           ((= (cadr ev) 104) (setq pending-ch t))        ; C-h: await suffix
           ((= (cadr ev) 103) (setq pending-cx nil)       ; C-g: quit
                              (echo "quit"))
           (t (run-mode-or-global ev))))                  ; mode keymap then global
    ((eq (car ev) 'char)
     (let ((m (buffer-mode (current-buffer))) (ch (cadr ev)))
       ;; Every char-branch action (RET=newline, backspace, self-insert) MODIFIES
       ;; the buffer, so a read-only buffer refuses them all -- motion (the ctrl
       ;; branch above) still works, the Emacs way.
       (if (read-only-p) (echo "buffer is read-only")
         (cond
           ((= ch 10) (eval (list (mode-return-fn m))))   ; RET: mode decides
           ((= ch 8) (mode-backspace m))                  ; backspace
           (t (eval (list (mode-self-insert-fn m) ch))))))) ; printable: mode decides
    (t nil)))
