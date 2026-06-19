;;; assistant-tools.l -- what the agent can DO (Phase 32.2). Each tool takes an
;;; alist of decoded arguments and returns a string (the tool_result content).
;;; eval-lisp routes through the apply gate so persistent changes are deferred.
;;;
;;; NOTE: tool results are built with string-concat, which caps at 2048 bytes,
;;; so read_file / introspect return at most ~2KB (the agent should filter /
;;; read targeted ranges). Lifting this needs a dynamic string-concat -- a
;;; separate kernel change worth doing later.

(defun at--arg (args key) (json-get args key))   ; alist lookup, nil if absent

(defun at-eval-lisp (args)
  "Run a Lisp code string through the apply gate; return its result text."
  (let ((code (at--arg args "code")))
    (if code (assistant-apply code) "error: missing 'code'")))

(defun at-introspect (args)
  "List interned symbols whose name contains the filter (all if absent), one per
line, each tagged with its function-info kind."
  (let ((filt (at--arg args "filter")) (out "") (syms (all-symbols)))
    (foreach
      (lambda (sym)
        (let ((nm (symbol-name sym)))
          (if (or (not filt) (string-search filt nm))
              (let ((fi (function-info sym)))
                (setq out (string-concat out nm
                            (if fi (string-concat " : " (symbol-name (car fi))) "")
                            "\n")))
            nil)))
      syms)
    (if (= (string-length out) 0) "(no matching symbols)" out)))

(defun at-source (args)
  "The living source of a function (its function-info), printed."
  (let ((nm (at--arg args "name")))
    (if nm (prin1-to-string (function-info (read-string nm))) "error: missing 'name'")))

(defun at-read-file (args)
  (let ((path (at--arg args "path")))
    (if (not path) "error: missing 'path'"
      (let ((fd (open path)))
        (if (< fd 0) (string-concat "error: cannot open " path)
          (let ((acc "") (go t))
            (while go
              (let ((chunk (fd-read fd 1024)))
                (if chunk (setq acc (string-concat acc chunk)) (setq go nil))))
            (close fd) acc))))))

(defun at-write-file (args)
  "Write a file -- always persistent, so this is gated like eval of a write op."
  (let ((path (at--arg args "path")) (content (at--arg args "content")))
    (if (and path content)
        (assistant-apply
          (string-concat "(let ((fd (creat \"" path "\"))) (fd-write fd \""
                         (json-escape content) "\") (close fd))"))
      "error: missing 'path' or 'content'")))

(defun at-run-bash (args)
  "Run a command via busybox sh -c, returning its stdout (capped)."
  (let ((cmd (at--arg args "command")))
    (if (not cmd) "error: missing 'command'"
      (let ((p (pipe)))
        (let ((pid (fork)))
          (if (= pid 0)
              (progn (dup2 (cdr p) 1) (close (car p)) (close (cdr p))
                     (exec "/bin/busybox" (list "sh" "-c" cmd)) (exit 127))
            (progn
              (close (cdr p))
              (let ((acc "") (go t))
                (while go
                  (let ((chunk (fd-read (car p) 1024)))
                    (if chunk (setq acc (string-concat acc chunk)) (setq go nil))))
                (close (car p)) (wait) acc))))))))

(defun assistant-tool-run (name args)
  "Dispatch a tool call by NAME with decoded ARGS (an alist)."
  (cond ((equal name "eval_lisp") (at-eval-lisp args))
        ((equal name "introspect_image") (at-introspect args))
        ((equal name "function_source") (at-source args))
        ((equal name "read_file") (at-read-file args))
        ((equal name "write_file") (at-write-file args))
        ((equal name "run_bash") (at-run-bash args))
        (t (string-concat "error: unknown tool " name))))

;; The JSON schema array advertised to the API (string parts -> the request).
(defun assistant-tools-json ()
  (string-concat
    "[{\"name\":\"eval_lisp\",\"description\":\"Evaluate a Lisp form in the live OS image. Use this to define functions and build features. Ephemeral forms run immediately; persistent ones (redefining an existing function, writing files) are queued for the user's approval.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"}},\"required\":[\"code\"]}},"
    "{\"name\":\"introspect_image\",\"description\":\"List interned symbols whose name contains the filter, to see what already exists before writing code.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\"}}}},"
    "{\"name\":\"function_source\",\"description\":\"Show the living source of a function by name.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}},"
    "{\"name\":\"read_file\",\"description\":\"Read a file from the OS filesystem.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
    "{\"name\":\"write_file\",\"description\":\"Write a file (queued for approval).\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}},"
    "{\"name\":\"run_bash\",\"description\":\"Run a shell command via busybox and return stdout.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}]"))
