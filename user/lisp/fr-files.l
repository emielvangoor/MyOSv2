;;; fr-files.l -- visiting files: open, edit, save (Phase 27.3).

;; ---- visiting files: open, edit, save (Phase 27.3) ----------------------

(defun find-file-here (path)
  "Open PATH in text-mode in the CURRENT window. A missing file starts empty.
The buffer remembers PATH (buffer-local file-path) so C-x C-s knows where to
write."
  (let ((b (make-buffer path)))
    (set-major-mode b 'text-mode)                     ; selects b, mode line (Text)
    (buffer-local-set b 'file-path path)
    (let ((fd (open path)))
      (if (>= fd 0)
          (progn
            (let ((chunk (fd-read fd 1024)))
              (while chunk (insert chunk) (setq chunk (fd-read fd 1024))))
            (close fd))
        nil))                                         ; nonexistent -> empty buffer
    (goto-char 0)
    (redisplay)
    b))

(defun cmd-find-file () (interactive) (mb-start 'find-file))         ; C-x C-f

(defun save-buffer ()
  "Write the current buffer back to the file it is visiting (C-x C-s)."
  (interactive)
  (let ((path (buffer-local-get (current-buffer) 'file-path nil)))
    (if path
        (let ((fd (creat path)))
          (if (>= fd 0)
              (progn (fd-write fd (buffer-substring 0 (buffer-length)))
                     (close fd)
                     (echo (string-concat "saved " path)))
            (echo "save failed")))
      (echo "no file for this buffer"))))
