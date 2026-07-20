#include "ble_central_mgr.h"

#include <string.h>

#include "esp_log.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"

#include "ble_cadence_client.h"
#include "ble_hid_client.h"

static const char *TAG = "ble_cmgr";

static bool    s_synced;
static uint8_t s_own_addr_type;

static int mgr_gap_event(struct ble_gap_event *event, void *arg);

/* Arm one connect covering every bound-but-disconnected peer. No-op while a
 * scan owns the scanner (the scan-complete path kicks us) or while a connect
 * attempt is already outstanding. */
static void arm(void)
{
    if (!s_synced) return;
    if (ble_gap_disc_active() || ble_gap_conn_active()) return;

    ble_addr_t wl[1 + BLE_HID_MAX_REMOTES];
    int n = 0;
    if (ble_cadence_get_wanted_addr(&wl[n])) n++;
    n += ble_hid_get_wanted_addrs(&wl[n], BLE_HID_MAX_REMOTES);
    if (n == 0) return;

    int rc;
    if (n == 1) {
        /* Single target — plain directed connect, no whitelist churn. */
        rc = ble_gap_connect(s_own_addr_type, &wl[0], BLE_HS_FOREVER, NULL,
                             mgr_gap_event, NULL);
    } else {
        rc = ble_gap_wl_set(wl, n);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gap_wl_set rc=%d", rc);
            return;
        }
        /* NULL peer → initiate with the controller's white list. */
        rc = ble_gap_connect(s_own_addr_type, NULL, BLE_HS_FOREVER, NULL,
                             mgr_gap_event, NULL);
    }
    if (rc == 0) {
        ESP_LOGI(TAG, "connect armed for %d peer(s)", n);
    } else if (rc != BLE_HS_EALREADY && rc != BLE_HS_EBUSY) {
        ESP_LOGW(TAG, "ble_gap_connect rc=%d", rc);
    }
}

static int mgr_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
#if defined(BLE_GAP_EVENT_LINK_ESTAB)
    case BLE_GAP_EVENT_LINK_ESTAB:
#else
    case BLE_GAP_EVENT_CONNECT:
#endif
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0) {
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            if (!ble_cadence_claim_conn(&desc) && !ble_hid_claim_conn(&desc)) {
                ESP_LOGW(TAG, "unclaimed central conn=%u — dropping",
                         (unsigned)event->connect.conn_handle);
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            /* Failure, timeout or an explicit cancel (pause/kick) — either
             * way the slot is free again. */
            ESP_LOGD(TAG, "central connect status=%d", event->connect.status);
        }
        arm();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ble_cadence_on_disconnect(event->disconnect.conn.conn_handle,
                                  event->disconnect.reason);
        ble_hid_on_disconnect(event->disconnect.conn.conn_handle,
                              event->disconnect.reason);
        arm();
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        ble_cadence_on_notify(event);
        ble_hid_on_notify(event);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ble_hid_on_enc_change(event->enc_change.conn_handle,
                              event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Peer lost its bond (button was factory-reset) — drop ours and
         * pair again instead of failing forever. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
    return 0;
}

void ble_central_mgr_init(void)
{
    /* Nothing yet — state arms on sync. */
}

void ble_central_mgr_on_sync(uint8_t own_addr_type)
{
    s_own_addr_type = own_addr_type;
    s_synced = true;
    arm();
}

void ble_central_mgr_kick(void)
{
    if (!s_synced) return;
    if (ble_gap_conn_active()) {
        /* Cancel → CONNECT event with a failure status → arm() re-runs with
         * the fresh target set. */
        ble_gap_conn_cancel();
    } else {
        arm();
    }
}

void ble_central_mgr_pause(void)
{
    if (ble_gap_conn_active()) {
        ble_gap_conn_cancel();
    }
}

uint8_t ble_central_mgr_own_addr_type(void)
{
    return s_own_addr_type;
}
