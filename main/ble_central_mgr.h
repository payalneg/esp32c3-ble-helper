#pragma once

/* Central-link connection manager.
 *
 * NimBLE allows exactly ONE outstanding ble_gap_connect at a time, but this
 * helper keeps two central links that both sleep aggressively (the cadence
 * sensor idles out after ~20 s, shutter buttons doze in seconds). Round-robin
 * connect attempts would add seconds of latency to a button wake-up, so
 * instead the manager arms a single whitelist-filtered connect covering every
 * bound-but-disconnected peer: whichever device wakes and advertises first
 * gets the link instantly, then the connect is re-armed for the rest.
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

void ble_central_mgr_init(void);

/* From ble_host's on_sync_cb: stack is up, own address type known. Arms the
 * first connect if anything is bound. */
void ble_central_mgr_on_sync(uint8_t own_addr_type);

/* Re-evaluate targets: cancels a pending connect if the target set changed
 * and re-arms. Call after bind/unbind/scan-complete/disconnect. */
void ble_central_mgr_kick(void);

/* Cancel the pending connect so a selection scan can use the scanner. The
 * scan-complete path must call ble_central_mgr_kick() to resume. */
void ble_central_mgr_pause(void);

/* Own address type resolved at sync — for modules issuing ble_gap_disc. */
uint8_t ble_central_mgr_own_addr_type(void);

#ifdef __cplusplus
}
#endif
