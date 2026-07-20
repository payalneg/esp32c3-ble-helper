#pragma once

/* BLE central / HID-over-GATT (HOGP) host for shutter buttons and other HID
 * remotes. Up to BLE_HID_MAX_REMOTES devices can be bound and connected at
 * the same time (each keychain remote sleeps aggressively; the shared
 * whitelist connect in ble_central_mgr picks whichever wakes up).
 *
 * Remotes expose HID service 0x1812 with one or more Report characteristics;
 * some give each physical button its own report, others pack both buttons
 * into ONE report as different bytes/bits. Buttons are therefore identified
 * by their press SIGNATURE — (device, report, first non-zero byte offset,
 * value) — learned on the first press and persisted (settings.c), so indices
 * stay stable across reboots: the first button you ever press becomes A, the
 * next B, and so on, across ALL bound remotes. A press edge fires the
 * callback with that index; the app sends the button's configured CAN frame.
 *
 * HID notifications require encryption, so links are bonded on first connect
 * (Just Works; bonds persist via CONFIG_BT_NIMBLE_NVS_PERSIST). */

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_gap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_HID_MAX_REMOTES 3

typedef struct {
    uint8_t bound_count;      /* bound remotes */
    uint8_t connected_count;  /* remotes currently connected + subscribed */
    bool    scanning;
    uint8_t button_count;     /* LEARNED buttons (press signatures seen) */
    uint8_t pressed_mask;     /* bit N = button N currently pressed */
} ble_hid_state_t;

/* Press-edge callback with the global button index (A=0, B=1, …). Runs on
 * the NimBLE host task — keep it short (queue work, don't block). */
typedef void (*ble_hid_press_cb_t)(uint8_t btn_idx);

/* Scan-result callback, same contract as the cadence one. */
typedef void (*ble_hid_scan_cb_t)(const uint8_t addr[6], uint8_t addr_type,
                                  const char *name, int8_t rssi);

void ble_hid_client_init(void);
void ble_hid_set_press_cb(ble_hid_press_cb_t cb);

void ble_hid_scan_start(void);
void ble_hid_scan_stop(void);
void ble_hid_set_scan_cb(ble_hid_scan_cb_t cb);

/* Bind ADDS a remote (up to BLE_HID_MAX_REMOTES; re-binding a known address
 * is a no-op). forget disconnects and clears ALL remotes, their bonds and
 * the learned button signatures. No NVS here — settings.c owns persistence
 * through the caller (ble_cfg_svc / app_main). */
bool ble_hid_bind(const uint8_t addr[6], uint8_t addr_type);
void ble_hid_forget(void);

void ble_hid_get(ble_hid_state_t *out);

/* ---- ble_central_mgr hooks (NimBLE host task) ---- */

/* Append the addresses of bound-but-disconnected remotes to out (up to max).
 * Returns how many were written. */
int  ble_hid_get_wanted_addrs(ble_addr_t *out, int max);
bool ble_hid_claim_conn(const struct ble_gap_conn_desc *desc);
void ble_hid_on_disconnect(uint16_t conn_handle, int reason);
void ble_hid_on_notify(struct ble_gap_event *event);
void ble_hid_on_enc_change(uint16_t conn_handle, int status);

#ifdef __cplusplus
}
#endif
