; ============================================================================
; Skeleton: receiving CUSTOM button commands from the BLE helper on the VESC.
;
; In the helper's GUI each button has a CAN frame: on every press the helper
; transmits it as-is (defaults: standard id 0x123, data = big-endian u16
; command = button number, e.g. 0001 for A, 0002 for B). Standard-id frames
; are ignored by the VESC protocol itself and delivered straight to this
; script via the event-can-sid event — the simplest possible command channel.
;
; Extend `on-command` below with whatever the commands should do.
; ============================================================================

(def cmd-id 0x123)    ; must match the CAN ID configured in the helper GUI

(defun on-command (cmd) {
    ; --- your command implementation goes here -------------------------
    (print (str-merge "helper cmd " (to-str cmd)))
    ; examples:
    ; (set-current-rel 0.5)                  ; motor command
    ; (foc-play-tone 0 800 0.4)              ; beep
    ; (setq my-mode (+ my-mode 1))           ; switch a mode variable
})

(defun proc-sid (id data)
    (if (= id cmd-id)
        (on-command (if (>= (buflen data) 2)
                        (bufget-u16 data 0)
                        (bufget-u8 data 0)))))

(defun event-loop ()
    (loopwhile t
        (recv ((event-can-sid (? id) . (? data)) (proc-sid id data))
              (_ nil))))

(event-register-handler (spawn event-loop))
(event-enable 'event-can-sid)

(loopwhile t (sleep 1))
