;;; json.l -- minimal JSON for the assistant (Phase 32.1).
;;; Milestone 1 needs only: escape a string for a request body, and pull one
;;; string field out of a small SSE data frame. A full recursive json-parse
;;; arrives in milestone 2 when tool_use inputs need structured decoding.
;;;
;;; NOTE the dialect: the reader has no \r escape and string-concat caps at
;;; 2048 bytes -- keep escaped strings short (M1 prompts are) and build CRLF
;;; from (string-from-char 13).

(defun json-escape (s)
  "S with the characters JSON requires escaped (quote, backslash, control)."
  (let ((n (string-length s)) (out "") (i 0))
    (while (< i n)
      (let ((c (string-ref s i)))
        (setq out
          (string-concat out
            (cond ((= c 34) "\\\"")          ; "
                  ((= c 92) "\\\\")          ; backslash
                  ((= c 10) "\\n")           ; LF
                  ((= c 13) "\\r")           ; CR (literal backslash-r in output)
                  ((= c 9)  "\\t")           ; TAB
                  (t (string-from-char c))))))
      (setq i (+ i 1)))
    out))

(defun json-find-key (s key)
  "Index of the first content char of the string value for \"KEY\":\" in S,
or nil if absent."
  (let ((pat (string-concat "\"" key "\":\"")))
    (let ((at (string-search pat s)))
      (if at (+ at (string-length pat)) nil))))

(defun json-read-string-at (s i)
  "Decode a JSON string body starting at index I (just past the opening quote)
up to the next unescaped quote. (\\uXXXX is not decoded in M1.)"
  (let ((n (string-length s)) (out "") (done nil))
    (while (and (not done) (< i n))
      (let ((c (string-ref s i)))
        (cond
          ((= c 34) (setq done t))                     ; closing quote
          ((= c 92)                                    ; escape: take next char
           (setq i (+ i 1))
           (let ((e (if (< i n) (string-ref s i) 0)))
             (setq out (string-concat out
               (cond ((= e 110) (string-from-char 10)) ; \n
                     ((= e 116) (string-from-char 9))  ; \t
                     ((= e 114) (string-from-char 13)) ; \r
                     ((= e 34) "\"")
                     ((= e 92) "\\")
                     ((= e 47) "/")
                     (t (string-from-char e)))))))
          (t (setq out (string-concat out (string-from-char c))))))
      (setq i (+ i 1)))
    out))

(defun json-string-value (s key)
  "The decoded string value of \"KEY\" in JSON text S, or nil if absent."
  (let ((i (json-find-key s key)))
    (if i (json-read-string-at s i) nil)))
