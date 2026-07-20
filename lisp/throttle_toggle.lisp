; ============================================================================
; Minimal teaching example for the VESC (LispBM):
; TURNING THE THROTTLE ON / OFF by a command from the BLE helper.
;
; How it works:
;   * The BLE helper (shutter button) or the display sends a
;     COMM_CUSTOM_APP_DATA packet over CAN framed as ['V' 'P'][msg][reply-id]…
;     (protocol: components/vesc_can/include/vesc_can/vesc_lisp_panel.h).
;   * The script declares a "panel" with a single toggle "Throttle" (id 1).
;     A button press on the remote arrives as a toggle command.
;   * throttle-on = 1 → the stock throttle (the VESC ADC app) works as
;     usual, the script touches nothing.
;   * throttle-on = 0 → the ADC app's output is suppressed
;     (app-disable-output), the motor coasts.
;
; Fail-safe: the output suppression is extended every 100 ms by 1 s. If the
; script dies, the suppression expires and the stock throttle comes back on
; its own.
;
; Full version (throttle/brake/cruise/PAS arbiter, profiles) — main.lisp.
; ============================================================================

(def throttle-on 1)

; --- packing panel replies ---------------------------------------------------
(def pbuf (bufcreate 64))
(def pi 0)
(defun pu8  (v) { (bufset-u8  pbuf pi v) (setq pi (+ pi 1)) })
(defun pi32 (v) { (bufset-i32 pbuf pi (to-i32 v)) (setq pi (+ pi 4)) })
(defun pstr (s) { (bufcpy pbuf pi s 0 (buflen s)) (setq pi (+ pi (buflen s))) })

; UI_DESC (0x81): version 1, one control — TOGGLE id=1 "Throttle"
(defun send-ui (reply-id) {
    (setq pi 0)
    (pu8 0x56) (pu8 0x50) (pu8 0x81) (pu8 1) (pu8 1)
    (pu8 1) (pu8 1) (pstr "Throttle") (pu8 (if (= throttle-on 1) 1 0))
    (send-data pbuf 2 reply-id)
})

; STATE (0x82): current toggle value (value*1000)
(defun send-state (reply-id) {
    (setq pi 0)
    (pu8 0x56) (pu8 0x50) (pu8 0x82) (pu8 1)
    (pu8 1) (pi32 (* (if (= throttle-on 1) 1 0) 1000))
    (send-data pbuf 2 reply-id)
})

; --- the actual throttle on/off ----------------------------------------------
(defun set-throttle (on) {
    (setq throttle-on on)
    (print (if (= on 1) "Throttle ON" "Throttle OFF"))
})

; --- receiving 'VP' frames from the helper/display ---------------------------
(defun handle (data) {
    (if (and (>= (buflen data) 4)
             (= (bufget-u8 data 0) 0x56)
             (= (bufget-u8 data 1) 0x50))
        (let ((msg      (bufget-u8 data 2))
              (reply-id (bufget-u8 data 3))) {
            (cond
                ((= msg 0x01) (send-ui reply-id))      ; panel description request
                ((= msg 0x03) (send-state reply-id))   ; state request
                ((= msg 0x06) {                        ; atomic toggle from the helper
                    (set-throttle (if (= throttle-on 1) 0 1))
                    (send-state reply-id)
                })
                ((= msg 0x02)                          ; explicit action (on/off)
                    (let ((cid (bufget-u8 data 4))
                          (val (/ (bufget-i32 data 5) 1000.0))) {
                        (if (= cid 1) (set-throttle (if (> val 0.5) 1 0)))
                        (send-state reply-id)
                    })))
        })))

(defun event-loop ()
    (loopwhile t
        (recv ((event-data-rx . (? data)) (handle data))
              (_ nil))))

(event-register-handler (spawn event-loop))
(event-enable 'event-data-rx)

; --- main loop ----------------------------------------------------------------
(loopwhile t {
    (if (= throttle-on 0) {
        (app-disable-output 1000)   ; suppress the stock throttle (extend the ban)
        (set-current 0)             ; and explicitly hold the coast
    })
    (sleep 0.1)
})
