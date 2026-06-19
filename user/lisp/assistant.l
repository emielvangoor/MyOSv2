;;; assistant.l -- the native agent (Phase 32). Talks to OpenRouter (the OpenAI
;;; chat-completions wire format, in assistant-openrouter.l) over http.l, runs the
;;; agentic tool loop, and streams the reply into the *emiel* buffer.
;;; (Top-level globals use setq -- this Lisp has no defvar.)

(setq *assistant-name* "emiel")              ; overridable persona name
(setq *assistant-endpoint-host* "10.0.2.2")  ; TCP target (the host TLS proxy)
(setq *assistant-host* "openrouter.ai")      ; HTTP Host: header (the real service)
(setq *assistant-endpoint-port* 8787)
(setq *assistant-model* "anthropic/claude-haiku-4.5")  ; an OpenRouter model slug

(defun assistant--first-line (s)
  (let ((nl (string-search "\n" s)))
    (if nl (substring s 0 nl) s)))

(defun assistant--api-key ()
  "Read the API key from /lib/claude/api-key (one line), or \"\" if absent."
  (let ((fd (open "/lib/claude/api-key")))
    (if (< fd 0) ""
      (let ((k (fd-read fd 256)))
        (close fd)
        (if k (assistant--first-line k) "")))))

;;; ---- agentic-loop helpers (shared with the provider module) ----------------
;;; The transcript is built as a STRING of complete message turns; it can exceed
;;; 2048, so it is sent as a list split into <=1500-byte parts by ac--split.

(defun ac--split (s)
  "Split a possibly-large string into <=1500-byte parts for fd-write."
  (let ((n (string-length s)) (parts nil) (i 0))
    (while (< i n)
      (let ((end (if (< (+ i 1500) n) (+ i 1500) n)))
        (setq parts (cons (substring s i end) parts)) (setq i end)))
    (reverse parts)))

(defun ac--blen (parts)
  (let ((blen 0)) (foreach (lambda (p) (setq blen (+ blen (string-length p)))) parts) blen))

(defun assistant-converse (prompt on-piece)
  "Agentic loop: send PROMPT, run any tool the model asks for, loop tool_result
back until it answers in prose. Returns the final assistant text."
  (let ((messages
          (string-concat "{\"role\":\"user\",\"content\":\""
                         (json-escape prompt) "\"}"))
        (final "") (go t))
    (while go
      (let ((r (or--round messages on-piece)))
        (let ((text (nth 0 r)) (tid (nth 1 r)) (tname (nth 2 r)) (tin (nth 3 r)))
          (if (not tid)
              (progn (setq final text) (setq go nil))     ; no tool -> done
            (let ((result (assistant-tool-run tname (json-parse tin))))
              (setq messages
                (or--append-tool messages tid tname tin result)))))))
    final))

;;; ---- M-x emiel: an interactive conversation buffer -------------------------
;;; assistant-mode: RET sends the typed prompt to the agent; Shift-RET (cooked to
;;; meta-RET by cook_event) inserts a literal newline. The user's pending input
;;; runs from the buffer-local 'assistant-input-start to the buffer end.

(setq *assistant-buffer* nil)

(defun assistant--buffer-name () (string-concat "*" *assistant-name* "*"))

(defun assistant--buffer ()
  "The (memoised) assistant conversation buffer."
  (if *assistant-buffer* *assistant-buffer*
    (progn (setq *assistant-buffer* (make-buffer (assistant--buffer-name)))
           *assistant-buffer*)))

(defun assistant--prompt ()
  "Write a fresh input prompt at the end and record where the user's input begins."
  (let ((b (current-buffer)))
    (goto-char (buffer-length))
    (insert "> ")
    (buffer-local-set b 'assistant-input-start (buffer-length))
    (redisplay)))

(defun assistant--blank (s)
  "t if S is empty or only whitespace."
  (let ((n (string-length s)) (i 0) (blank t))
    (while (< i n)
      (let ((c (string-ref s i)))
        (if (or (= c 32) (= c 10) (= c 13) (= c 9)) nil (setq blank nil)))
      (setq i (+ i 1)))
    blank))

(defun assistant-self-insert (ch) (insert (string-from-char ch)))

(defun assistant-send ()
  "RET in assistant-mode: send the typed prompt and stream the reply, then lay a
fresh prompt. A blank input just redraws the prompt."
  (let ((b (current-buffer)))
    (let ((start (buffer-local-get b 'assistant-input-start 0)))
      (let ((prompt (buffer-substring start (buffer-length))))
        (if (assistant--blank prompt) (assistant--prompt)
          (progn
            (goto-char (buffer-length))
            (insert (string-concat "\n" *assistant-name* ": "))
            (redisplay)
            (assistant-converse prompt
              (lambda (piece)
                (set-buffer b) (goto-char (buffer-length)) (insert piece) (redisplay)))
            (set-buffer b) (goto-char (buffer-length)) (insert "\n\n")
            (assistant-show-pending)
            (assistant--prompt)))))))

(defun assistant-return () (assistant-send))

(register-mode 'assistant-mode
  (list 'fundamental-mode nil 'assistant-self-insert 'assistant-return nil
        "Assistant mode: RET sends the prompt; Shift-RET inserts a newline."
        "Assistant"))

(defun emiel ()
  "M-x emiel: open an interactive conversation with the assistant. Type a prompt
and press RET to send; Shift-RET inserts a newline."
  (interactive)
  (let ((b (assistant--buffer)))
    (set-buffer b)
    (set-major-mode b 'assistant-mode)
    (assistant--prompt)))

;;; ---- gate UI (Phase 32.2): review/accept persistent changes, undo ----------

(defun assistant-show-pending ()
  "Insert any queued persistent forms into *emiel* for review."
  (let ((b (assistant--buffer)))
    (set-buffer b) (goto-char (buffer-length))
    (if *assistant-pending*
        (foreach (lambda (p)
                   (insert (string-concat "\n[pending] " (car p)
                            "\n  M-x emiel-accept  /  M-x emiel-undo\n")))
                 *assistant-pending*)
      nil)
    (redisplay)))

(defun emiel-accept ()
  "M-x emiel-accept: apply the oldest queued persistent change."
  (interactive)
  (let ((b (assistant--buffer)))
    (set-buffer b) (goto-char (buffer-length))
    (insert (string-concat "\n" (assistant-accept) "\n")) (redisplay)))

(defun emiel-undo ()
  "M-x emiel-undo: roll back the most recent applied redefinition."
  (interactive)
  (let ((b (assistant--buffer)))
    (set-buffer b) (goto-char (buffer-length))
    (insert (string-concat "\n" (assistant-undo) "\n")) (redisplay)))
