#pragma once

/* Config GATT service — the Python GUI's (and future Android app's) channel.
 * Deliberately separate from NUS: NUS stays a transparent VESC Tool↔CAN
 * bridge, this service owns helper configuration, live status and OTA.
 *
 * Service  ab1e0001-b1e5-4e15-8ac3-5e00c0de15b7
 *   CTRL     …0002  WRITE|WRITE_NR|NOTIFY   commands + responses
 *   STATUS   …0003  READ|NOTIFY             status blob, ~2 Hz while subscribed
 *   SCAN     …0004  NOTIFY                  scan results stream
 *   OTA-CTRL …0005  WRITE|NOTIFY            firmware update control (ble_ota)
 *   OTA-DATA …0006  WRITE_NR                firmware bytes (ble_ota)
 *
 * CTRL commands (first byte; multi-byte fields little-endian):
 *   0x01 SCAN          [u8 what: 1=button 2=cadence]
 *   0x02 BIND_BUTTON   [u8 addr_type][6B addr LE] — ADDS a remote (up to
 *                      BLE_HID_MAX_REMOTES); ack status 3 = list full
 *   0x03 BIND_CADENCE  [u8 addr_type][6B addr LE]
 *   0x04 UNBIND        [u8 what] — for buttons clears ALL remotes + learned
 *                      button signatures
 *   0x05 GET_PARAMS    []
 *   0x06 SET_PARAMS    [params blob v1]
 *   0x07 SET_THROTTLE  [u8 0=off 1=on 0xFF=toggle]
 *   0x08 SET_BINDING   [u8 idx][u8 ext][u8 len][u32 can_id LE][8B data] —
 *                      custom CAN frame sent when button idx has action 4
 *   0x09 GET_BINDING   [u8 idx]
 * Responses on CTRL notify: [0x80|cmd][u8 status 0=ok], except GET_PARAMS →
 * [0x85][params blob v2] and GET_BINDING → [0x89][idx][ext][len][can_id][data8].
 *
 * params blob v3 (35 B): ver u8=3, enabled u8, reverse u8, level u8,
 *   level_count u8, mode u8, start_current_pct u8, start_delay_ms u16,
 *   stop_delay_ms u16, min_cadence_rpm u16, full_cadence_rpm u16,
 *   max_current_ma u32, ramp_up_maps u32, controller_id u8, target_vesc_id u8,
 *   btn_action u8[8] (btn_action_t per learned button; see settings.h),
 *   can_kbps u16 (125/250/500/1000)
 *
 * status blob v2 (12 B): ver u8=2, flags u8 (bit0 cad_bound, bit1 cad_conn,
 *   bit2 any remote bound, bit3 any remote connected, bit4 scanning,
 *   bit5 throttle_on, bit6 throttle_valid, bit7 pas_enabled), centi_rpm i16,
 *   cad_battery u8, level u8, assist_ma i32, btn_pressed_mask u8 (bit N =
 *   learned button N down), btn_count u8 (buttons learned so far)
 *
 * scan frame: what u8, addr_type u8, addr 6B LE, rssi i8, name_len u8, name
 */

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_gatt.h"

#ifdef __cplusplus
extern "C" {
#endif

void ble_cfg_svc_init(void);

const struct ble_gatt_svc_def *ble_cfg_svc_get_svcs(void);
void ble_cfg_svc_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt,
                                   void *arg);

/* ble_host wiring (GAP events for the peripheral connection). */
void ble_cfg_svc_on_connect(uint16_t conn_handle);
void ble_cfg_svc_on_disconnect(void);
void ble_cfg_svc_on_subscribe(uint16_t attr_handle, bool cur_notify);

/* Push a status notification immediately (e.g. on a button press) instead of
 * waiting for the periodic tick. Safe from the NimBLE host task. */
void ble_cfg_svc_notify_status_now(void);

#ifdef __cplusplus
}
#endif
