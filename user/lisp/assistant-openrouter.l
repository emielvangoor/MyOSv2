;;; assistant-openrouter.l -- the OpenRouter provider (OpenAI chat-completions
;;; wire format) for Phase 32.3. Reuses the M2 tools, gate and agentic loop;
;;; only the request shape, the SSE delta parsing and the auth header differ.
;;; Endpoint: POST /api/v1/chat/completions, Authorization: Bearer <key>.

(defun or-tools-json ()
  "The tool set as OpenAI 'function' tools."
  (string-concat
    "[{\"type\":\"function\",\"function\":{\"name\":\"eval_lisp\",\"description\":\"Evaluate a Lisp form in the live OS image to build features. Ephemeral forms run immediately; persistent ones (redefining an existing function, writing files) are queued for the user's approval.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"}},\"required\":[\"code\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"introspect_image\",\"description\":\"List interned symbols whose name contains a filter, to see what already exists.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\"}}}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"function_source\",\"description\":\"Show the living source of a function by name.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a file from the OS filesystem.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Write a file (queued for approval).\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"run_bash\",\"description\":\"Run a shell command via busybox and return stdout.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}}}]"))

(defun or--headers (blen)
  (list (string-concat "Host: " *assistant-host*)
        "Content-Type: application/json"
        (string-concat "Authorization: Bearer " (assistant--api-key))
        (string-concat "Content-Length: " blen) "Connection: close"))

(defun or--request-parts (messages-json)
  (append
    (list (string-concat
            "{\"model\":\"" *assistant-model* "\",\"stream\":true,\"tools\":"
            (or-tools-json) ",\"messages\":["))
    (ac--split messages-json)
    (list "]}")))

;; One OpenRouter round -> (text tool-id tool-name tool-args-json).
(defun or--round (messages-json on-piece)
  (let ((parts (or--request-parts messages-json))
        (text "") (tid nil) (tname nil) (tin ""))
    (http-post-sse *assistant-endpoint-host* *assistant-endpoint-port*
      "/api/v1/chat/completions" (or--headers (ac--blen parts)) parts
      (lambda (ev data)
        (if (equal data "[DONE]") nil
          (let ((choice (car (json-get (json-parse data) "choices"))))
            (if choice
                (let ((delta (json-get choice "delta")))
                  (let ((c (json-get delta "content")))
                    (if (and c (> (string-length c) 0))
                        (progn (setq text (string-concat text c)) (on-piece c)) nil))
                  (let ((tcs (json-get delta "tool_calls")))
                    (if tcs
                        (let ((tc (car tcs)))
                          (let ((idv (json-get tc "id"))) (if idv (setq tid idv) nil))
                          (let ((fn (json-get tc "function")))
                            (let ((nm (json-get fn "name"))) (if nm (setq tname nm) nil))
                            (let ((ag (json-get fn "arguments")))
                              (if ag (setq tin (string-concat tin ag)) nil))))
                      nil)))
              nil)))))
    (list text tid tname tin)))

(defun or--append-tool (messages tid tname tin result)
  "Append the OpenAI assistant tool_call turn + the tool result turn."
  (string-concat messages
    ",{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"" tid
    "\",\"type\":\"function\",\"function\":{\"name\":\"" tname
    "\",\"arguments\":" (json-encode-string tin) "}}]}"
    ",{\"role\":\"tool\",\"tool_call_id\":\"" tid
    "\",\"content\":\"" (json-escape result) "\"}"))
