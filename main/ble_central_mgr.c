#include "ble_central_mgr.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"

#include "ble_cadence_client.h"
#include "ble_hid_client.h"

static const char *TAG = "ble_cmgr";

#define SELECT_WINDOW_MS   6000
#define CONNECT_TIMEOUT_MS 4000

/* 50% scan duty (units of 0.625 ms): dense enough to catch the sensor's
 * ~100 ms advertising quickly, light enough to coexist with the peripheral
 * link + up to 4 central links. */
#define SCAN_ITVL   0x00A0   /* 100 ms */
#define SCAN_WINDOW 0x0050   /*  50 ms */

static bool    s_synced;
static uint8_t s_own_addr_type;

static volatile ble_central_select_t s_select = BLE_CENTRAL_SELECT_NONE;
static ble_central_select_cb_t       s_select_cb;
static esp_timer_handle_t            s_select_timer;

/* Selection dedup: addresses already reported in the current window. An
 * entry re-reports once when its name first becomes known (names usually
 * arrive in the scan response, a separate report from the ADV_IND). */
#define SEEN_MAX 24
static struct { ble_addr_t addr; bool named; } s_seen[SEEN_MAX];
static int s_seen_count;

static int mgr_gap_event(struct ble_gap_event *event, void *arg);

/* ---------- wanted-peer bookkeeping ---------- */

static int wanted_addrs(ble_addr_t *out, int max)
{
    int n = 0;
    if (n < max && ble_cadence_get_wanted_addr(&out[n])) n++;
    n += ble_hid_get_wanted_addrs(&out[n], max - n);
    return n;
}

/* Type-tolerant compare: the controller's resolving list may report a bonded
 * peer with an -ID address type (0x02/0x03) while the stored binding has the
 * plain type (0x00/0x01) — the LSB still tells public from random. */
static bool is_wanted(const ble_addr_t *addr)
{
    ble_addr_t wl[1 + BLE_HID_MAX_REMOTES];
    int n = wanted_addrs(wl, 1 + BLE_HID_MAX_REMOTES);
    for (int i = 0; i < n; i++) {
        if ((wl[i].type & 1) == (addr->type & 1) &&
            memcmp(wl[i].val, addr->val, 6) == 0) {
            return true;
        }
    }
    return false;
}

/* ---------- scanner ownership ---------- */

/* Start/stop the perpetual scan to match what is actually needed. The scan
 * runs while a selection window is open OR a bound peer is disconnected;
 * a pending connect owns the radio, so we wait for its CONNECT event. */
static void ensure_scan(void)
{
    if (!s_synced || ble_gap_conn_active()) return;

    ble_addr_t tmp[1 + BLE_HID_MAX_REMOTES];
    bool need = s_select != BLE_CENTRAL_SELECT_NONE ||
                wanted_addrs(tmp, 1 + BLE_HID_MAX_REMOTES) > 0;

    if (!need) {
        if (ble_gap_disc_active()) {
            ble_gap_disc_cancel();
            ESP_LOGI(TAG, "scan stopped (all bound peers connected)");
        }
        return;
    }
    if (ble_gap_disc_active()) return;

    struct ble_gap_disc_params dp = { 0 };
    dp.passive           = 0;   /* active: names live in scan responses */
    dp.filter_duplicates = 0;   /* re-advertising bound peers must re-report */
    dp.itvl              = SCAN_ITVL;
    dp.window            = SCAN_WINDOW;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp,
                          mgr_gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "scan running (select=%d)", (int)s_select);
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
    }
}

/* ---------- selection window ---------- */

static bool seen_pass(const ble_addr_t *addr, bool named)
{
    for (int i = 0; i < s_seen_count; i++) {
        if (s_seen[i].addr.type == addr->type &&
            memcmp(s_seen[i].addr.val, addr->val, 6) == 0) {
            if (named && !s_seen[i].named) {
                s_seen[i].named = true;   /* re-report once, now with name */
                return true;
            }
            return false;
        }
    }
    if (s_seen_count < SEEN_MAX) {
        s_seen[s_seen_count].addr  = *addr;
        s_seen[s_seen_count].named = named;
        s_seen_count++;
    }
    return true;   /* table overflow: report unseen extras, just undeduped */
}

static void handle_select_hit(const struct ble_gap_event *event)
{
    ble_central_select_t sel = s_select;
    if (sel == BLE_CENTRAL_SELECT_NONE || !s_select_cb) return;

    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, event->disc.data,
                                event->disc.length_data) != 0) {
        return;
    }
    bool match = (sel == BLE_CENTRAL_SELECT_CADENCE)
                     ? ble_cadence_adv_match(&f)
                     : ble_hid_adv_match(&f);
    if (!match) return;

    char name[32] = { 0 };
    if (f.name && f.name_len) {
        size_t n = f.name_len < sizeof(name) - 1 ? f.name_len
                                                 : sizeof(name) - 1;
        memcpy(name, f.name, n);
    }
    if (!seen_pass(&event->disc.addr, name[0] != 0)) return;

    ESP_LOGI(TAG, "select hit %02X:%02X:%02X:%02X:%02X:%02X \"%s\" rssi=%d",
             event->disc.addr.val[5], event->disc.addr.val[4],
             event->disc.addr.val[3], event->disc.addr.val[2],
             event->disc.addr.val[1], event->disc.addr.val[0],
             name, event->disc.rssi);
    s_select_cb(sel, event->disc.addr.val, event->disc.addr.type, name,
                event->disc.rssi);
}

static void select_timer_cb(void *arg)
{
    (void)arg;
    s_select = BLE_CENTRAL_SELECT_NONE;
    ESP_LOGI(TAG, "selection window closed");
    ensure_scan();
}

/* ---------- GAP events (scan + centrally-initiated connections) ---------- */

static int mgr_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        /* Reconnect beats selection: a bound peer that advertises gets its
         * link back immediately, the window keeps running meanwhile. */
        if (is_wanted(&event->disc.addr) && !ble_gap_conn_active()) {
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr,
                                     CONNECT_TIMEOUT_MS, NULL,
                                     mgr_gap_event, NULL);
            ESP_LOGI(TAG,
                     "bound peer %02X:%02X:%02X:%02X:%02X:%02X advertising — "
                     "connect rc=%d",
                     event->disc.addr.val[5], event->disc.addr.val[4],
                     event->disc.addr.val[3], event->disc.addr.val[2],
                     event->disc.addr.val[1], event->disc.addr.val[0], rc);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ensure_scan();
            }
            return 0;
        }
        handle_select_hit(event);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* Only happens on errors/host resets — the scan runs FOREVER and is
         * otherwise stopped via explicit cancels. */
        ESP_LOGD(TAG, "disc complete, reason=%d",
                 event->disc_complete.reason);
        ensure_scan();
        return 0;

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
            } else if (!ble_cadence_claim_conn(&desc) &&
                       !ble_hid_claim_conn(&desc)) {
                ESP_LOGW(TAG, "unclaimed central conn=%u — dropping",
                         (unsigned)event->connect.conn_handle);
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            /* Timeout (peer stopped advertising between report and connect)
             * or cancel — back to scanning. */
            ESP_LOGD(TAG, "central connect status=%d", event->connect.status);
        }
        ensure_scan();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ble_cadence_on_disconnect(event->disconnect.conn.conn_handle,
                                  event->disconnect.reason);
        ble_hid_on_disconnect(event->disconnect.conn.conn_handle,
                              event->disconnect.reason);
        ensure_scan();
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
}

/* ---------- public API ---------- */

void ble_central_mgr_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = select_timer_cb,
        .name     = "ble_select",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_select_timer));
}

void ble_central_mgr_on_sync(uint8_t own_addr_type)
{
    s_own_addr_type = own_addr_type;
    s_synced = true;
    ensure_scan();
}

void ble_central_mgr_kick(void)
{
    ensure_scan();
}

void ble_central_mgr_select_start(ble_central_select_t what)
{
    if (what == BLE_CENTRAL_SELECT_NONE) {
        ble_central_mgr_select_stop();
        return;
    }
    s_seen_count = 0;
    s_select = what;
    esp_timer_stop(s_select_timer);   /* no-op unless already running */
    ESP_ERROR_CHECK(esp_timer_start_once(s_select_timer,
                                         (uint64_t)SELECT_WINDOW_MS * 1000));
    ESP_LOGI(TAG, "selection scan: %s, %d ms",
             what == BLE_CENTRAL_SELECT_CADENCE ? "cadence" : "button",
             SELECT_WINDOW_MS);
    ensure_scan();
}

void ble_central_mgr_select_stop(void)
{
    esp_timer_stop(s_select_timer);
    if (s_select != BLE_CENTRAL_SELECT_NONE) {
        s_select = BLE_CENTRAL_SELECT_NONE;
        ESP_LOGI(TAG, "selection window closed (bind/stop)");
    }
    ensure_scan();
}

ble_central_select_t ble_central_mgr_selecting(void)
{
    return s_select;
}

void ble_central_mgr_set_select_cb(ble_central_select_cb_t cb)
{
    s_select_cb = cb;
}
