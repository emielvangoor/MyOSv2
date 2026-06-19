;;; assistant.l -- the native agent (Phase 32). Talks to OpenRouter (the OpenAI
;;; chat-completions wire format, in assistant-openrouter.l) over http.l, runs the
;;; agentic tool loop, and streams the reply into the *emiel* buffer.
;;; (Top-level globals use setq -- this Lisp has no defvar.)

(setq *assistant-name* "emiel")              ; overridable persona name
(setq *assistant-endpoint-host* "10.0.2.2")  ; TCP target (the host TLS proxy)
(setq *assistant-host* "openrouter.ai")      ; HTTP Host: header (the real service)
(setq *assistant-endpoint-port* 8787)
(setq *assistant-model* "anthropic/claude-3.5-sonnet")  ; an OpenRouter model slug

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

;;; ---- M1 UI shim: M-x emiel -> stream a reply into the *emiel* buffer -------
;;; Free-text prompt entry (a minibuffer "ask" key) and the dynamic rename via
;;; set-assistant-name both land in milestone 4; M1 ships a proof-of-life greet.

(setq *assistant-buffer* nil)

(defun assistant--buffer-name () (string-concat "*" *assistant-name* "*"))

(defun assistant--buffer ()
  "The (memoised) assistant conversation buffer."
  (if *assistant-buffer* *assistant-buffer*
    (progn (setq *assistant-buffer* (make-buffer (assistant--buffer-name)))
           *assistant-buffer*)))

(defun assistant-ask (prompt)
  "Open the assistant buffer and stream a reply to PROMPT into it."
  (let ((b (assistant--buffer)))
    (set-buffer b) (goto-char (buffer-length))
    (insert (string-concat "\n> " prompt "\n"))
    (insert (string-concat *assistant-name* ": "))
    (redisplay)
    (assistant-converse prompt              ; the agentic loop -- tools + gate
      (lambda (piece)
        (set-buffer b) (goto-char (buffer-length)) (insert piece) (redisplay)))
    (set-buffer b) (goto-char (buffer-length)) (insert "\n") (redisplay)
    (assistant-show-pending)))

(defun emiel ()
  "M-x emiel: greet the assistant; its reply streams into the *emiel* buffer.
(Free-text prompt entry arrives in milestone 4.)"
  (interactive)
  (assistant-ask "Introduce yourself in one short sentence."))

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
