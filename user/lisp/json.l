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
