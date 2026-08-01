;; -*- lexical-binding: t; -*-

(require 'msgpack)

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

(defun magit-server-start (git-root)
  (or (magit-server-get-process git-root)
      (let ((process
             (let ((default-directory git-root))
               (make-process
                :name (format "magit-server:%s" git-root)
                :command
                (list (expand-file-name
                       "~/.emacs.d/elpa/magit/cmake-build-debug/src/magit-server"))
                :coding 'binary
                :connection-type 'pipe
                :filter #'magit-server-filter
                :sentinel #'magit-server-sentinel))))
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

    (message "[%s] received %d bytes"
             git-root
             (length chunk))

    (catch 'magit-server-filter-done
      (while (>= (length buffer) 4)
        (let ((len (+ (lsh (aref buffer 0) 24)
                      (lsh (aref buffer 1) 16)
                      (lsh (aref buffer 2) 8)
                      (aref buffer 3))))

          (if (< (length buffer) (+ 4 len))
              ;; Wait for more data.
              (progn
                (magit-server-set-buffer git-root buffer)
                (throw 'magit-server-filter-done nil))

            (let* ((payload (substring buffer 4 (+ 4 len)))
                   (remaining (substring buffer (+ 4 len)))
                   (msg (msgpack-read-from-string payload)))

              (message "[%s] decoded: %S"
                       git-root
                       msg)

              (let* ((request-id (cdr (assq 0 msg)))
                     (callback
                      (and request-id
                           (magit-server-find-callback
                            process
                            request-id))))

                (if callback
                    (funcall callback msg)
                  (message "magit-server: no pending callback for request-id %s; discarding message: %S"
                           request-id msg)))

              (setq buffer remaining))))))

    (magit-server-set-buffer git-root buffer)))


(defvar test-msg-id 0)

(cl-defun make-test-msg (&key cmd-id default-dir)
  (list
   (cons 1 (prog1 test-msg-id (setq test-msg-id (1+ test-msg-id))))
   (cons 2 cmd-id)
   (cons 3 default-dir)))

(defun test-msg ()
  (list
   (cons 1 1)
   (cons 2 1)
   (cons 3 (expand-file-name "~/.emacs.d/elpa/magit/src"))))


(defun test-send-async ()
  (let ((root (expand-file-name "~/.emacs.d/elpa/magit")))
    (require 'magit-client)
    (magit-server-start root)
    (magit-server-send-async
     root
     (make-test-msg :cmd-id 1 :default-dir default-directory)
     (lambda (response)
       (message "ASYNC RESPONSE: %S" response)))))

(provide 'magit-client)
