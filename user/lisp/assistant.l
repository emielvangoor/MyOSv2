;;; assistant.l -- the native agent's request path (Phase 32.1, proof of life).
;;; Builds an Anthropic Messages request, drives http-post-sse, and assembles
;;; the streamed assistant text. The tool loop, the apply gate and persistence
;;; arrive in milestone 2.  (Top-level globals use setq -- this Lisp has no
;;; defvar.)

(setq *assistant-name* "emiel")              ; overridable persona name
(setq *assistant-provider* 'openrouter)      ; 'openrouter | 'anthropic
(setq *assistant-endpoint-host* "10.0.2.2")  ; TCP target (the host TLS proxy)
(setq *assistant-host* "openrouter.ai")      ; HTTP Host: header (the real service)
(setq *assistant-endpoint-port* 8787)
(setq *assistant-model* "anthropic/claude-3.5-sonnet")  ; an OpenRouter model slug
(setq *assistant-max-tokens* 1024)

;; Dispatch the per-round request + the tool-result turn by provider, so the
;; agentic loop below is provider-agnostic.
(defun assistant--round (messages on-piece)
  (if (eq *assistant-provider* 'openrouter)
      (or--round messages on-piece) (ac--round messages on-piece)))

(defun assistant--append-tool (messages tid tname tin result)
  (if (eq *assistant-provider* 'openrouter)
      (or--append-tool messages tid tname tin result)
      (string-concat messages          ; the Anthropic Messages shape (M2)
        ",{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\""
        tid "\",\"name\":\"" tname "\",\"input\":" tin "}]}"
        ",{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\""
        tid "\",\"content\":\"" (json-escape result) "\"}]}")))

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

(defun assistant--request-body (prompt)
  "A list of body parts (never one big string) for a single-user-turn request."
  (list
    (string-concat
      "{\"model\":\"" *assistant-model* "\","
      "\"max_tokens\":" *assistant-max-tokens* ","
      "\"stream\":true,"
      "\"messages\":[{\"role\":\"user\",\"content\":\"")
    (json-escape prompt)
    "\"}]}"))

(defun assistant-stream (prompt on-piece)
  "Send PROMPT as one user turn; stream the reply, calling (ON-PIECE text) for
each text_delta, and return the full assembled assistant text."
  (let ((body (assistant--request-body prompt)) (acc ""))
    (let ((blen 0))
      (foreach (lambda (p) (setq blen (+ blen (string-length p)))) body)
      (http-post-sse
        *assistant-endpoint-host* *assistant-endpoint-port* "/v1/messages"
        (list (string-concat "Host: " *assistant-endpoint-host*)
              "Content-Type: application/json"
              "anthropic-version: 2023-06-01"
              (string-concat "x-api-key: " (assistant--api-key))
              (string-concat "Content-Length: " blen)
              "Connection: close")
        body
        (lambda (ev data)
          (if (equal ev "content_block_delta")
              (let ((piece (json-string-value data "text")))
                (if piece (progn (setq acc (string-concat acc piece))
                                 (on-piece piece)) nil))
            nil))))
    acc))

;;; ---- the agentic loop (Phase 32.2) -----------------------------------------
;;; The transcript is built as a STRING of complete content blocks; it can exceed
;;; 2048, so it is sent as a list split into <2048 parts by ac--split.

(defun ac--split (s)
  "Split a possibly-large string into <=1500-byte parts for fd-write."
  (let ((n (string-length s)) (parts nil) (i 0))
    (while (< i n)
      (let ((end (if (< (+ i 1500) n) (+ i 1500) n)))
        (setq parts (cons (substring s i end) parts)) (setq i end)))
    (reverse parts)))

(defun ac--request-parts (messages-json)
  "Body parts: model/tools/stream wrapper around the messages-json string."
  (append
    (list (string-concat
            "{\"model\":\"" *assistant-model* "\",\"max_tokens\":"
            *assistant-max-tokens* ",\"stream\":true,\"tools\":"
            (assistant-tools-json) ",\"messages\":["))
    (ac--split messages-json)
    (list "]}")))

(defun ac--blen (parts)
  (let ((blen 0)) (foreach (lambda (p) (setq blen (+ blen (string-length p)))) parts) blen))

(defun ac--headers (blen)
  (list (string-concat "Host: " *assistant-endpoint-host*)
        "Content-Type: application/json" "anthropic-version: 2023-06-01"
        (string-concat "x-api-key: " (assistant--api-key))
        (string-concat "Content-Length: " blen) "Connection: close"))

;; One round-trip -> (assistant-text tool-id tool-name tool-input-json); the tool
;; fields are nil when the model produced no tool_use.
(defun ac--round (messages-json on-piece)
  (let ((parts (ac--request-parts messages-json))
        (text "") (tid nil) (tname nil) (tin ""))
    (http-post-sse *assistant-endpoint-host* *assistant-endpoint-port* "/v1/messages"
      (ac--headers (ac--blen parts)) parts
      (lambda (ev data)
        (cond
          ((equal ev "content_block_start")
           (let ((cb (json-get (json-parse data) "content_block")))
             (if (and cb (equal (json-get cb "type") "tool_use"))
                 (progn (setq tid (json-get cb "id")) (setq tname (json-get cb "name")))
               nil)))
          ((equal ev "content_block_delta")
           (let ((d (json-get (json-parse data) "delta")))
             (cond
               ((equal (json-get d "type") "text_delta")
                (let ((p (json-get d "text")))
                  (if p (progn (setq text (string-concat text p)) (on-piece p)) nil)))
               ((equal (json-get d "type") "input_json_delta")
                (let ((p (json-get d "partial_json")))
                  (if p (setq tin (string-concat tin p)) nil)))
               (t nil))))
          (t nil))))
    (list text tid tname tin)))

(defun assistant-converse (prompt on-piece)
  "Agentic loop: send PROMPT, run any tool the model asks for, loop tool_result
back until it answers in prose. Returns the final assistant text."
  (let ((messages
          (string-concat "{\"role\":\"user\",\"content\":\""
                         (json-escape prompt) "\"}"))
        (final "") (go t))
    (while go
      (let ((r (assistant--round messages on-piece)))
        (let ((text (nth 0 r)) (tid (nth 1 r)) (tname (nth 2 r)) (tin (nth 3 r)))
          (if (not tid)
              (progn (setq final text) (setq go nil))     ; no tool -> done
            (let ((result (assistant-tool-run tname (json-parse tin))))
              (setq messages
                (assistant--append-tool messages tid tname tin result)))))))
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
