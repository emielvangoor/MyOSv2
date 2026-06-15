;;; frame.l -- the graphical machine's event loop and REPL (Phase 25.4).
;;;
;;; Everything a key DOES is decided here, in Lisp -- C only reports what the
;;; key IS ((char 97), (ctrl 120), (mouse x y)). This is the Emacs deal: the
;;; redisplay engine and the input cooker are C machinery; the personality of
;;; the machine -- keymaps, the REPL, what mouse-1 means -- is live Lisp you
;;; can redefine from this very REPL (or over the network).
;;;
;;; Loaded by `lisp -frame` after bootstrap.l and system.l.

;; REPL state, kept PER BUFFER so each split window is an independent REPL.
;; "Being a REPL" is no longer a list-membership flag: a buffer is a REPL when
;; its major-mode is repl-mode (see modes.l). Where each REPL's prompt left off
;; is the buffer-local `repl-start` (read/written via cur-start/set-cur-start).
;; Input history is shared across REPLs (one command history), newest-first;
;; repl-hist-idx is the position being shown (-1 = the live line).
(setq repl-history nil)
(setq repl-hist-idx -1)

(defun cur-start ()
  "Where the SELECTED buffer's REPL input begins (0 if it has no prompt yet)."
  (buffer-local-get (current-buffer) 'repl-start 0))

(defun set-cur-start (pos)
  "Record the current buffer's input start."
  (buffer-local-set (current-buffer) 'repl-start pos))

(defun prompt ()
  "Show a fresh prompt and remember where input starts (in THIS buffer)."
  (insert "lisp> ")
  (set-cur-start (buffer-length)))

(defun repl-eval ()
  "Evaluate the text after the last prompt; print its value; re-prompt.
The whole REPL in one function: buffer-substring lifts the input out of
the buffer, read-string turns text into a form, eval runs it."
  (let ((src (buffer-substring (cur-start) (buffer-length))))
    ;; Remember non-empty input, and leave history-navigation mode: the next
    ;; Up starts again from the most recent entry.
    (if (> (string-length src) 0)
        (setq repl-history (cons src repl-history)) nil)
    (setq repl-hist-idx -1)
    ;; Point may be mid-input after cursor motion (C-a, C-b, ...); the result
    ;; and next prompt must go at the END, below the input, not wherever the
    ;; cursor happens to sit -- otherwise we splice them into the line.
    (goto-char (buffer-length))
    (insert "\n")
    (let ((val (eval (read-string src))))
      (flush-output)                ; princ'd text lands HERE, before the value
      (insert (prin1-to-string val))
      (insert "\n")
      (prompt))))

(defun repl-set-input (s)
  "Replace the current REPL input (everything after the prompt) with S."
  (let ((n (- (buffer-length) (cur-start))))
    (if (> n 0) (delete-char n) nil))     ; erase the old line back to the prompt
  (insert s))

(defun repl-recall ()
  "Show the history entry at repl-hist-idx (or an empty line at -1)."
  (repl-set-input (if (>= repl-hist-idx 0) (nth repl-hist-idx repl-history) "")))

(defun repl-history-prev ()
  "Up / C-p: step to an OLDER entry, if there is one."
  (if (< (+ repl-hist-idx 1) (length repl-history))
      (progn (setq repl-hist-idx (+ repl-hist-idx 1)) (repl-recall)) nil))

(defun repl-history-next ()
  "Down / C-n: step to a NEWER entry, or back to the empty line."
  (if (>= repl-hist-idx 0)
      (progn (setq repl-hist-idx (- repl-hist-idx 1)) (repl-recall)) nil))

;; C-x prefix state: nil normally, t after a C-x.
(setq pending-cx nil)

;; The alist of (buffer . file path) for buffers visiting a file. The buffer's
;; major-mode now decides what RET means -- "evaluate" in repl-mode, "insert a
;; newline" in fundamental/text-mode -- so dispatch no longer needs a flag here.
(setq file-of nil)

(defun new-repl ()
  "Turn the selected window's buffer into a FRESH REPL buffer in repl-mode."
  (let ((b (make-buffer "*repl*")))
    (set-major-mode b 'repl-mode)                         ; prompts via repl-setup
    b))

(defun repl-here ()
  "C-x r: turn the current window's buffer into a fresh REPL (clear + prompt)."
  (goto-char (buffer-length)) (delete-char (buffer-length))   ; clear the buffer
  (set-major-mode (current-buffer) 'repl-mode))

(defun split-repl-below ()
  "C-x 2: split, and give the new (lower) window its own fresh REPL."
  (split-below) (other-window) (new-repl) (redisplay))

(defun split-repl-right ()
  "C-x 3: split side by side, fresh REPL in the new (right) window."
  (split-right) (other-window) (new-repl) (redisplay))

;; ---- basic Emacs motion + editing commands -----------------------------
;; These act on the current buffer; in the REPL the lower bound is the prompt
;; (repl-start), so motion and deletion never reach into the prompt text.

(defun edit-lo ()
  "Leftmost editable position: the prompt in a REPL, else 0."
  (if (eq (buffer-mode (current-buffer)) 'repl-mode) (cur-start) 0))

(defun word-char-p (c)
  (or (and (>= c 48) (<= c 57))          ; 0-9
      (and (>= c 65) (<= c 90))          ; A-Z
      (and (>= c 97) (<= c 122))))       ; a-z

(defun bol-pos ()
  "Start of the current line, clamped to the editable region (C-a)."
  (let ((p (point)) (lo (edit-lo)))
    (while (and (> p lo) (not (= (char-at (- p 1)) 10))) (setq p (- p 1)))
    p))

(defun eol-pos ()
  "End of the current line: the next newline or end of buffer (C-e)."
  (let ((p (point)) (n (buffer-length)))
    (while (and (< p n) (not (= (char-at p) 10))) (setq p (+ p 1)))
    p))

(defun fwd-word-pos ()
  "Just past the next word ahead of point (skip separators, span the word)."
  (let ((p (point)) (n (buffer-length)))
    (while (and (< p n) (not (word-char-p (char-at p)))) (setq p (+ p 1)))
    (while (and (< p n) (word-char-p (char-at p))) (setq p (+ p 1)))
    p))

(defun bwd-word-pos ()
  "Start of the word behind point (clamped to the editable region)."
  (let ((p (point)) (lo (edit-lo)))
    (while (and (> p lo) (not (word-char-p (char-at (- p 1))))) (setq p (- p 1)))
    (while (and (> p lo) (word-char-p (char-at (- p 1)))) (setq p (- p 1)))
    p))

(defun del-fwd (k)
  "Delete K characters AFTER point. delete-char only deletes BEFORE point, so
move right K and delete K -- those K become the chars that were ahead."
  (let ((avail (- (buffer-length) (point))))
    (let ((k2 (if (< k avail) k avail)))
      (if (> k2 0) (progn (goto-char (+ (point) k2)) (delete-char k2)) nil))))

(defun kill-line ()
  "C-k: kill to end of line; if already at the line end, eat the newline."
  (let ((e (eol-pos)))
    (if (> e (point)) (del-fwd (- e (point)))
      (if (< (point) (buffer-length)) (del-fwd 1) nil))))

(defun kill-word ()       ; M-d
  (let ((e (fwd-word-pos))) (if (> e (point)) (del-fwd (- e (point))) nil)))

;; ---- the major modes (the MODEL is in modes.l; definitions live here) ----
;; Each mode names handler functions that touch the buffer (insert, prompt,
;; eval). The descriptor order is
;;   (parent keymap on-self-insert on-return setup-fn doc mode-line-name).
;; Hierarchy (behavior axis): special-mode is the inert root; surface-mode and
;; fundamental-mode are its two children. "Graphics is the substrate" lives on
;; the RENDERING axis (rd_core paints pixels for all buffers), not here.
;;   special-mode
;;     surface-mode          (a pixel canvas; ignores typing)
;;     fundamental-mode      (adds text self-insert / RET)
;;       text / repl / lisp-interaction

(defun ignore-char (ch) ch)              ; inert: a key in the canvas does nothing
(defun ignore-ret () nil)
(register-mode 'special-mode
  (list nil nil 'ignore-char 'ignore-ret nil
        "Special: an inert displayed buffer. Typing does nothing; ctrl/meta keys
fall through to the global keymap (so C-x o etc. still work)."
        "Special"))

(register-mode 'surface-mode
  (list 'special-mode nil nil nil nil
        "Surface: a pixel canvas an external renderer (e.g. the teapot) draws
into. Keys do nothing -- it is not text."
        "Surface"))

(defun fundamental-self-insert (ch) (insert (string-from-char ch)))
(defun fundamental-return () (insert "\n"))
(register-mode 'fundamental-mode
  (list 'special-mode nil 'fundamental-self-insert 'fundamental-return nil
        "Fundamental mode: text self-inserts; RET inserts a newline."
        "Fundamental"))

(register-mode 'text-mode
  (list 'fundamental-mode nil nil nil nil
        "Text mode: plain text editing. C-x C-s saves to the visited file."
        "Text"))

(defun repl-self-insert (ch) (insert (string-from-char ch)))
(defun repl-return () (repl-eval))
(defun repl-setup () (goto-char (buffer-length)) (prompt))
(setq repl-keymap
  (list
    (list 'ctrl 112 'cmd-history-prev "C-p    previous input (history)")
    (list 'ctrl 110 'cmd-history-next "C-n    next input (history)")))
(register-mode 'repl-mode
  (list 'fundamental-mode repl-keymap 'repl-self-insert 'repl-return 'repl-setup
        "REPL mode: RET evaluates the input; C-p/C-n walk the input history."
        "REPL"))

(defun scratch-setup ()
  (insert ";; *scratch* -- type a Lisp form and press C-j to evaluate it.\n\n"))
(setq lisp-interaction-keymap
  (list (list 'ctrl 106 'eval-last-sexp "C-j    eval the form before point")))
(register-mode 'lisp-interaction-mode
  (list 'fundamental-mode lisp-interaction-keymap nil nil 'scratch-setup
        "Lisp Interaction: RET=newline; C-j evaluates the form before point."
        "Lisp Interaction"))

(defun set-major-mode (buf mode-sym)
  "Make BUF use MODE-SYM: record it (buffer-local), select it, paint the mode
line, and run the mode's setup-fn."
  (buffer-local-set buf 'major-mode mode-sym)
  (set-buffer buf)
  (set-mode-line-name (mode-line-name-of mode-sym))
  ;; this Lisp has no funcall: (eval (list s)) calls the setup fn named by symbol s
  (let ((s (mode-setup-fn mode-sym))) (if s (eval (list s)) nil))
  mode-sym)

;; ---- lisp-interaction: evaluate the form before point (C-j) ----
(defun sexp-ws-p (c) (or (= c 32) (= c 10) (= c 9)))   ; space/newline/tab

(defun last-sexp-start (p)
  "Scan left from P over one balanced form; return its start position."
  (let ((q (- p 1)) (lo (edit-lo)))
    (while (and (> q lo) (sexp-ws-p (char-at q))) (setq q (- q 1)))  ; trailing ws
    (if (and (>= q 0) (= (char-at q) 41))            ; ')' -> match parens back
        (let ((depth 0) (done nil))
          (while (not done)
            (let ((c (char-at q)))
              ;; ')' deepens; the '(' that brings depth back to 0 is the matching opener
              (cond ((= c 41) (setq depth (+ depth 1)))
                    ((= c 40) (setq depth (- depth 1))))
              (cond ((and (= c 40) (= depth 0)) (setq done t))
                    ((<= q lo) (setq done t))
                    (t (setq q (- q 1))))))
          q)
      (progn                                          ; an atom: back over it
        (while (and (> q lo)
                    (not (sexp-ws-p (char-at (- q 1))))
                    (not (= (char-at (- q 1)) 40)))
          (setq q (- q 1)))
        q))))

(defun eval-last-sexp ()
  "C-j in lisp-interaction-mode: eval the form before point; insert its value."
  (let ((start (last-sexp-start (point))) (end (point)))
    (let ((src (buffer-substring start end)))
      (goto-char end)
      (insert "\n")
      (let ((val (eval (read-string src))))
        (flush-output)
        (insert (prin1-to-string val))
        (insert "\n")))))

;; ---- keymaps as DATA (the self-documenting heart) -----------------------
;; A binding is (kind code command description). dispatch looks a key up in a
;; keymap and runs its command; C-h b/k/m read the SAME tables -- so the help
;; can never drift from what the keys actually do. Commands are named functions
;; so C-h k can show each one's living source via describe-function-text.

(defun cmd-beginning-of-line () (goto-char (bol-pos)))
(defun cmd-end-of-line () (goto-char (eol-pos)))
(defun cmd-backward-char ()
  (let ((lo (edit-lo))) (if (> (point) lo) (goto-char (- (point) 1)) nil)))
(defun cmd-forward-char ()
  (if (< (point) (buffer-length)) (goto-char (+ (point) 1)) nil))
(defun cmd-delete-forward () (del-fwd 1))
(defun cmd-forward-word () (goto-char (fwd-word-pos)))
(defun cmd-backward-word () (goto-char (bwd-word-pos)))
(defun cmd-history-prev () (repl-history-prev))
(defun cmd-history-next () (repl-history-next))
(defun cmd-describe-function () (mb-start 'describe))

(setq global-keymap
  (list
    (list 'ctrl 97  'cmd-beginning-of-line "C-a    beginning of line")
    (list 'ctrl 101 'cmd-end-of-line       "C-e    end of line")
    (list 'ctrl 98  'cmd-backward-char     "C-b    backward char (Left)")
    (list 'ctrl 102 'cmd-forward-char      "C-f    forward char (Right)")
    (list 'ctrl 100 'cmd-delete-forward    "C-d    delete forward char")
    (list 'ctrl 107 'kill-line             "C-k    kill to end of line")
    (list 'ctrl 112 'cmd-history-prev      "C-p    previous history (Up)")
    (list 'ctrl 110 'cmd-history-next      "C-n    next history (Down)")
    (list 'meta 102 'cmd-forward-word      "M-f    forward word")
    (list 'meta 98  'cmd-backward-word     "M-b    backward word")
    (list 'meta 100 'kill-word             "M-d    kill word")))

(defun cmd-delete-other-windows ()
  "C-x 1: the selected window fills the frame again. Any *Help* window is gone,
so forget help-buf and let show-help re-split next time."
  (delete-other-windows)
  (setq help-buf nil)
  (redisplay))

(defun switch-to-buffer ()
  "C-x b: pick a buffer by name in the minibuffer and show it here."
  (mb-start 'switch))

(setq cx-keymap
  (list
    (list 'char 49  'cmd-delete-other-windows "C-x 1  delete other windows")
    (list 'char 50  'split-repl-below "C-x 2  split below (new REPL)")
    (list 'char 51  'split-repl-right "C-x 3  split right (new REPL)")
    (list 'char 48  'delete-window    "C-x 0  delete window")
    (list 'char 111 'other-window     "C-x o  other window")
    (list 'char 98  'switch-to-buffer "C-x b  switch to buffer")
    (list 'ctrl 98  'list-buffers     "C-x C-b  list buffers")
    (list 'ctrl 115 'save-buffer      "C-x C-s  save buffer")
    (list 'char 114 'repl-here       "C-x r    open a REPL in this window")))

(setq help-keymap
  (list
    (list 'char 102 'cmd-describe-function "C-h f  describe function")
    (list 'char 107 'describe-key          "C-h k  describe key")
    (list 'char 98  'describe-bindings     "C-h b  list key bindings")
    (list 'char 109 'describe-mode         "C-h m  describe mode")))

(defun run-key (ev km)
  "If EV is bound in KM, run its command and return t; otherwise nil."
  (let ((b (key-lookup ev km)))
    (if b (progn (eval (list (nth 2 b))) t) nil)))

(defun handle-cx (ev)
  "The C-x map (data-driven). C-x C-s is two CTRL events, so we dispatch on
the whole event, not a bare char."
  (setq pending-cx nil)
  (if (run-key ev cx-keymap) nil (echo "unbound C-x key")))

(defun run-mode-or-global (ev)
  "Run EV via the current mode's keymap chain, else the global keymap."
  (let ((b (mode-key-lookup (buffer-mode (current-buffer)) ev)))
    (if b (progn (eval (list (nth 2 b))) t)
      (if (run-key ev global-keymap) t (progn (echo "unbound key") nil)))))

(defun mode-backspace (m)
  "Backspace in mode M: in a REPL never past the prompt; else one char."
  (if (eq m 'repl-mode)
      (if (> (point) (cur-start)) (delete-char 1) nil)
    (if (> (point) 0) (delete-char 1) nil)))

(defun dispatch (ev)
  "Route one cooked input event through the selected buffer's major mode."
  (cond
    ((null ev) nil)
    (mb-action (mb-dispatch ev))                          ; minibuffer owns input
    (pending-cx (handle-cx ev))                           ; C-x <key>
    ((eq (car ev) 'mouse)
     (select-window-at (cadr ev) (caddr ev)))
    ((eq (car ev) 'meta)
     (if (= (cadr ev) 120) (mb-start 'mx)                 ; M-x
       (run-mode-or-global ev)))
    (pending-ch
     (setq pending-ch nil)
     (if (run-key ev help-keymap) nil (echo "unbound C-h key")))
    ((eq (car ev) 'ctrl)
     (cond ((= (cadr ev) 120) (setq pending-cx t))        ; C-x: await suffix
           ((= (cadr ev) 104) (setq pending-ch t))        ; C-h: await suffix
           ((= (cadr ev) 103) (setq pending-cx nil)       ; C-g: quit
                              (echo "quit"))
           (t (run-mode-or-global ev))))                  ; mode keymap then global
    ((eq (car ev) 'char)
     (let ((m (buffer-mode (current-buffer))) (ch (cadr ev)))
       (cond
         ((= ch 10) (eval (list (mode-return-fn m))))     ; RET: mode decides
         ((= ch 8) (mode-backspace m))                    ; backspace
         (t (eval (list (mode-self-insert-fn m) ch))))))  ; printable: mode decides
    (t nil)))

;; ---- visiting files: open, edit, save (Phase 27.3) ----------------------

(defun find-file (path)
  "Open PATH in a split window below; the file shows there and the buffer
remembers its path (so C-x C-s knows where to write). A missing file starts
empty. We return selection to the REPL before finishing -- like (teapot) --
so the REPL's own 'print the value and re-prompt' lands in the REPL, not in
the file buffer. Switch to the file with C-x o to start editing."
  (split-below)
  (other-window)                           ; into the new (bottom) window
  (let ((b (make-buffer path)))
    (set-buffer b)                         ; show the file there
    (setq file-of (cons (cons b path) file-of))
    (let ((fd (open path)))
      (if (>= fd 0)
          (progn
            (let ((chunk (fd-read fd 1024)))
              (while chunk (insert chunk) (setq chunk (fd-read fd 1024))))
            (close fd))
        nil))                              ; nonexistent: a fresh empty buffer
    (goto-char 0)
    (other-window)                         ; back to the REPL (see docstring)
    (redisplay)
    b))

(defun save-buffer ()
  "Write the current buffer back to the file it is visiting (C-x C-s)."
  (let ((path (cdr (assoc (current-buffer) file-of))))
    (if path
        (let ((fd (creat path)))
          (if (>= fd 0)
              (progn (fd-write fd (buffer-substring 0 (buffer-length)))
                     (close fd)
                     (echo (string-concat "saved " path)))
            (echo "save failed")))
      (echo "no file for this buffer"))))

;; ---- the minibuffer: vertico-style command selection -------------------
;;
;; The echo area grows into a live-narrowing candidate list: type to filter,
;; C-n/C-p (or arrows) to move the selection bar, RET to commit, C-g to
;; cancel, TAB to adopt the selection as input. M-x completes over every
;; symbol with a function -- the symbol table IS the command palette.

(setq mb-action nil)        ; nil = inactive; 'mx or 'describe
(setq mb-input "")
(setq mb-matches nil)
(setq mb-sel 0)              ; selected candidate, an index into ALL matches
(setq mb-top 0)             ; first VISIBLE candidate -- the scroll offset
(setq mb-rows 8)            ; how many candidates the echo area shows at once

(defun command-symbols ()
  "Every symbol whose function slot holds something runnable."
  (filter (lambda (s) (function-info s)) (all-symbols)))

;; The minibuffer is generic over its candidates: each action supplies a
;; SOURCE (the full candidate list) and a LABEL (the display/match string for
;; one candidate). M-x/describe complete over command symbols; switch-to-buffer
;; completes over (handle . name) buffer entries.
(defun mb-source ()
  (if (eq mb-action 'switch) (buffer-list) (command-symbols)))

(defun mb-label (c)
  (if (eq mb-action 'switch) (cdr c) (symbol-name c)))

(defun mb-filter ()
  "Narrow the action's candidates to those whose label contains mb-input."
  (setq mb-matches
        (filter (lambda (c)
                  (or (= (string-length mb-input) 0)
                      (string-search mb-input (mb-label c))))
                (mb-source)))
  (setq mb-sel 0)
  (setq mb-top 0))

(defun take (n lst)
  (if (or (= n 0) (null lst)) nil
    (cons (car lst) (take (- n 1) (cdr lst)))))

(defun mb-move (d)
  "Move the selection by D over ALL matches, scrolling the visible window
(mb-top .. mb-top+mb-rows) to keep the selection inside it -- vertico's
behavior, the same minimal-scroll rule the buffer uses for point."
  (let ((n (length mb-matches)))
    (if (> n 0)
        (progn
          (setq mb-sel (+ mb-sel d))
          (if (< mb-sel 0) (setq mb-sel 0) nil)
          (if (>= mb-sel n) (setq mb-sel (- n 1)) nil)
          (if (< mb-sel mb-top) (setq mb-top mb-sel) nil)               ; scrolled up
          (if (>= mb-sel (+ mb-top mb-rows))                            ; scrolled down
              (setq mb-top (- mb-sel (- mb-rows 1))) nil))
      nil)))

(defun mb-render ()
  "Paint the minibuffer: prompt + a position counter on top, then the visible
slice of candidates (mb-top onward), the selected one highlighted (echo-select
drives the face-2 bar)."
  (let ((n (length mb-matches))
        (txt (string-concat
              (cond ((eq mb-action 'mx) "M-x ")
                    ((eq mb-action 'switch) "Buffer: ")
                    (t "Describe: "))
              mb-input)))
    (if (> n 0)
        (setq txt (string-concat txt "  " (prin1-to-string (+ mb-sel 1))
                                 "/" (prin1-to-string n))) nil)
    (foreach (lambda (c) (setq txt (string-concat txt "\n  " (mb-label c))))
             (take mb-rows (nthcdr mb-top mb-matches)))
    (echo txt)
    (echo-select (+ (- mb-sel mb-top) 1))))

(defun mb-start (action)
  (setq mb-action action)
  (setq mb-input "")
  (mb-filter)
  (mb-render))

(defun mb-stop ()
  (setq mb-action nil)
  (echo "ready"))

(defun describe-function-text (sym)
  "The living object, not documentation about it: for a defun this prints
the very lambda the machine runs -- which you can redefine."
  (let ((info (function-info sym)))
    (cond
      ((null info) (string-concat (symbol-name sym) " is not a function"))
      ((eq (car info) 'primitive)
       (string-concat (symbol-name sym) " is a C primitive taking "
                      (caddr info) ".." (caddr (cdr info)) " args"))
      (t (string-concat (symbol-name sym) " is a " (symbol-name (car info))
                        "\n\nparameters: " (prin1-to-string (cadr info))
                        "\n\nsource:\n" (prin1-to-string (caddr info)))))))

(setq help-buf nil)

(defun show-help (txt)
  "Pop *Help* in the other window (splitting on first use), Emacs-style."
  (if help-buf nil
    (progn (setq help-buf (make-buffer "*Help*"))
           (split-below)))
  (other-window)
  (set-buffer help-buf)
  (goto-char (buffer-length))
  (delete-char (buffer-length))
  (insert txt)
  (goto-char 0)          ; show *Help* from the TOP, like Emacs (scroll follows point)
  (other-window))

;; ---- self-documenting help: C-h k / C-h b / C-h m -----------------------

(defun fmt-bindings (km)
  "One description line per binding in KM."
  (let ((s ""))
    (foreach (lambda (b) (setq s (string-concat s (nth 3 b) "\n"))) km)
    s))

(defun list-buffers ()
  "C-x C-b: every live buffer in a *Help* window, the current one marked '*'."
  (let ((here (current-buffer))
        (txt "=== Buffers ===\n\n"))
    (foreach (lambda (e)
               (setq txt (string-concat txt
                 (if (= (car e) here) "* " "  ")
                 (prin1-to-string (car e)) "  " (cdr e) "\n")))
             (buffer-list))
    (show-help txt)))

(defun describe-bindings ()
  "C-h b: every key binding, in a *Help* window -- read from the live keymaps."
  (show-help (string-concat
    "=== Key bindings ===\n\n"
    (fmt-bindings global-keymap)
    (fmt-bindings cx-keymap)
    (fmt-bindings help-keymap)
    "\nC-x      prefix\nC-h      prefix (help)\nC-g      quit\n"
    "M-x      run a command by name\nRET      evaluate (REPL) / newline (file)\n")))

(defun describe-mode ()
  "C-h m: describe the selected buffer's mode and the keys that work in it."
  (show-help (string-concat
    ;; `editing-p` is gone -- a buffer is a REPL when its major-mode IS
    ;; repl-mode. (Task 8 makes this fully mode-aware; here we just preserve the
    ;; old two-branch wording with the equivalent mode test.)
    (if (not (eq (buffer-mode (current-buffer)) 'repl-mode))
        "=== File buffer ===\n\nText self-inserts; RET inserts a newline.\nC-x C-s saves to disk.\n\n"
      "=== REPL ===\n\nRET evaluates the input; the value prints below.\nUp/Down walk the input history.\n\n")
    "Key bindings:\n\n"
    (fmt-bindings global-keymap)
    (fmt-bindings cx-keymap))))

(defun describe-binding (b prefix)
  (if b
      (show-help (string-concat prefix (nth 3 b) "\n\nruns " (symbol-name (nth 2 b))
                                "\n\n" (describe-function-text (nth 2 b))))
    (show-help (string-concat prefix "key is unbound (or self-inserts)"))))

(defun describe-key ()
  "C-h k: read a key (following a C-x / C-h prefix) and report the command it
runs, with that command's living source -- read from the same keymaps dispatch
uses, so it is always accurate."
  (echo "Describe key: press a key")
  (redisplay)
  (let ((ev (read-event)))
    (cond
      ((and (eq (car ev) 'ctrl) (= (cadr ev) 120))
       (describe-binding (key-lookup (read-event) cx-keymap) "C-x "))
      ((and (eq (car ev) 'ctrl) (= (cadr ev) 104))
       (describe-binding (key-lookup (read-event) help-keymap) "C-h "))
      (t (describe-binding (key-lookup ev global-keymap) "")))))

(defun mb-commit ()
  (let ((sel (if mb-matches (nth mb-sel mb-matches) nil))
        (action mb-action))
    (mb-stop)
    (cond
      ((eq action 'switch)                    ; sel is (handle . name)
       (if sel (progn (set-buffer (car sel)) (redisplay)) (echo "no such buffer")))
      ((eq action 'mx)
       (let ((sym (if sel sel (read-string mb-input))))
         (let ((val (eval (list sym))))
           (flush-output) (insert (prin1-to-string val)) (insert "\n") (prompt))))
      ((eq action 'describe)
       (show-help (describe-function-text (if sel sel (read-string mb-input))))))))

(defun mb-dispatch (ev)
  "The minibuffer keymap -- live data, like everything else here."
  (cond
    ((eq (car ev) 'ctrl)
     (cond ((= (cadr ev) 110) (mb-move 1) (mb-render))     ; C-n / Down
           ((= (cadr ev) 112) (mb-move -1) (mb-render))    ; C-p / Up
           ((= (cadr ev) 103) (mb-stop))                   ; C-g: cancel
           (t nil)))
    ((eq (car ev) 'char)
     (let ((ch (cadr ev)))
       (cond
         ((= ch 10) (mb-commit))                           ; RET
         ((= ch 9)                                         ; TAB: adopt selection
          (if mb-matches
              (progn (setq mb-input (mb-label (nth mb-sel mb-matches)))
                     (mb-filter) (mb-render))
            nil))
         ((= ch 8)                                         ; backspace
          (let ((n (string-length mb-input)))
            (if (> n 0) (setq mb-input (substring mb-input 0 (- n 1))) nil))
          (mb-filter) (mb-render))
         (t (setq mb-input (string-concat mb-input (string-from-char ch)))
            (mb-filter) (mb-render)))))
    (t nil)))

;; C-h prefix state (like pending-cx): C-h f = describe-function.
(setq pending-ch nil)

(defun spawn-vm ()
  "Start ANOTHER complete Lisp machine on its own seat: fork, and the child
becomes a fresh `lisp -frame` (its own image, window tree and framebuffer).
Switch with Ctrl-Alt-F1..F4 or (switch-seat n). Returns the child's pid."
  (let ((pid (fork)))
    (if (= pid 0)
        (progn (exec "/bin/lisp" (list "lisp" "-frame")) (exit 127))
      pid)))

;; ---- running programs INSIDE the frame ----------------------------------
;;
;; system.l's run-prog lets a child inherit fd 1 (the serial console). In the
;; frame, output belongs in the BUFFER -- so we redefine run-prog (loading
;; after system.l: the Lisp-machine way to specialize) to pipe the child's
;; stdout into the current buffer, streaming with a redisplay per chunk, and
;; to map C-c on the graphical keyboard to SIGINT for the child.
;; (Interactive stdin still comes from the serial console -- a full in-buffer
;; comint is a later refinement.)

(defun stream-thunk (thunk)
  "Run THUNK in a forked child wired to TWO pipes -- the child's fd 1 streams
into the current buffer (output), and the frame's keyboard streams into the
child's fd 0 (input). The terminal, finally complete inside the frame: while
the child runs, what you type is echoed into the buffer and edited in a COOKED
line (backspace fixes it locally, the program never sees the editing); RET
delivers the line, C-d is end-of-input, and C-c interrupts the whole job. The
general mechanism behind EVERY command in the frame: single programs and whole
pipelines alike.

Before this, a frame child inherited the frame's fd 0 -- the serial console
nobody was typing on -- so anything that READ stdin (wc, a prompt) hung. Now
the input comes from the very keyboard you are using."
  (let ((out (pipe))                ; child stdout -> (car out), read by us
        (inp (pipe)))               ; child stdin  <- (cdr inp), written by us
    (let ((pid (fork)))
      (if (= pid 0)
          (progn (setpgid 0 0)               ; become a JOB leader: programs we
                                             ; fork inherit the group, so one
                                             ; group-kill interrupts them all
                 (setq in-pipeline t)        ; descendants run raw
                 (dup2 (cdr out) 1)          ; our stdout is the output pipe
                 (dup2 (cdr out) 2)          ; and stderr too, so errors/usage
                                             ; land in the buffer, not the serial
                                             ; port (busybox prints help to fd 2)
                 (dup2 (car inp) 0)          ; our stdin is the input pipe
                 (close (car out)) (close (cdr out))
                 (close (car inp)) (close (cdr inp))
                 (let ((v (funcall thunk)))
                   (exit (if (numberp v) v 0))))
        (progn
          (setpgid pid pid)                  ; parent sets it TOO (the classic
                                             ; shell double-set: no race if C-c
                                             ; lands before the child runs)
          (close (cdr out))                  ; we only READ the child's output
          (close (car inp))                  ; and only WRITE its input
          ;; sin = is our stdin write end still open? C-d closes it early; if
          ;; the child exits first we close it after the loop. Closing twice
          ;; would be a bug, so we track it. line = the COOKED input line: a
          ;; canonical terminal buffers a line and lets you edit it (backspace)
          ;; before delivering it on RET, so the program never sees the editing.
          (let ((running t) (sin t) (line ""))
            (while running
              (if (poll-fd (car out) 50)
                  (let ((chunk (fd-read (car out) 1024)))
                    (if chunk
                        (progn (insert chunk) (redisplay))
                      (setq running nil)))
                ;; Quiet 50ms: nothing arrived, but if a live surface is
                ;; animating (the teapot) we must STILL repaint -- redisplay
                ;; is what blits surface canvases, and a slow-printing child
                ;; (ping: one line a second) must not become the frame rate.
                (if animate (redisplay) nil))
              (let ((ev (read-event-nowait)))
                (cond
                  ((null ev) nil)
                  ((and (eq (car ev) 'ctrl) (= (cadr ev) 99))   ; C-c
                   ;; Signal the GROUP (-pid), not the pid: our child is only
                   ;; a Lisp wrapper -- the program doing the work (ping) is
                   ;; its child, and killing the wrapper alone would orphan
                   ;; it, still streaming. SIGINT to -pid reaches them all.
                   (kill (- 0 pid) 2)
                   (echo "C-c: interrupted"))
                  ((and (eq (car ev) 'ctrl) (= (cadr ev) 100))  ; C-d: EOF
                   ;; Canonical EOF: a pending (unsent) line is delivered NOW
                   ;; without a newline; on an empty line we close the write end
                   ;; so the child's next read returns 0 -- end of input. (Real
                   ;; ttys do exactly this: C-d twice ends input mid-line.)
                   (if (> (string-length line) 0)
                       (progn (if sin (fd-write (cdr inp) line) nil) (setq line ""))
                     (if sin (progn (close (cdr inp)) (setq sin nil)) nil)))
                  ((eq (car ev) 'char)
                   (let ((ch (cadr ev)))
                     (cond
                       ((= ch 10)                  ; RET: deliver the cooked line
                        (insert (string-from-char 10)) (redisplay)
                        (if sin (fd-write (cdr inp)
                                          (string-concat line (string-from-char 10)))
                          nil)
                        (setq line ""))
                       ((= ch 8)                   ; backspace: edit locally only
                        (if (> (string-length line) 0)
                            (progn (setq line (substring line 0
                                                          (- (string-length line) 1)))
                                   (delete-char 1) (redisplay))
                          nil))
                       (t                          ; ordinary char: echo + buffer
                        (insert (string-from-char ch)) (redisplay)
                        (setq line (string-concat line (string-from-char ch)))))))
                  (t nil))))
            (close (car out))
            (if sin (close (cdr inp)) nil)   ; child exited before C-d
            (cdr (wait))))))))

(defun run-prog (path argv)
  "Frame edition: stream output into the buffer -- unless we ARE a pipeline
stage, where fds were already plumbed and raw inheritance is the contract."
  (if in-pipeline
      (run-prog-raw path argv)
    (stream-thunk (lambda () (run-prog-raw path argv)))))

(defun pipe-stages (left right)
  "Frame edition: the whole pipeline's output streams into the buffer."
  (if in-pipeline
      (pipe-stages-raw left right)
    (stream-thunk (lambda () (pipe-stages-raw left right)))))

(defun flush-output ()
  "Drain captured image output -- (princ ...) text and ERROR lines -- into
the current buffer, where the person at the frame can actually see it."
  (let ((out (frame-output)))
    (if (> (string-length out) 0)
        (progn (insert out) (redisplay))
      nil)))

(defun frame-setup ()
  "Acquire the screen, then open *scratch* in lisp-interaction-mode (Emacs-style)."
  (if (frame-init)
      (progn
        (let ((sb (make-buffer "*scratch*")))
          (set-major-mode sb 'lisp-interaction-mode))     ; selects + banner
        (echo "ready")
        (redisplay)
        t)
    (progn (princ "frame: no display\n") nil)))

;; When t, the event loop polls input at ~25Hz instead of blocking, so
;; surface buffers with live renderers (the teapot!) keep repainting.
(setq animate nil)

(defun teapot ()
  "The Utah teapot, spinning in a buffer: split the window, give the new
half a surface canvas, and run /bin/teapot (TinyGL) into it."
  (split-below)
  (other-window)
  (let ((sb (make-surface-buffer "*teapot*" 480 360)))
    (set-buffer sb)
    (other-window)
    (setq animate t)
    (run-in-buffer sb "teapot")))

(defun frame-tick ()
  "ONE beat of the machine: show anything printed since last time (including
an error from the previous tick -- each tick gets a fresh C-level recovery,
so a typo aborts one keystroke's work, never the loop), handle one event,
show its output, repaint. read-event BLOCKS in the kernel; redisplay paints
only what changed -- an idle machine costs nothing."
  (let ((pending (frame-output)))
    (if (> (string-length pending) 0)
        ;; Output pending at the TOP of a tick means the previous tick was
        ;; aborted mid-eval (a typo): show what it printed -- the ERROR line
        ;; -- and lay a fresh prompt, which also re-anchors repl-start past
        ;; the broken input so the next RET doesn't re-eval it.
        (progn (insert pending) (insert "\n")
               (if (eq (buffer-mode (current-buffer)) 'repl-mode) (prompt) nil)
               (redisplay))
      nil))
  (if animate
      ;; Animation heartbeat: poll input, repaint at ~25Hz so live surface
      ;; renderers show. (The blocking path below stays the idle default.)
      (progn (let ((ev (read-event-nowait)))
               (if ev (dispatch ev) nil))
             (sleep 40))
    (dispatch (read-event)))
  (flush-output)
  (redisplay))

(princ "frame.l loaded.")
(terpri)
