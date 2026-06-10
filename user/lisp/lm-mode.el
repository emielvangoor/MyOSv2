;;; lm-mode.el --- eval into the MyOSv2 Lisp machine over TCP  -*- lexical-binding: t; -*-

;; The host half of Phase 24.1b: a thin comint client for the guest's network
;; REPL, so forms travel editor -> TCP -> QEMU hostfwd -> guest /bin/lisp and
;; the result comes back into an Emacs buffer. You are editing a LIVE image:
;; a defun sent today is still defined tomorrow (until the guest reboots).
;;
;; Setup:
;;   1. In the repo:        make run
;;   2. At the guest shell: lisp -serve
;;      (the Makefile forwards host port 7777 into the guest; 7777 and not the
;;       classic 7000 because macOS's AirPlay Receiver squats on 7000)
;;   3. In Emacs:           M-x lm-connect
;;
;; Doom Emacs users -- in config.el:
;;   (load! "~/Code/Sides/os/user/lisp/lm-mode.el")
;;   (add-hook 'lisp-mode-hook #'lm-minor-mode)
;;
;; With `lm-minor-mode' enabled in any Lisp buffer:
;;   C-c C-e   send the sexp before point to the machine
;;   C-c C-r   send the region to the machine
;;
;; Everything echoes into the *myos-lisp* comint buffer, which is itself a
;; fully interactive REPL (type at the `lisp> ' prompt).

;;; Code:

(require 'comint)

(defgroup lm nil
  "Client for the MyOSv2 Lisp machine's network REPL."
  :group 'comm)

(defcustom lm-host "localhost"
  "Host where the QEMU forward for the Lisp machine lives."
  :type 'string :group 'lm)

(defcustom lm-port 7777
  "TCP port of the Lisp machine REPL (must match `lisp -serve [port]')."
  :type 'integer :group 'lm)

(defun lm-connect ()
  "Connect to the MyOSv2 Lisp machine and pop to its REPL buffer.
comint handles the rest: a network process looks like any other
inferior-lisp, with history, prompt navigation and so on."
  (interactive)
  (let ((buf (make-comint "myos-lisp" (cons lm-host lm-port))))
    (pop-to-buffer buf)))

(defun lm--process ()
  "The live connection, or a helpful error."
  (or (get-buffer-process "*myos-lisp*")
      (user-error "Not connected -- M-x lm-connect first (and `lisp -serve' in the guest)")))

(defun lm--send (string)
  "Ship STRING (one or more forms) to the machine, newline-terminated.
Sent through comint so the form AND its result both appear in the
REPL buffer -- you can watch your editor talking to the OS."
  (with-current-buffer "*myos-lisp*"
    (comint-send-string (lm--process)
                        (concat (string-trim-right string) "\n"))))

(defun lm-eval-last-sexp ()
  "Send the sexp before point to the Lisp machine."
  (interactive)
  (lm--send (buffer-substring-no-properties
             (save-excursion (backward-sexp) (point))
             (point))))

(defun lm-eval-region (beg end)
  "Send the region to the Lisp machine."
  (interactive "r")
  (lm--send (buffer-substring-no-properties beg end)))

(defvar lm-minor-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map (kbd "C-c C-e") #'lm-eval-last-sexp)
    (define-key map (kbd "C-c C-r") #'lm-eval-region)
    map)
  "Keymap for `lm-minor-mode'.")

;;;###autoload
(define-minor-mode lm-minor-mode
  "Eval Lisp buffers into a running MyOSv2 guest."
  :lighter " LM"
  :keymap lm-minor-mode-map)

(provide 'lm-mode)
;;; lm-mode.el ends here
