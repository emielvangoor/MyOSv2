;;; system.l -- the shell, in Lisp (Phase 24.3).
;;;
;;; Where bootstrap.l builds the LANGUAGE library out of the C primitives,
;;; this file builds the OPERATING-SYSTEM library out of the syscall
;;; primitives (user/lm_sys.c). It is the Eshell idea: the shell is not a
;;; separate program with its own little language -- it IS the Lisp image,
;;; and "commands" are just functions. External ELF programs and in-image
;;; functions compose with the same syntax, including through pipelines.
;;;
;;; Loaded by /bin/lisp right after bootstrap.l (the initrd puts it at
;;; /lib/system.l).

;; ---- running external programs --------------------------------------

(defun bin-path (name)
  "Turn a command name into its /bin path: \"wc\" -> \"/bin/wc\"."
  (string-concat "/bin/" name))

(defun run-prog (path argv)
  "fork, exec PATH with ARGV in the child, wait; evaluate to the exit status.
The three syscalls of process creation, in one function. If exec fails the
child must NOT fall back into being a second Lisp REPL fighting over the
terminal -- it exits 127, the shell convention for command-not-found."
  (if (= (fork) 0)
      (progn (exec path argv) (exit 127))
    (cdr (wait))))

(defun run cmdargs
  "(run \"hello\" \"arg\"...) -> run /bin/hello with arguments, wait.
A rest-parameter function: the whole call is the argv, Unix-style, with
argv[0] = the command name."
  (run-prog (bin-path (car cmdargs)) cmdargs))

;; ---- pipelines -------------------------------------------------------

(defun pipe-stages (left right)
  "Run thunks LEFT and RIGHT as two forked children joined by a pipe:
LEFT's fd 1 is the pipe's write end, RIGHT's fd 0 is its read end.
Evaluates to RIGHT's exit status (like $? after `a | b` in sh).

Each child runs its whole stage -- a (run ...) inside the thunk execs the
program as a grandchild whose stdio the dup2'd fds already shape -- and an
in-image stage like (princ \"x\") works because after (dup2 w 1) anything
written to fd 1 lands in the pipe. The parent closes BOTH pipe ends before
waiting, or RIGHT would never see EOF (the parent's open write end would
keep the pipe 'alive')."
  (let ((p (pipe)))
    (if (= (fork) 0)
        (progn (dup2 (cdr p) 1)            ; left child: stdout -> pipe
               (close (car p)) (close (cdr p))
               (let ((v (funcall left)))
                 (exit (if (numberp v) v 0))))
      nil)
    (let ((rpid (fork)))
      (if (= rpid 0)
          (progn (dup2 (car p) 0)          ; right child: stdin <- pipe
                 (close (car p)) (close (cdr p))
                 (let ((v (funcall right)))
                   (exit (if (numberp v) v 0))))
        (progn
          (close (car p))
          (close (cdr p))
          ;; Two children, two waits; they finish in either order, so keep
          ;; the status that belongs to the right-hand stage.
          (let ((r1 (wait)) (r2 (wait)))
            (if (= (car r1) rpid) (cdr r1) (cdr r2))))))))

(defmacro | (a b)
  "(| left right) -- the pipeline, in its traditional spelling.
A macro so the stages are NOT evaluated here: each is wrapped in a lambda
and only runs inside its own forked child."
  (list 'pipe-stages (list 'lambda nil a) (list 'lambda nil b)))

;; ---- coreutils, in Lisp ----------------------------------------------

(defun ls args
  "(ls) or (ls \"/bin\") -> the directory's entries, as a list of strings."
  (readdir (if args (car args) "/")))

(defun cat (path)
  "Print the contents of PATH. Evaluates to t, or nil if PATH won't open."
  (let ((fd (open path)))
    (if (< fd 0)
        nil
      (let ((chunk (fd-read fd 256)))
        (while chunk
          (princ chunk)
          (setq chunk (fd-read fd 256)))
        (close fd)
        t))))

;; ---- a REPL written in the language it serves -------------------------

(defun repl ()
  "read -> eval -> print, forever. The whole REPL, in one Lisp function,
made possible by the eval primitive. A typo unwinds to the C-level
recovery point, which also ends this loop -- fine for a demo repl; the
resident REPL in user/lm.c is the one with error recovery."
  (while t
    (princ "lisp* ")
    (print (eval (read)))))

(princ "system.l loaded.")
(terpri)
