;;; fr-edit.l -- basic Emacs motion/editing + lisp-interaction eval (C-j).

;; ---- basic Emacs motion + editing commands -----------------------------
;; These act on the current buffer; in the REPL the lower bound is the prompt
;; (repl-start), so motion and deletion never reach into the prompt text.

(defun edit-lo ()
  "Leftmost editable position: the prompt in a REPL, else 0."
  (if (eq (buffer-mode (current-buffer)) 'repl-mode) (cur-start) 0))

(defun word-char-p (c)
  (or (and (>= c 48) (<= c 57))          ; 0-9
      (and (>= c 65) (<= c 90))          ; A-Z
      (and (>= c 97) (<= c 122))))       ; a-z

(defun bol-pos ()
  "Start of the current line, clamped to the editable region (C-a)."
  (let ((p (point)) (lo (edit-lo)))
    (while (and (> p lo) (not (= (char-at (- p 1)) 10))) (setq p (- p 1)))
    p))

(defun eol-pos ()
  "End of the current line: the next newline or end of buffer (C-e)."
  (let ((p (point)) (n (buffer-length)))
    (while (and (< p n) (not (= (char-at p) 10))) (setq p (+ p 1)))
    p))

(defun fwd-word-pos ()
  "Just past the next word ahead of point (skip separators, span the word)."
  (let ((p (point)) (n (buffer-length)))
    (while (and (< p n) (not (word-char-p (char-at p)))) (setq p (+ p 1)))
    (while (and (< p n) (word-char-p (char-at p))) (setq p (+ p 1)))
    p))

(defun bwd-word-pos ()
  "Start of the word behind point (clamped to the editable region)."
  (let ((p (point)) (lo (edit-lo)))
    (while (and (> p lo) (not (word-char-p (char-at (- p 1))))) (setq p (- p 1)))
    (while (and (> p lo) (word-char-p (char-at (- p 1)))) (setq p (- p 1)))
    p))

(defun del-fwd (k)
  "Delete K characters AFTER point. delete-char only deletes BEFORE point, so
move right K and delete K -- those K become the chars that were ahead."
  (let ((avail (- (buffer-length) (point))))
    (let ((k2 (if (< k avail) k avail)))
      (if (> k2 0) (progn (goto-char (+ (point) k2)) (delete-char k2)) nil))))

(defun kill-line ()
  "C-k: kill to end of line; if already at the line end, eat the newline."
  (if (read-only-p) (echo "buffer is read-only")
    (let ((e (eol-pos)))
      (if (> e (point)) (del-fwd (- e (point)))
        (if (< (point) (buffer-length)) (del-fwd 1) nil)))))

(defun kill-word ()       ; M-d
  (if (read-only-p) (echo "buffer is read-only")
    (let ((e (fwd-word-pos))) (if (> e (point)) (del-fwd (- e (point))) nil))))

;; ---- lisp-interaction: evaluate the form before point (C-j) ----
(defun sexp-ws-p (c) (or (= c 32) (= c 10) (= c 9)))   ; space/newline/tab

(defun last-sexp-start (p)
  "Scan left from P over one balanced form; return its start position."
  (let ((q (- p 1)) (lo (edit-lo)))
    (while (and (> q lo) (sexp-ws-p (char-at q))) (setq q (- q 1)))  ; trailing ws
    (if (and (>= q 0) (= (char-at q) 41))            ; ')' -> match parens back
        (let ((depth 0) (done nil))
          (while (not done)
            (let ((c (char-at q)))
              ;; ')' deepens; the '(' that brings depth back to 0 is the matching opener
              (cond ((= c 41) (setq depth (+ depth 1)))
                    ((= c 40) (setq depth (- depth 1))))
              (cond ((and (= c 40) (= depth 0)) (setq done t))
                    ((<= q lo) (setq done t))
                    (t (setq q (- q 1))))))
          q)
      (progn                                          ; an atom: back over it
        (while (and (> q lo)
                    (not (sexp-ws-p (char-at (- q 1))))
                    (not (= (char-at (- q 1)) 40)))
          (setq q (- q 1)))
        q))))

(defun eval-last-sexp ()
  "C-j in lisp-interaction-mode: eval the form before point; insert its value."
  (let ((start (last-sexp-start (point))) (end (point)))
    (let ((src (buffer-substring start end)))
      (goto-char end)
      (insert "\n")
      (let ((val (eval (read-string src))))
        (flush-output)
        (insert (prin1-to-string val))
        (insert "\n")))))
