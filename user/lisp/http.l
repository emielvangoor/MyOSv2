;;; http.l -- a tiny HTTP/1.1 client that streams Server-Sent Events over the
;;; raw socket primitives (Phase 32.1). No chunked decoding: the assistant
;;; endpoint / proxy / mock present an unchunked text/event-stream and close
;;; with Connection: close, so we read until EOF.
;;;
;;; CRLF is built from (string-from-char 13) because the reader has no \r.

(setq http-cr (string-from-char 13))
(setq http-crlf (string-concat http-cr "\n"))

(defun http--trim-leading-space (s)
  "Drop one leading space (the SSE 'data: x' convention)."
  (if (and (> (string-length s) 0) (= (string-ref s 0) 32))
      (substring s 1 (string-length s)) s))

(defun http--strip-cr (s)
  "Drop a single trailing CR from a line."
  (if (and (> (string-length s) 0)
           (= (string-ref s (- (string-length s) 1)) 13))
      (substring s 0 (- (string-length s) 1)) s))

(defun http--dispatch (ev data on-event)
  (if (> (string-length data) 0) (on-event ev data) nil))

(defun http--is-chunked (hdr) (if (string-search "chunked" hdr) t nil))

(defun http--hex (s)               ; leading hex digits -> integer (ignores ;ext)
  (let ((n (string-length s)) (i 0) (val 0) (go t))
    (while (and go (< i n))
      (let ((c (string-ref s i)))
        (cond ((and (>= c 48) (<= c 57)) (setq val (+ (* val 16) (- c 48))))
              ((and (>= c 97) (<= c 102)) (setq val (+ (* val 16) (- c 87))))
              ((and (>= c 65) (<= c 70)) (setq val (+ (* val 16) (- c 55))))
              (t (setq go nil))))
      (if go (setq i (+ i 1)) nil))
    val))

;; Drain as much of RAW as possible into decoded body bytes. STATE: -1 need a
;; size line, -2 need the CRLF after a chunk's data, -3 done, >=1 bytes left in
;; the current chunk. Returns (decoded new-raw new-state).
(defun http--dechunk (raw state)
  (let ((out "") (go t))
    (while go
      (cond
        ((= state -1)
         (let ((nl (string-search "\n" raw)))
           (if (not nl) (setq go nil)
             (let ((sz (http--hex (http--strip-cr (substring raw 0 nl)))))
               (setq raw (substring raw (+ nl 1) (string-length raw)))
               (if (= sz 0) (progn (setq state -3) (setq go nil)) (setq state sz))))))
        ((= state -2)
         (if (< (string-length raw) 2) (setq go nil)
           (progn (setq raw (substring raw 2 (string-length raw))) (setq state -1))))
        ((> state 0)
         (if (= (string-length raw) 0) (setq go nil)
           (let ((take (if (< (string-length raw) state) (string-length raw) state)))
             (setq out (string-concat out (substring raw 0 take)))
             (setq raw (substring raw take (string-length raw)))
             (setq state (- state take))
             (if (= state 0) (setq state -2) nil))))
        (t (setq go nil))))
    (list out raw state)))

(defun http--read-stream (fd on-event)
  "Read FD to EOF, strip headers (decoding chunked transfer-encoding), then frame
SSE lines from the body and dispatch each frame."
  (let ((raw "") (hdr-done nil) (chunked nil) (cstate -1)
        (body "") (ev "message") (data "") (go t))
    (while go
      (let ((chunk (fd-read fd 4096)))
        (if (not chunk) (setq go nil)
          (progn
            (setq raw (string-concat raw chunk))
            (if (not hdr-done)
                (let ((b (string-search (string-concat http-crlf http-crlf) raw)))
                  (if b (progn
                          (setq chunked (http--is-chunked (substring raw 0 b)))
                          (setq raw (substring raw (+ b 4) (string-length raw)))
                          (setq hdr-done t))
                    nil))
              nil)
            (if hdr-done
                (progn
                  (if chunked
                      (let ((r (http--dechunk raw cstate)))
                        (setq body (string-concat body (nth 0 r)))
                        (setq raw (nth 1 r)) (setq cstate (nth 2 r)))
                    (progn (setq body (string-concat body raw)) (setq raw "")))
                  (let ((nl (string-search "\n" body)))
                    (while nl
                      (let ((line (http--strip-cr (substring body 0 nl))))
                        (setq body (substring body (+ nl 1) (string-length body)))
                        (cond
                          ((= (string-length line) 0)
                           (http--dispatch ev data on-event)
                           (setq ev "message") (setq data ""))
                          ((and (>= (string-length line) 6)
                                (equal (substring line 0 6) "event:"))
                           (setq ev (http--trim-leading-space
                                      (substring line 6 (string-length line)))))
                          ((and (>= (string-length line) 5)
                                (equal (substring line 0 5) "data:"))
                           (setq data (http--trim-leading-space
                                        (substring line 5 (string-length line)))))
                          (t nil)))
                      (setq nl (string-search "\n" body)))))
              nil)))))
    (http--dispatch ev data on-event)))   ; flush a final frame with no blank line

(defun http-post-sse (host port path header-lines body-parts on-event)
  "POST to HOST:PORT PATH; HEADER-LINES are \"Key: value\" strings, BODY-PARTS a
list of strings (never one big string -> dodges the 2048 cap). Parse the
text/event-stream reply, calling (ON-EVENT event-type data-json) per frame.
Returns t on a clean finish, nil if the socket could not be opened."
  (let ((fd (socket 'stream)))
    (if (< fd 0) nil
      (if (< (connect fd host port) 0) (progn (close fd) nil)
        (progn
          (fd-write fd (string-concat "POST " path " HTTP/1.1" http-crlf))
          (foreach (lambda (h) (fd-write fd (string-concat h http-crlf))) header-lines)
          (fd-write fd http-crlf)
          (foreach (lambda (p) (fd-write fd p)) body-parts)
          (http--read-stream fd on-event)
          (close fd)
          t)))))
