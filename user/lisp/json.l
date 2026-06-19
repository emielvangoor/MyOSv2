;;; json.l -- minimal JSON for the assistant (Phase 32.1).
;;; Milestone 1 needs only: escape a string for a request body, and pull one
;;; string field out of a small SSE data frame. A full recursive json-parse
;;; arrives in milestone 2 when tool_use inputs need structured decoding.
;;;
;;; NOTE the dialect: the reader has no \r escape and string-concat caps at
;;; 2048 bytes -- keep escaped strings short (M1 prompts are) and build CRLF
;;; from (string-from-char 13).

(defun json--hexdigit (n) (if (< n 10) (+ 48 n) (+ 87 n)))   ; 0-9 -> '0'..; 10+ -> 'a'..

(defun json-escape (s)
  "S with the characters JSON requires escaped: quote, backslash, and EVERY
control char (< 0x20) -- the common ones as \\n/\\r/\\t, the rest as \\u00XX.
Tool output (e.g. ANSI colour from busybox ls) carries raw ESC bytes; leaving
them unescaped makes the request body invalid JSON."
  (let ((n (string-length s)) (out "") (i 0))
    (while (< i n)
      (let ((c (string-ref s i)))
        (setq out
          (string-concat out
            (cond ((= c 34) "\\\"")          ; "
                  ((= c 92) "\\\\")          ; backslash
                  ((= c 10) "\\n")           ; LF
                  ((= c 13) "\\r")           ; CR
                  ((= c 9)  "\\t")           ; TAB
                  ((< c 32)                  ; any other control char -> \u00XX
                   (string-concat "\\u00"
                     (string-from-char (json--hexdigit (/ c 16)))
                     (string-from-char (json--hexdigit (% c 16)))))
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

;;; ---- recursive descent parser (M2): JSON text -> Lisp ----------------------
;;; object -> alist of (key-string . value); array -> list; string -> string;
;;; number -> fixnum (fraction/exponent skipped); true -> t; false/null -> nil.
;;; Each jp-* helper returns (value . next-index) so positions thread cleanly.

(defun jp-ws (s i)
  (let ((n (string-length s)) (go t))
    (while (and go (< i n))
      (let ((c (string-ref s i)))
        (if (or (= c 32) (= c 9) (= c 10) (= c 13)) (setq i (+ i 1)) (setq go nil))))
    i))

(defun jp-string (s i)              ; i AT the opening quote -> (string . next)
  (let ((n (string-length s)) (out "") (done nil))
    (setq i (+ i 1))
    (while (and (not done) (< i n))
      (let ((c (string-ref s i)))
        (cond
          ((= c 34) (setq done t) (setq i (+ i 1)))
          ((= c 92)
           (setq i (+ i 1))
           (let ((e (if (< i n) (string-ref s i) 0)))
             (setq out (string-concat out
               (cond ((= e 110) (string-from-char 10)) ((= e 116) (string-from-char 9))
                     ((= e 114) (string-from-char 13)) ((= e 34) "\"")
                     ((= e 92) "\\") ((= e 47) "/") (t (string-from-char e)))))
             (setq i (+ i 1))))
          (t (setq out (string-concat out (string-from-char c))) (setq i (+ i 1))))))
    (cons out i)))

(defun jp-number (s i)              ; integer part; skips any fraction/exponent
  (let ((n (string-length s)) (neg nil) (val 0))
    (if (and (< i n) (= (string-ref s i) 45)) (progn (setq neg t) (setq i (+ i 1))) nil)
    (while (and (< i n) (let ((c (string-ref s i))) (and (>= c 48) (<= c 57))))
      (setq val (+ (* val 10) (- (string-ref s i) 48))) (setq i (+ i 1)))
    (while (and (< i n) (let ((c (string-ref s i)))
                          (or (= c 46) (= c 101) (= c 69) (= c 43) (= c 45)
                              (and (>= c 48) (<= c 57)))))
      (setq i (+ i 1)))
    (cons (if neg (- 0 val) val) i)))

(defun jp-value (s i)
  (setq i (jp-ws s i))
  (let ((c (string-ref s i)))
    (cond ((= c 34) (jp-string s i))
          ((= c 123) (jp-object s i))            ; {
          ((= c 91) (jp-array s i))              ; [
          ((= c 116) (cons t (+ i 4)))           ; true
          ((= c 102) (cons nil (+ i 5)))         ; false
          ((= c 110) (cons nil (+ i 4)))         ; null
          (t (jp-number s i)))))

(defun jp-object (s i)              ; i AT '{' -> (alist . next)
  (let ((n (string-length s)) (out nil) (done nil))
    (setq i (jp-ws s (+ i 1)))
    (if (and (< i n) (= (string-ref s i) 125)) (progn (setq done t) (setq i (+ i 1))) nil)
    (while (not done)
      (let ((kp (jp-string s (jp-ws s i))))
        (let ((ci (jp-ws s (cdr kp))))
          (let ((vp (jp-value s (+ ci 1))))      ; skip the ':'
            (setq out (cons (cons (car kp) (car vp)) out))
            (let ((j (jp-ws s (cdr vp))))
              (let ((ch (string-ref s j)))
                (if (= ch 44) (setq i (+ j 1))    ; ,
                  (progn (setq done t) (setq i (+ j 1))))))))))   ; } (or malformed)
    (cons (reverse out) i)))

(defun jp-array (s i)               ; i AT '[' -> (list . next)
  (let ((n (string-length s)) (out nil) (done nil))
    (setq i (jp-ws s (+ i 1)))
    (if (and (< i n) (= (string-ref s i) 93)) (progn (setq done t) (setq i (+ i 1))) nil)
    (while (not done)
      (let ((vp (jp-value s i)))
        (setq out (cons (car vp) out))
        (let ((j (jp-ws s (cdr vp))))
          (let ((ch (string-ref s j)))
            (if (= ch 44) (setq i (+ j 1))
              (progn (setq done t) (setq i (+ j 1))))))))
    (cons (reverse out) i)))

(defun json-parse (s) "Parse one JSON value from string S." (car (jp-value s 0)))

(defun json-get (obj key)
  "Value for KEY in a json-parse object (alist), or nil."
  (if obj (if (equal (car (car obj)) key) (cdr (car obj)) (json-get (cdr obj) key)) nil))

(defun json-encode-string (s)
  "Wrap S as a JSON string literal (quotes + escapes) -- e.g. for OpenAI tool_call
arguments, which are themselves a JSON string."
  (string-concat "\"" (json-escape s) "\""))
