;;; fr-modes.l -- the major modes and the minor-mode machinery.

;; ---- the major modes (the MODEL is in modes.l; definitions live here) ----
;; Each mode names handler functions that touch the buffer (insert, prompt,
;; eval). The descriptor order is
;;   (parent keymap on-self-insert on-return setup-fn doc mode-line-name).
;; Hierarchy (behavior axis): special-mode is the inert root; surface-mode and
;; fundamental-mode are its two children. "Graphics is the substrate" lives on
;; the RENDERING axis (rd_core paints pixels for all buffers), not here.
;;   special-mode
;;     surface-mode          (a pixel canvas; ignores typing)
;;     fundamental-mode      (adds text self-insert / RET)
;;       text / repl / lisp-interaction

;; The basic text handlers, shared by every mode (in read-only modes they're
;; simply never reached -- dispatch blocks modification, the Emacs way).
(defun fundamental-self-insert (ch) (insert (string-from-char ch)))
(defun fundamental-return () (insert "\n"))

;; Descriptor index 7 is the read-only marker: 'ro (navigable, not editable) or
;; 'rw (editable), inherited up the parent chain like every other field. This
;; is Emacs's buffer-read-only model -- special/surface buffers move point fine
;; (C-a/C-n work) but interactive insert/delete is refused. special-mode is the
;; read-only root; fundamental-mode flips to 'rw, so its children (text/repl/
;; lisp-interaction) are editable while surface (a child of special) stays 'ro.
(register-mode 'special-mode
  (list nil nil 'fundamental-self-insert 'fundamental-return nil
        "Special: a displayed but read-only buffer -- navigable, not editable.
ctrl/meta keys fall through to the global keymap."
        "Special" 'ro))

(register-mode 'surface-mode
  (list 'special-mode nil nil nil nil
        "Surface: a pixel canvas an external renderer (e.g. the teapot) draws
into. Read-only -- it is not text."
        "Surface"))                          ; inherits 'ro from special-mode

(register-mode 'fundamental-mode
  (list 'special-mode nil 'fundamental-self-insert 'fundamental-return nil
        "Fundamental mode: text self-inserts; RET inserts a newline."
        "Fundamental" 'rw))                  ; editable (overrides special's 'ro)

(register-mode 'text-mode
  (list 'fundamental-mode nil nil nil nil
        "Text mode: plain text editing. C-x C-s saves to the visited file."
        "Text"))

(defun repl-self-insert (ch) (insert (string-from-char ch)))
(defun repl-return () (repl-eval))
(defun repl-setup () (goto-char (buffer-length)) (prompt))
(setq repl-keymap
  (list
    (list 'ctrl 112 'cmd-history-prev "C-p    previous input (history)")
    (list 'ctrl 110 'cmd-history-next "C-n    next input (history)")))
(register-mode 'repl-mode
  (list 'fundamental-mode repl-keymap 'repl-self-insert 'repl-return 'repl-setup
        "REPL mode: RET evaluates the input; C-p/C-n walk the input history."
        "REPL"))

(defun scratch-setup ()
  (insert ";; *scratch* -- type a Lisp form and press C-j to evaluate it.\n\n"))
(setq lisp-interaction-keymap
  (list (list 'ctrl 106 'eval-last-sexp "C-j    eval the form before point")))
(register-mode 'lisp-interaction-mode
  (list 'fundamental-mode lisp-interaction-keymap nil nil 'scratch-setup
        "Lisp Interaction: RET=newline; C-j evaluates the form before point."
        "Lisp Interaction"))

;; ---- minor modes: buffer-local toggles that compose with the major mode ----
;; A minor mode is a symbol with a registry entry (lighter keymap effect-fn):
;; the lighter is the tag it adds to the mode line, the keymap (may be nil) is
;; consulted by dispatch BEFORE the major-mode chain (Emacs precedence), and the
;; effect-fn is run with t/nil whenever the mode turns on/off. Which minors are
;; ACTIVE is a buffer-local list ('minor-modes), so each buffer toggles its own.
(setq *minor-modes* nil)   ; alist (sym . (lighter keymap effect-fn))

(defun register-minor-mode (sym lighter keymap effect-fn)
  "Register minor mode SYM: LIGHTER (mode-line tag), optional KEYMAP, and
EFFECT-FN called with t/nil when the mode turns on/off."
  (setq *minor-modes* (cons (cons sym (list lighter keymap effect-fn))
                            (filter (lambda (e) (not (eq (car e) sym))) *minor-modes*)))
  sym)

(defun minor-lighter (sym) (nth 0 (cdr (assoc sym *minor-modes*))))
(defun minor-keymap (sym)  (nth 1 (cdr (assoc sym *minor-modes*))))
(defun minor-effect-fn (sym) (nth 2 (cdr (assoc sym *minor-modes*))))

(defun active-minor-modes () (buffer-local-get (current-buffer) 'minor-modes nil))
(defun minor-mode-on-p (sym) (member sym (active-minor-modes)))

(defun mode-line-string ()
  "Compose '<Major> <Lighter1> <Lighter2>...' for the current buffer."
  (let ((s (mode-line-name-of (buffer-mode (current-buffer)))))
    (foreach (lambda (m) (setq s (string-concat s " " (minor-lighter m))))
             (active-minor-modes))
    s))

(defun refresh-mode-line () (set-mode-line-name (mode-line-string)))

(defun set-major-mode (buf mode-sym)
  "Make BUF use MODE-SYM: record it (buffer-local), select it, paint the mode
line (major name + any active minor lighters), and run the mode's setup-fn."
  (buffer-local-set buf 'major-mode mode-sym)
  (buffer-local-set buf 'read-only (eq (mode-read-only mode-sym) 'ro))  ; Emacs read-only model
  (set-buffer buf)
  ;; refresh-mode-line reads buffer-mode (just set) + active minors, so the bar
  ;; always reflects major + minors -- not just the bare major name.
  (refresh-mode-line)
  ;; funcall needs a function VALUE, not a symbol; (eval (list s)) calls the
  ;; setup fn named by the symbol s (s sits in function position for eval).
  (let ((s (mode-setup-fn mode-sym))) (if s (eval (list s)) nil))
  mode-sym)

(defun toggle-minor-mode (sym)
  "Flip minor mode SYM in the current buffer: update the active list, run its
effect, repaint the mode line, redisplay, and echo the new state."
  (let ((on (not (minor-mode-on-p sym))))
    (buffer-local-set (current-buffer) 'minor-modes
      (if on (cons sym (active-minor-modes))
        (filter (lambda (m) (not (eq m sym))) (active-minor-modes))))
    ;; effect-fn is stored as a SYMBOL; this Lisp's funcall needs a function
    ;; VALUE, so call it the way set-major-mode calls its setup-fn: build the
    ;; form (effect-fn on) and eval it.
    (let ((fx (minor-effect-fn sym))) (if fx (eval (list fx on)) nil))
    (refresh-mode-line)
    (redisplay)
    (echo (string-concat (symbol-name sym) (if on " enabled" " disabled")))))

(defun line-wrap-effect (on) (set-line-wrap (if on 1 0)))
(register-minor-mode 'line-wrap "Wrap" nil 'line-wrap-effect)

(defun line-wrap-mode ()
  "Toggle line wrapping in the current buffer (minor mode; lighter Wrap)."
  (interactive)
  (toggle-minor-mode 'line-wrap))
