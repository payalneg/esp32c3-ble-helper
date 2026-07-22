#pragma once

/* Central-link connection manager.
 *
 * The helper keeps up to 1 + BLE_HID_MAX_REMOTES central links whose peers all
 * sleep aggressively (the cadence sensor idles out after ~20 s, shutter
 * buttons doze in seconds). Reconnect is scan-driven: while any bound peer is
 * disconnected the manager runs a perpetual active scan, and the moment a
 * bound address advertises it cancels the scan and issues a short directed
 * connect, then resumes scanning for the rest. This keeps the scanner free —
 * a perpetual ble_gap_connect would own the radio and make user-initiated
 * selection scans fail with EBUSY.
 *
 * Selection scans ("pick your sensor/remote in the GUI") are just a 6-second
 * window over the same scan: matching advertisements (filters live in
 * ble_cadence_client / ble_hid_client) are deduplicated and fanned to the
 * callback registered by ble_cfg_svc.
 *
 * All GAP events for centrally-initiated connections land here and are fanned
 * out to the owning module (ble_cadence_client / ble_hid_client) by address on
 * connect and by conn handle afterwards.
 *
 * Threading: every entry point just calls thread-safe ble_gap_* APIs; safe
 * from the NimBLE host task and app tasks alike. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values match the config protocol's WHAT_BUTTON / WHAT_CADENCE. */
typedef enum {
    BLE_CENTRAL_SELECT_NONE    = 0,
    BLE_CENTRAL_SELECT_BUTTON  = 1,
    BLE_CENTRAL_SELECT_CADENCE = 2,
} ble_central_select_t;

/* Selection-scan hit: invoked on the NimBLE host task, deduplicated per
 * window (an address repeats only when its name first becomes known). addr is
 * 6 bytes in NimBLE native (little-endian) order. */
typedef void (*ble_central_select_cb_t)(ble_central_select_t what,
                                        const uint8_t addr[6],
                                        uint8_t addr_type, const char *name,
                                        int8_t rssi);

void ble_central_mgr_init(void);

/* From ble_host's on_sync_cb: stack is up, own address type known. Starts the
 * reconnect scan if anything is bound. */
void ble_central_mgr_on_sync(uint8_t own_addr_type);

/* Re-evaluate: starts the scan when a bound peer is missing, stops it when
 * there is nothing to look for. Call after bind/unbind/forget/disconnect. */
void ble_central_mgr_kick(void);

/* Open / close the selection window (auto-closes after a few seconds). Hits
 * stream via the registered callback; background reconnect keeps working. */
void ble_central_mgr_select_start(ble_central_select_t what);
void ble_central_mgr_select_stop(void);
ble_central_select_t ble_central_mgr_selecting(void);
void ble_central_mgr_set_select_cb(ble_central_select_cb_t cb);

#ifdef __cplusplus
}
#endif
