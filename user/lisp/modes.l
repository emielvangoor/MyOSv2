;;; modes.l -- the major-mode MODEL (pure data; loads in the serial REPL too).
;;;
;;; A mode is a symbol with a descriptor stored in *modes*:
;;;   (parent keymap on-self-insert on-return setup-fn doc mode-line-name)
;;; Inherited fields resolve UP the parent chain (mode-field), so a derived
;;; mode only restates what it changes. Buffer-local variables live in
;;; *buffer-locals*, an alist keyed by buffer handle (a fixnum). This file has
;;; NO dependency on the graphics primitives, so the plain `lisp` REPL can load
;;; it and the mode model can be unit-tested without a frame.

;; ---- the mode registry --------------------------------------------------
(setq *modes* nil)            ; alist (mode-symbol . descriptor)

(defun register-mode (sym desc)
  "Register (or replace) mode SYM with descriptor DESC, a 7-element list:
(parent keymap on-self-insert on-return setup-fn doc mode-line-name)."
  (setq *modes* (cons (cons sym desc)
                      (filter (lambda (e) (not (eq (car e) sym))) *modes*)))
  sym)

(defun mode-desc (sym) (cdr (assoc sym *modes*)))
(defun mode-parent (sym) (nth 0 (mode-desc sym)))
(defun mode-keymap (sym) (nth 1 (mode-desc sym)))

(defun mode-field (sym idx)
  "Field IDX (>=2) of mode SYM, walking up the parent chain until non-nil.
Returns nil when the mode is unregistered or the chain reaches a nil parent."
  (let ((d (mode-desc sym)))
    (cond ((null d) nil)
          ((nth idx d) (nth idx d))
          (t (mode-field (mode-parent sym) idx)))))

;; Field-index accessors (parent=0 keymap=1 handled above).
(defun mode-self-insert-fn (sym) (mode-field sym 2))
(defun mode-return-fn (sym)      (mode-field sym 3))
(defun mode-setup-fn (sym)       (mode-field sym 4))
(defun mode-doc (sym)            (mode-field sym 5))
(defun mode-line-name-of (sym)   (mode-field sym 6))
(defun mode-read-only (sym)      (mode-field sym 7))   ; 'ro / 'rw, inherited

;; ---- keymaps (key-lookup MOVED here from frame.l so the serial REPL has it)
(defun key-lookup (ev km)
  "The binding for EV (a (kind code ...) event) in keymap KM, or nil."
  (cond ((null km) nil)
        ((and (eq (nth 0 (car km)) (car ev)) (= (nth 1 (car km)) (cadr ev)))
         (car km))
        (t (key-lookup ev (cdr km)))))

(defun mode-key-lookup (sym ev)
  "Look EV up in SYM's keymap chain (mode, then parents). nil if unbound."
  (cond ((null sym) nil)
        (t (let ((b (key-lookup ev (mode-keymap sym))))
             (if b b (mode-key-lookup (mode-parent sym) ev))))))

;; ---- buffer-local variables --------------------------------------------
(setq *buffer-locals* nil)    ; alist (handle . plist), plist = (sym val sym val ..)

(defun plist-get (pl sym dflt)
  "Value of SYM in property list PL (a flat list k1 v1 k2 v2 ...; keys compared
with eq), or DFLT when SYM is absent."
  (cond ((null pl) dflt)
        ((eq (car pl) sym) (cadr pl))
        (t (plist-get (cddr pl) sym dflt))))

(defun plist-put (pl sym val)
  "Return a NEW property list like PL but with SYM bound to VAL (replacing any
existing value, else appending). Purely functional -- PL is not mutated."
  (cond ((null pl) (list sym val))
        ((eq (car pl) sym) (cons sym (cons val (cddr pl))))
        (t (cons (car pl) (cons (cadr pl) (plist-put (cddr pl) sym val))))))

;; NOTE: buffer handles are always fixnums. buffer-local-get/-set use `assoc`
;; (which compares with `equal`) to FIND an entry and `filter` with `=` to
;; DROP the old one. Both predicates agree for fixnums; keep handles fixnums so
;; the two stay consistent (a symbol handle would be found but not replaced).
(defun buffer-local-get (buf sym dflt)
  (let ((e (assoc buf *buffer-locals*)))
    (if e (plist-get (cdr e) sym dflt) dflt)))

(defun buffer-local-set (buf sym val)
  (let ((e (assoc buf *buffer-locals*)))
    (setq *buffer-locals*
          (cons (cons buf (plist-put (if e (cdr e) nil) sym val))
                (filter (lambda (x) (not (= (car x) buf))) *buffer-locals*))))
  val)

(defun buffer-mode (buf) (buffer-local-get buf 'major-mode 'fundamental-mode))

(princ "modes.l loaded.")
(terpri)
