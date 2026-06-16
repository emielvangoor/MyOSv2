;;; fr-mini.l -- the minibuffer (vertico-style) + (interactive) + show-help.

;; ---- the minibuffer: vertico-style command selection -------------------
;;
;; The echo area grows into a live-narrowing candidate list: type to filter,
;; C-n/C-p (or arrows) to move the selection bar, RET to commit, C-g to
;; cancel, TAB to adopt the selection as input. M-x completes over every
;; symbol with a function -- the symbol table IS the command palette.

(setq mb-action nil)        ; nil = inactive; 'mx or 'describe
(setq mb-input "")
(setq mb-matches nil)
(setq mb-sel 0)              ; selected candidate, an index into ALL matches
(setq mb-top 0)             ; first VISIBLE candidate -- the scroll offset
(setq mb-rows 8)            ; how many candidates the echo area shows at once

;; ---- (interactive): mark a defun as an M-x command (Emacs-style) ----------
;; A command declares itself by making (interactive) its FIRST body form. At
;; run time (interactive) is a harmless no-op; M-x reads it as data via
;; function-info to decide what to OFFER. This keeps M-x to real commands and
;; away from internal helpers and blocking loops (e.g. system.l's demo `repl`).
(defun interactive () nil)

(defun interactive-form-p (f)
  "t when F is the call (interactive ...)."
  (and (consp f) (eq (car f) 'interactive)))

(defun command-p (sym)
  "t when SYM is a defun declaring (interactive) as its first real body form.
defun stores a single-form body as-is and a multi-form body wrapped in
(progn ...). A docstring, if present, is the FIRST progn element (this Lisp
keeps it as a literal form rather than stripping it), so (interactive) may be
either the first OR the second progn element -- accept both."
  (let ((info (function-info sym)))
    (and info
         (eq (car info) 'lambda)
         (let ((body (caddr info)))
           (and (consp body)
                (or (eq (car body) 'interactive)            ; body IS (interactive)
                    (and (eq (car body) 'progn)             ; body is (progn ...)
                         (or (interactive-form-p (cadr body))     ; (progn (interactive) ...)
                             (interactive-form-p (caddr body))    ; (progn "doc" (interactive) ...)
                             ))))))))

(defun command-symbols ()
  "Every interactive command -- the M-x palette (see command-p / (interactive))."
  (filter (lambda (s) (command-p s)) (all-symbols)))

;; The minibuffer is generic over its candidates: each action supplies a
;; SOURCE (the full candidate list) and a LABEL (the display/match string for
;; one candidate). M-x/describe complete over command symbols; switch-to-buffer
;; completes over (handle . name) buffer entries.
(defun mb-source ()
  (cond ((eq mb-action 'switch) (buffer-list))
        ((eq mb-action 'find-file) nil)               ; free-text path entry
        (t (command-symbols))))

(defun mb-label (c)
  (if (eq mb-action 'switch) (cdr c) (symbol-name c)))

(defun mb-filter ()
  "Narrow the action's candidates to those whose label contains mb-input."
  (setq mb-matches
        (filter (lambda (c)
                  (or (= (string-length mb-input) 0)
                      (string-search mb-input (mb-label c))))
                (mb-source)))
  (setq mb-sel 0)
  (setq mb-top 0))

(defun take (n lst)
  (if (or (= n 0) (null lst)) nil
    (cons (car lst) (take (- n 1) (cdr lst)))))

(defun mb-move (d)
  "Move the selection by D over ALL matches, scrolling the visible window
(mb-top .. mb-top+mb-rows) to keep the selection inside it -- vertico's
behavior, the same minimal-scroll rule the buffer uses for point."
  (let ((n (length mb-matches)))
    (if (> n 0)
        (progn
          (setq mb-sel (+ mb-sel d))
          (if (< mb-sel 0) (setq mb-sel 0) nil)
          (if (>= mb-sel n) (setq mb-sel (- n 1)) nil)
          (if (< mb-sel mb-top) (setq mb-top mb-sel) nil)               ; scrolled up
          (if (>= mb-sel (+ mb-top mb-rows))                            ; scrolled down
              (setq mb-top (- mb-sel (- mb-rows 1))) nil))
      nil)))

(defun mb-render ()
  "Paint the minibuffer: prompt + a position counter on top, then the visible
slice of candidates (mb-top onward), the selected one highlighted (echo-select
drives the face-2 bar)."
  (let ((n (length mb-matches))
        (txt (string-concat
              (cond ((eq mb-action 'mx) "M-x ")
                    ((eq mb-action 'switch) "Buffer: ")
                    ((eq mb-action 'find-file) "Find file: ")
                    (t "Describe: "))
              mb-input)))
    (if (> n 0)
        (setq txt (string-concat txt "  " (prin1-to-string (+ mb-sel 1))
                                 "/" (prin1-to-string n))) nil)
    (foreach (lambda (c) (setq txt (string-concat txt "\n  " (mb-label c))))
             (take mb-rows (nthcdr mb-top mb-matches)))
    (echo txt)
    (echo-select (+ (- mb-sel mb-top) 1))))

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
           ;; *Help* is read-only special-mode: you can navigate it (C-n/C-p),
           ;; but not type into it. show-help fills it with the `insert`
           ;; PRIMITIVE below, which is programmatic and so not gated by the
           ;; read-only check (that only refuses interactive modification).
           (buffer-local-set help-buf 'major-mode 'special-mode)
           (buffer-local-set help-buf 'read-only t)
           (split-below)))
  (other-window)
  (set-buffer help-buf)
  (refresh-mode-line)    ; paint "(Special)" for the *Help* window
  (goto-char (buffer-length))
  (delete-char (buffer-length))
  (insert txt)
  (goto-char 0)          ; show *Help* from the TOP, like Emacs (scroll follows point)
  (other-window))
