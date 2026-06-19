;;; assistant-apply.l -- the hybrid-by-scope gate (Phase 32.2). Generated forms
;;; are classified: EPHEMERAL (a fresh defun, a computation) auto-evals;
;;; PERSISTENT (redefining an existing function, or any file write / permanent
;;; command) is queued for the user's approval. Undo snapshots the old binding.

(setq *assistant-pending* nil)     ; list of (form-string . form) awaiting approval
(setq *assistant-undo* nil)        ; list of (symbol . function-info) for rollback

;; primitives whose presence in a form makes it persistent (touches disk/registry)
(setq *assistant-write-ops*
  (list "write-file" "creat" "save-buffer" "fd-write" "register-command"))

(defun aa--defun-target (form)
  "If FORM is (defun NAME ...), the NAME symbol; else nil."
  (if (and (consp form) (eq (car form) 'defun) (consp (cdr form)))
      (car (cdr form)) nil))

(defun aa--mentions-write-op (code)
  "t if CODE (the raw string) names a disk/registry primitive."
  (let ((ops *assistant-write-ops*) (hit nil))
    (while (and ops (not hit))
      (if (string-search (car ops) code) (setq hit t) nil)
      (setq ops (cdr ops)))
    hit))

(defun assistant-classify (code)
  "'persistent if CODE redefines an existing function or writes disk/registry;
'ephemeral otherwise (conservative: a defun of a new name, or a plain
computation, is ephemeral)."
  (let ((form (read-string code)))
    (let ((tgt (aa--defun-target form)))
      (cond
        ((aa--mentions-write-op code) 'persistent)
        ((and tgt (function-info tgt)) 'persistent)   ; redefining an existing fn
        (t 'ephemeral)))))

(defun assistant--snapshot (form)
  "Record the old function binding of a defun target so undo can restore it."
  (let ((tgt (aa--defun-target form)))
    (if (and tgt (function-info tgt))
        (setq *assistant-undo* (cons (cons tgt (function-info tgt)) *assistant-undo*))
      nil)))

(defun assistant-apply (code)
  "Classify CODE and either eval it now (ephemeral -> \"applied: <result>\") or
queue it for approval (persistent -> \"queued: needs approval\")."
  (if (eq (assistant-classify code) 'ephemeral)
      (string-concat "applied: " (prin1-to-string (eval (read-string code))))
    (progn
      (setq *assistant-pending*
            (cons (cons code (read-string code)) *assistant-pending*))
      "queued: needs approval")))

(defun assistant-accept ()
  "Apply the oldest pending persistent form (after the user okays it)."
  (if *assistant-pending*
      (let ((p (car (reverse *assistant-pending*))))
        (assistant--snapshot (cdr p))
        (eval (cdr p))
        (setq *assistant-pending* (reverse (cdr (reverse *assistant-pending*))))
        "accepted")
    "nothing pending"))

(defun assistant-undo ()
  "Restore the most recently redefined function to its previous binding.
function-info returns (lambda params body); rebuild the defun and re-eval."
  (if *assistant-undo*
      (let ((u (car *assistant-undo*)))
        (let ((fi (cdr u)))
          (eval (list 'defun (car u) (nth 1 fi) (nth 2 fi))))
        (setq *assistant-undo* (cdr *assistant-undo*))
        "undone")
    "nothing to undo"))

;;; ---- persistence: kept features land in /lib/claude and reload at boot ------

(defun aa--write (path content)
  (let ((fd (creat path)))
    (if (< fd 0) nil (progn (fd-write fd content) (close fd) t))))

(defun aa--slurp (path)
  "Whole file as a string, or \"\" if absent (capped at 4096 -- M2 manifests are small)."
  (let ((fd (open path)))
    (if (< fd 0) ""
      (let ((c (fd-read fd 4096))) (close fd) (if c c "")))))

(defun assistant-persist (name code)
  "Save CODE as /lib/claude/NAME.l and append a loader to the manifest."
  (mkdir "/lib/claude")                       ; harmless if it already exists
  (let ((path (string-concat "/lib/claude/" name ".l")))
    (aa--write path (string-concat code "\n"))
    (aa--write "/lib/claude/manifest.l"
      (string-concat (aa--slurp "/lib/claude/manifest.l")
                     "(load \"" path "\")\n"))
    "saved"))

(defun assistant-load-all ()
  "Load every persisted feature (the manifest). Safe to call at boot."
  (let ((fd (open "/lib/claude/manifest.l")))
    (if (< fd 0) nil (progn (close fd) (load "/lib/claude/manifest.l")))))
