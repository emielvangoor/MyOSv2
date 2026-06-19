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

(defun http--read-stream (fd on-event)
  "Read FD to EOF, skip the response headers, frame SSE lines, dispatch frames."
  (let ((frag "") (in-body nil) (ev "message") (data "") (go t))
    (while go
      (let ((chunk (fd-read fd 4096)))
        (if (not chunk) (setq go nil)
          (progn
            (setq frag (string-concat frag chunk))
            (if (not in-body)
                (let ((b (string-search (string-concat http-crlf http-crlf) frag)))
                  (if b (progn
                          (setq frag (substring frag (+ b 4) (string-length frag)))
                          (setq in-body t))
                    nil))
              nil)
            (if in-body
                (let ((nl (string-search "\n" frag)))
                  (while nl
                    (let ((line (http--strip-cr (substring frag 0 nl))))
                      (setq frag (substring frag (+ nl 1) (string-length frag)))
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
                    (setq nl (string-search "\n" frag))))
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
