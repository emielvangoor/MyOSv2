;;; bootstrap.l -- the soul of the machine.
;;;
;;; This is the first Lisp loaded into the bare C core at startup, the way
;;; loadup.el bootstraps Emacs. It builds the higher-level library out of the
;;; handful of C primitives the kernel exposes. /bin/lisp loads it from
;;; /lib/bootstrap.l (placed there by the initrd) before handing you the REPL.

;; ---- Basic list operations ----

(defun cadr (x) (car (cdr x)))
(defun caddr (x) (car (cdr (cdr x))))
(defun caar (x) (car (car x)))
(defun cddr (x) (cdr (cdr x)))

(defun length (lst)
  (if (null lst) 0
    (+ 1 (length (cdr lst)))))

(defun append (a b)
  (if (null a) b
    (cons (car a) (append (cdr a) b))))

(defun reverse (lst)
  (if (null lst) nil
    (append (reverse (cdr lst)) (list (car lst)))))

(defun nth (n lst)
  (if (= n 0) (car lst)
    (nth (- n 1) (cdr lst))))

(defun nthcdr (n lst)
  (if (= n 0) lst
    (nthcdr (- n 1) (cdr lst))))

(defun member (item lst)
  (cond
    ((null lst) nil)
    ((equal item (car lst)) lst)
    (t (member item (cdr lst)))))

;; ---- Higher-order functions ----

(defun filter (pred lst)
  (cond
    ((null lst) nil)
    ((funcall pred (car lst))
     (cons (car lst) (filter pred (cdr lst))))
    (t (filter pred (cdr lst)))))

(defun reduce (fn init lst)
  (if (null lst) init
    (reduce fn (funcall fn init (car lst)) (cdr lst))))

(defun foreach (fn lst)
  (if (null lst) nil
    (progn
      (funcall fn (car lst))
      (foreach fn (cdr lst)))))

;; ---- Association lists ----

(defun assoc (key lst)
  (cond
    ((null lst) nil)
    ((equal key (car (car lst))) (car lst))
    (t (assoc key (cdr lst)))))

(defun acons (key val alist)
  (cons (cons key val) alist))

;; ---- Numeric utilities ----

(defun abs (n) (if (< n 0) (- 0 n) n))
(defun min (a b) (if (< a b) a b))
(defun max (a b) (if (> a b) a b))

(defun range (n)
  "Generate list (0 1 2 ... n-1)"
  (if (<= n 0) nil
    (append (range (- n 1)) (list (- n 1)))))

;; ---- Printing utilities ----

(defun newline () (terpri))

(defun println (x) (print x) x)

(defun display (x) (princ x) (terpri) x)

;; ---- The soul is loaded ----

(princ "bootstrap.l loaded.")
(terpri)
