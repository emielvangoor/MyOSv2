;;; fr-help.l -- self-documenting help (C-h k/b/m) + minibuffer commit/dispatch.

;; ---- self-documenting help: C-h k / C-h b / C-h m -----------------------

(defun fmt-bindings (km)
  "One description line per binding in KM."
  (let ((s ""))
    (foreach (lambda (b) (setq s (string-concat s (nth 3 b) "\n"))) km)
    s))

(defun list-buffers ()
  "C-x C-b: every live buffer in a *Help* window, the current one marked '*'."
  (interactive)
  (let ((here (current-buffer))
        (txt "=== Buffers ===\n\n"))
    (foreach (lambda (e)
               (setq txt (string-concat txt
                 (if (= (car e) here) "* " "  ")
                 (prin1-to-string (car e)) "  " (cdr e) "\n")))
             (buffer-list))
    (show-help txt)))

(defun describe-bindings ()
  "C-h b: every key binding (current mode's keymap + the global maps)."
  (interactive)
  (show-help (string-concat
    "=== Key bindings ===\n\n"
    "-- " (mode-line-name-of (buffer-mode (current-buffer))) " mode --\n"
    (fmt-bindings (mode-keymap (buffer-mode (current-buffer))))
    "\n-- global --\n"
    (fmt-bindings global-keymap)
    (fmt-bindings cx-keymap)
    (fmt-bindings help-keymap)
    "\nC-x      prefix\nC-h      prefix (help)\nC-g      quit\n"
    "M-x      run a command by name\nRET      mode-defined (eval / newline)\n")))

(defun describe-mode ()
  "C-h m: describe the selected buffer's mode (its doc + the keys it adds)."
  (interactive)
  (let ((m (buffer-mode (current-buffer))))
    (show-help (string-concat
      "=== " (mode-line-name-of m) " mode ===\n\n"
      (mode-doc m) "\n\n"
      "Mode keys:\n\n" (fmt-bindings (mode-keymap m))
      "\nGlobal keys:\n\n"
      (fmt-bindings global-keymap)
      (fmt-bindings cx-keymap)))))

(defun describe-binding (b prefix)
  (if b
      (show-help (string-concat prefix (nth 3 b) "\n\nruns " (symbol-name (nth 2 b))
                                "\n\n" (describe-function-text (nth 2 b))))
    (show-help (string-concat prefix "key is unbound (or self-inserts)"))))

(defun describe-key ()
  "C-h k: read a key (following a C-x / C-h prefix) and report the command it
runs, with that command's living source -- read from the same keymaps dispatch
uses, so it is always accurate."
  (interactive)
  (echo "Describe key: press a key")
  (redisplay)
  (let ((ev (read-event)))
    (cond
      ((and (eq (car ev) 'ctrl) (= (cadr ev) 120))
       (describe-binding (key-lookup (read-event) cx-keymap) "C-x "))
      ((and (eq (car ev) 'ctrl) (= (cadr ev) 104))
       (describe-binding (key-lookup (read-event) help-keymap) "C-h "))
      (t (describe-binding (key-lookup ev global-keymap) "")))))

(defun mb-commit ()
  (let ((sel (if mb-matches (nth mb-sel mb-matches) nil))
        (action mb-action)
        (input mb-input))
    (mb-stop)
    (cond
      ((eq action 'switch)
       (if sel (progn (set-buffer (car sel)) (redisplay)) (echo "no such buffer")))
      ((eq action 'find-file)                          ; the typed text IS the path
       (find-file-here input))
      ((eq action 'mx)
       (let ((sym (if sel sel (read-string input))))
         (let ((val (eval (list sym))))
           (flush-output)
           (echo (prin1-to-string val)))))
      ((eq action 'describe)
       (show-help (describe-function-text (if sel sel (read-string input))))))))

(defun mb-dispatch (ev)
  "The minibuffer keymap -- live data, like everything else here."
  (cond
    ((eq (car ev) 'ctrl)
     (cond ((= (cadr ev) 110) (mb-move 1) (mb-render))     ; C-n / Down
           ((= (cadr ev) 112) (mb-move -1) (mb-render))    ; C-p / Up
           ((= (cadr ev) 103) (mb-stop))                   ; C-g: cancel
           (t nil)))
    ((eq (car ev) 'char)
     (let ((ch (cadr ev)))
       (cond
         ((= ch 10) (mb-commit))                           ; RET
         ((= ch 9)                                         ; TAB: adopt selection
          (if mb-matches
              (progn (setq mb-input (mb-label (nth mb-sel mb-matches)))
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
