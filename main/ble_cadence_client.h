#pragma once

/* BLE central / GATT-client for the external cadence sensor (pedal-assist).
 *
 * The helper is a BLE peripheral (NUS bridge + config service) AND a central:
 * it binds to one specific BLE cadence sensor (e.g. "BK6LS…", custom service
 * cad00001-… with a notify characteristic cad00002-… streaming a signed int16
 * centi-RPM every 100 ms) and keeps reconnecting to it whenever it wakes.
 *
 * Connection lifecycle and scanning are owned by ble_central_mgr; this module
 * exposes its bound address via ble_cadence_get_wanted_addr and its
 * advertisement filter via ble_cadence_adv_match, claims the link on connect,
 * runs the GATT discovery/subscribe chain and consumes RPM notifications. */

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Live sensor state snapshot (safe from any task; backed by a spinlock). */
typedef struct {
    bool     bound;      /* a sensor address has been bound (cadence_bind) */
    bool     connected;  /* GATT link up AND subscribed to RPM notifications */
    bool     scanning;   /* a selection scan is currently running */
    int16_t  centi_rpm;  /* signed centi-RPM (RPM*100); sign = direction */
    uint32_t age_ms;     /* ms since the last RPM notification (UINT32_MAX = never) */
    uint8_t  battery;    /* 0..100 percent, 0xFF = unknown */
} ble_cadence_state_t;

/* Set up module state. Call once from ble_host_init() before the host task
 * starts. */
void ble_cadence_client_init(void);

/* Bind to a specific sensor and (re)connect to it. Does NOT persist — the
 * caller (PAS settings) owns NVS. addr is 6 bytes native order. */
void ble_cadence_bind(const uint8_t addr[6], uint8_t addr_type);

/* Forget the bound sensor: disconnect and stop reconnecting. */
void ble_cadence_forget(void);

/* True if a sensor address is bound. */
bool ble_cadence_is_bound(void);

/* Copy the current sensor state. */
void ble_cadence_get(ble_cadence_state_t *out);

/* ---- ble_central_mgr hooks (NimBLE host task) ---- */
bool ble_cadence_adv_match(const struct ble_hs_adv_fields *f);
bool ble_cadence_get_wanted_addr(ble_addr_t *out);
bool ble_cadence_claim_conn(const struct ble_gap_conn_desc *desc);
void ble_cadence_on_disconnect(uint16_t conn_handle, int reason);
void ble_cadence_on_notify(struct ble_gap_event *event);

#ifdef __cplusplus
}
#endif
