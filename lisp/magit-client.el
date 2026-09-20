;; -*- lexical-binding: t; -*-

(require 'msgpack)
(require 'cl-lib)

(defvar magit-server-processes nil)
(defvar magit-server-buffers nil)

(defun magit-pack-message (object)
  (let* ((payload (msgpack-encode object)))
    (when (and payload (multibyte-string-p payload))
      (setq payload (string-as-unibyte payload)))
    (let* ((len (length payload))
           (prefix (string
                    (logand (lsh len -24) #xff)
                    (logand (lsh len -16) #xff)
                    (logand (lsh len -8) #xff)
                    (logand len #xff))))
      (string-as-unibyte (concat prefix payload)))))

(defun magit-server-get-process (git-root)
  (cdr (assoc git-root magit-server-processes)))

(defun magit-server-set-process (git-root process)
  (setf (alist-get git-root magit-server-processes nil nil #'equal)
        process))

(defun magit-server-get-buffer (git-root)
  (or (cdr (assoc git-root magit-server-buffers))
      ""))

(defun magit-server-set-buffer (git-root buffer)
  (setf (alist-get git-root magit-server-buffers nil nil #'equal)
        buffer))

(defun magit-server-sentinel (process event git-root)
  (when (memq (process-status process) '(exit signal))
    (magit-server-set-buffer git-root nil)
    (magit-server-set-process git-root nil)))

(defun magit-server-start (git-root)
  (or (magit-server-get-process git-root)
      (let ((process
             (let ((default-directory git-root))
               (make-process
                :name (format "magit-server:%s" git-root)
                :command (list (expand-file-name "~/.emacs.d/elpa/magit/cmake-build-debug/src/magit-server") default-directory)
                :coding 'binary
                :connection-type 'pipe
                :filter #'magit-server-filter
                :sentinel (lambda (process event)
                            (magit-server-sentinel process event git-root))))))
        (process-put process 'git-root git-root)
        (magit-server-set-process git-root process)
        (magit-server-set-buffer git-root "")
        process)))

(defun magit-server-stop (git-root)
  (let ((process (magit-server-get-process git-root)))
    (when (process-live-p process)
      (delete-process process)))

  (setf (alist-get git-root magit-server-processes nil nil #'equal)
        nil)

  (setf (alist-get git-root magit-server-buffers nil nil #'equal)
        nil))

(defun magit-server-send-async (root msg callback)
  (let* ((process (magit-server-get-process root))
         (request-id (1+ (or (process-get process 'request-counter) 0)))
         (msg-with-id (cons (cons 0 request-id) msg)))

    (process-put process 'request-counter request-id)

    (process-put
     process
     'pending-callbacks
     (cons (cons request-id callback)
           (process-get process 'pending-callbacks)))

    (process-send-string
     process
     (magit-pack-message msg-with-id))))

(defun magit-server-find-callback (process request-id)
  (let ((entry (assoc request-id (process-get process 'pending-callbacks))))
    (when entry
      (process-put process 'pending-callbacks
                   (delete entry (process-get process 'pending-callbacks)))
      (cdr entry))))

(defun magit-server-filter (process chunk)
  (let* ((git-root (process-get process 'git-root))
         (buffer (concat (magit-server-get-buffer git-root)
                         chunk)))

    ;; (message "[%s] received %d bytes"
    ;;          git-root
    ;;          (length chunk))

    (catch 'magit-server-filter-done
      (while (>= (length buffer) 4)
        (let ((len (+ (lsh (aref buffer 0) 24)
                      (lsh (aref buffer 1) 16)
                      (lsh (aref buffer 2) 8)
                      (aref buffer 3))))

          (if (< (length buffer) (+ 4 len))
              (progn
                (magit-server-set-buffer git-root buffer)
                (throw 'magit-server-filter-done nil))
            (let* ((payload (substring buffer 4 (+ 4 len)))
                   (remaining (substring buffer (+ 4 len)))
                   (msg (msgpack-read-from-string payload)))
              ;; (message "[%s] decoded: %S"
              ;;          git-root
              ;;          msg)
              (let* ((request-id (cdr (assq 0 msg)))
                     (callback
                      (and request-id
                           (magit-server-find-callback
                            process
                            request-id))))
                (when request-id (cl-assert callback))
                (funcall callback msg))
              (setq buffer remaining))))))
    (magit-server-set-buffer git-root buffer)))

(cl-defun make-magit-status-msg (&key cmd-id default-dir)
  (list
   (cons 1 cmd-id)
   (cons 2 default-dir)))

(defun magit-status-req-test ()
  (let ((root (expand-file-name "~/.emacs.d/elpa/magit")))
    (require 'magit-client)
    (magit-server-start root)
    (magit-server-send-async
     root
     (make-magit-status-msg :cmd-id 1 :default-dir default-directory)
     (lambda (response)
       (message "ASYNC RESPONSE: %S" response)))))

;; (defun magit-status-req (root callback)
;;   (require 'magit-client)
;;   (magit-server-start root)
;;   (magit-server-send-async
;;    root
;;    (make-magit-status-msg
;;     :cmd-id 1
;;     :default-dir default-directory)
;;    callback))

(defun magit-status-req (root callback)
  (let ((root (expand-file-name (directory-file-name (vc-root-dir)))))
    (require 'magit-client)
    (magit-server-start root)
    (magit-server-send-async
     root
     (make-magit-status-msg :cmd-id 1 :default-dir default-directory)
     callback)))

(provide 'magit-client)
