;;; assistant.l -- the native agent's request path (Phase 32.1, proof of life).
;;; Builds an Anthropic Messages request, drives http-post-sse, and assembles
;;; the streamed assistant text. The tool loop, the apply gate and persistence
;;; arrive in milestone 2.  (Top-level globals use setq -- this Lisp has no
;;; defvar.)

(setq *assistant-name* "emiel")              ; overridable persona name
(setq *assistant-endpoint-host* "10.0.2.2")  ; proxy by default (M1)
(setq *assistant-endpoint-port* 8787)
(setq *assistant-model* "claude-sonnet-4-6")
(setq *assistant-max-tokens* 1024)

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
