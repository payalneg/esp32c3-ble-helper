#include "ble_hid_client.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "ble_central_mgr.h"
#include "settings.h"

static const char *TAG = "ble_hid";

#define HID_SVC_UUID16     0x1812
#define HID_REPORT_UUID16  0x2A4D
#define CCCD_UUID16        0x2902

static const ble_uuid_t *HID_SVC_UUID = BLE_UUID16_DECLARE(HID_SVC_UUID16);
static const ble_uuid_t *CCCD_UUID    = BLE_UUID16_DECLARE(CCCD_UUID16);

/* Ignore press edges closer than this — debounce + double-report protection
 * (many remotes fire several reports per physical click). */
#define PRESS_COOLDOWN_MS  300

#define MAX_CHARS   16   /* characteristics inside one HID service */
#define MAX_REPORTS 8    /* input reports per remote */

/* ---- per-remote state ---- */

typedef struct { uint16_t val_handle; bool is_report; } hid_chr_t;
typedef struct {
    uint16_t val_handle;
    uint16_t cccd_handle;
    uint32_t active_key;  /* signature currently held down on this report */
} hid_report_t;

typedef struct {
    bool         bound;
    ble_addr_t   addr;
    uint16_t     conn;          /* BLE_HS_CONN_HANDLE_NONE while down */
    bool         disc_started;
    bool         subscribed;    /* ≥1 input report subscribed */
    uint16_t     svc_start, svc_end;
    hid_chr_t    chrs[MAX_CHARS];
    int          chr_count;
    hid_report_t reports[MAX_REPORTS];
    int          report_count;
    int          sub_idx;       /* CCCD-write chain cursor */
} hid_dev_t;

static hid_dev_t s_dev[BLE_HID_MAX_REMOTES];

/* ---- module state ---- */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool s_inited;

/* Buttons are identified by a press SIGNATURE shared across all remotes:
 * (device << 24) | (report << 16) | (first non-zero byte offset << 8) |
 * value. Learned on first press, persisted so indices stay stable across
 * reboots, cleared together with the bindings. */
static uint32_t s_sigs[SETTINGS_BTN_SIGS];
static uint8_t  s_sig_count;

static uint8_t  s_pressed_mask;                  /* bit N = button N down */
static int64_t  s_last_press_us[SETTINGS_BTN_SIGS];

static ble_hid_press_cb_t s_press_cb;

/* ---------- helpers ---------- */

static bool addr_equal(const ble_addr_t *a, const ble_addr_t *b)
{
    return a->type == b->type && memcmp(a->val, b->val, 6) == 0;
}

static hid_dev_t *dev_by_conn(uint16_t conn)
{
    for (int i = 0; i < BLE_HID_MAX_REMOTES; i++) {
        if (s_dev[i].bound && s_dev[i].conn == conn &&
            conn != BLE_HS_CONN_HANDLE_NONE) {
            return &s_dev[i];
        }
    }
    return NULL;
}

static void dev_reset_link(hid_dev_t *d)
{
    uint8_t clear_mask = 0;
    for (int r = 0; r < d->report_count; r++) {
        if (d->reports[r].active_key) {
            for (int i = 0; i < s_sig_count; i++) {
                if (s_sigs[i] == d->reports[r].active_key) {
                    clear_mask |= 1u << i;
                }
            }
        }
    }
    d->conn = BLE_HS_CONN_HANDLE_NONE;
    d->disc_started = false;
    d->subscribed = false;
    d->svc_start = d->svc_end = 0;
    d->chr_count = 0;
    d->report_count = 0;
    d->sub_idx = 0;
    memset(d->reports, 0, sizeof(d->reports));
    if (clear_mask) {
        portENTER_CRITICAL(&s_mux);
        s_pressed_mask &= ~clear_mask;
        portEXIT_CRITICAL(&s_mux);
    }
}

/* Find the button index for a signature; learn (and persist) a new one when
 * there is room. Returns -1 when the table is full. Host task only. */
static int sig_find(uint32_t key)
{
    for (int i = 0; i < s_sig_count; i++) {
        if (s_sigs[i] == key) return i;
    }
    return -1;
}

static int sig_find_or_learn(uint32_t key)
{
    int idx = sig_find(key);
    if (idx >= 0) return idx;
    if (s_sig_count >= SETTINGS_BTN_SIGS) return -1;
    idx = s_sig_count;
    s_sigs[idx] = key;
    s_sig_count++;
    settings_save_button_sigs(s_sigs, s_sig_count);
    ESP_LOGI(TAG, "learned button %c: dev=%u report=%u byte=%u val=0x%02X",
             'A' + idx, (unsigned)(key >> 24) & 0xFF,
             (unsigned)(key >> 16) & 0xFF,
             (unsigned)(key >> 8) & 0xFF, (unsigned)key & 0xFF);
    return idx;
}

/* ---------- CCCD subscribe chain (per device) ---------- */

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg);

static void subscribe_next(hid_dev_t *d)
{
    while (d->sub_idx < d->report_count) {
        int i = d->sub_idx++;
        static const uint8_t en[2] = { 0x01, 0x00 };
        int rc = ble_gattc_write_flat(d->conn, d->reports[i].cccd_handle,
                                      en, sizeof en, on_cccd_written, d);
        if (rc == 0) return;        /* continue from the write callback */
        ESP_LOGW(TAG, "CCCD write rc=%d (handle %u)", rc,
                 (unsigned)d->reports[i].cccd_handle);
    }
    if (d->report_count > 0) {
        d->subscribed = true;
        ESP_LOGI(TAG, "remote ready: %d input report(s) subscribed (conn=%u)",
                 d->report_count, (unsigned)d->conn);
    } else {
        ESP_LOGW(TAG, "no subscribable reports found on remote");
    }
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr;
    if (err->status != 0) {
        ESP_LOGW(TAG, "CCCD write failed status=%d", err->status);
    }
    subscribe_next((hid_dev_t *)arg);
    return 0;
}

/* ---------- descriptor sweep: find CCCDs owned by Report chars ---------- */

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    (void)conn; (void)chr_val_handle;
    hid_dev_t *d = (hid_dev_t *)arg;
    if (err->status == 0 && dsc &&
        ble_uuid_cmp(&dsc->uuid.u, CCCD_UUID) == 0) {
        /* Attribute the CCCD to the nearest preceding characteristic. */
        const hid_chr_t *owner = NULL;
        for (int i = 0; i < d->chr_count; i++) {
            if (d->chrs[i].val_handle < dsc->handle &&
                (!owner || d->chrs[i].val_handle > owner->val_handle)) {
                owner = &d->chrs[i];
            }
        }
        if (owner && owner->is_report && d->report_count < MAX_REPORTS) {
            d->reports[d->report_count].val_handle  = owner->val_handle;
            d->reports[d->report_count].cccd_handle = dsc->handle;
            d->reports[d->report_count].active_key  = 0;
            d->report_count++;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "descriptor sweep done: %d report CCCD(s)",
                 d->report_count);
        d->sub_idx = 0;
        subscribe_next(d);
    }
    return 0;
}

/* ---------- characteristic discovery ---------- */

static int on_chr(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    hid_dev_t *d = (hid_dev_t *)arg;
    if (err->status == 0 && chr) {
        if (d->chr_count < MAX_CHARS) {
            d->chrs[d->chr_count].val_handle = chr->val_handle;
            d->chrs[d->chr_count].is_report =
                ble_uuid_cmp(&chr->uuid.u,
                             BLE_UUID16_DECLARE(HID_REPORT_UUID16)) == 0 &&
                (chr->properties & BLE_GATT_CHR_PROP_NOTIFY);
            d->chr_count++;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        int reports = 0;
        for (int i = 0; i < d->chr_count; i++) reports += d->chrs[i].is_report;
        ESP_LOGI(TAG, "HID svc: %d chars, %d notifiable report(s)",
                 d->chr_count, reports);
        if (reports == 0) {
            ESP_LOGW(TAG, "remote exposes no notifiable Report chars");
            return 0;
        }
        uint16_t first_val = 0xFFFF;
        for (int i = 0; i < d->chr_count; i++) {
            if (d->chrs[i].val_handle < first_val) {
                first_val = d->chrs[i].val_handle;
            }
        }
        int rc = ble_gattc_disc_all_dscs(conn, first_val, d->svc_end,
                                         on_dsc, d);
        if (rc != 0) ESP_LOGW(TAG, "disc_all_dscs rc=%d", rc);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_svc *svc, void *arg)
{
    hid_dev_t *d = (hid_dev_t *)arg;
    if (err->status == 0 && svc) {
        d->svc_start = svc->start_handle;
        d->svc_end   = svc->end_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (d->svc_start == 0) {
            ESP_LOGW(TAG, "HID service not found on peer");
            return 0;
        }
        int rc = ble_gattc_disc_all_chrs(conn, d->svc_start, d->svc_end,
                                         on_chr, d);
        if (rc != 0) ESP_LOGW(TAG, "disc_all_chrs rc=%d", rc);
    }
    return 0;
}

static void start_discovery(hid_dev_t *d)
{
    if (d->disc_started) return;
    d->disc_started = true;
    d->svc_start = d->svc_end = 0;
    d->chr_count = 0;
    d->report_count = 0;
    ESP_LOGI(TAG, "discovering HID service on conn=%u", (unsigned)d->conn);
    int rc = ble_gattc_disc_svc_by_uuid(d->conn, HID_SVC_UUID, on_svc, d);
    if (rc != 0) ESP_LOGW(TAG, "disc_svc rc=%d", rc);
}

/* ---------- ble_central_mgr hooks ---------- */

bool ble_hid_adv_match(const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        if (ble_uuid_u16(&f->uuids16[i].u) == HID_SVC_UUID16) return true;
    }
    /* Appearance category 15 = Human Interface Device (0x03C0..0x03FF) —
     * some remotes advertise only that. */
    if (f->appearance_is_present && (f->appearance >> 6) == 0x00F) return true;
    /* HID markers are the fast path, but plenty of keyboards/remotes
     * advertise neither the 0x1812 UUID nor an appearance (those often
     * live only in the scan response, or nowhere) — so anything with a
     * readable name is shown too. Binding is an explicit user pick, a
     * longer list is harmless; nameless AND markerless beacons are
     * still dropped to keep the noise down. */
    return f->name && f->name_len;
}

int ble_hid_get_wanted_addrs(ble_addr_t *out, int max)
{
    if (!s_inited) return 0;
    int n = 0;
    for (int i = 0; i < BLE_HID_MAX_REMOTES && n < max; i++) {
        if (s_dev[i].bound && s_dev[i].conn == BLE_HS_CONN_HANDLE_NONE) {
            out[n++] = s_dev[i].addr;
        }
    }
    return n;
}

bool ble_hid_claim_conn(const struct ble_gap_conn_desc *desc)
{
    hid_dev_t *d = NULL;
    for (int i = 0; i < BLE_HID_MAX_REMOTES; i++) {
        if (s_dev[i].bound && s_dev[i].conn == BLE_HS_CONN_HANDLE_NONE &&
            (addr_equal(&desc->peer_id_addr, &s_dev[i].addr) ||
             addr_equal(&desc->peer_ota_addr, &s_dev[i].addr))) {
            d = &s_dev[i];
            break;
        }
    }
    if (!d) return false;

    d->conn = desc->conn_handle;
    d->disc_started = false;
    ESP_LOGI(TAG, "remote %d connected, conn=%u", (int)(d - s_dev),
             (unsigned)d->conn);
    ble_gattc_exchange_mtu(d->conn, NULL, NULL);
    /* HID notifications require an encrypted link; bond on first connect
     * (Just Works). ENC_CHANGE starts discovery. If the stack can't even
     * begin pairing (peer without SM), fall through to plain discovery —
     * some no-name remotes ship without security at all. */
    int rc = ble_gap_security_initiate(d->conn);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "security_initiate rc=%d — trying without encryption", rc);
        start_discovery(d);
    }
    return true;
}

void ble_hid_on_enc_change(uint16_t conn_handle, int status)
{
    hid_dev_t *d = dev_by_conn(conn_handle);
    if (!d) return;
    ESP_LOGI(TAG, "encryption %s (status=%d)",
             status == 0 ? "established" : "FAILED", status);
    /* Proceed either way — an unencrypted link still works on remotes that
     * don't enforce security; the CCCD write will tell us if it doesn't. */
    start_discovery(d);
}

void ble_hid_on_disconnect(uint16_t conn_handle, int reason)
{
    hid_dev_t *d = dev_by_conn(conn_handle);
    if (!d) return;
    ESP_LOGI(TAG, "remote %d disconnect, reason=%d", (int)(d - s_dev), reason);
    dev_reset_link(d);
    /* mgr re-arms; the remote reconnects on its next wake-up/press. */
}

void ble_hid_on_notify(struct ble_gap_event *event)
{
    hid_dev_t *d = dev_by_conn(event->notify_rx.conn_handle);
    if (!d) return;

    int rep = -1;
    for (int i = 0; i < d->report_count; i++) {
        if (d->reports[i].val_handle == event->notify_rx.attr_handle) {
            rep = i;
            break;
        }
    }
    if (rep < 0) return;

    uint8_t buf[16] = {0};
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len > sizeof(buf)) len = sizeof(buf);
    if (os_mbuf_copydata(event->notify_rx.om, 0, len, buf) != 0) return;

    /* Bring-up visibility: what the remote actually sends. */
    ESP_LOGI(TAG, "dev %d rep %d len %u data %02X %02X %02X %02X",
             (int)(d - s_dev), rep, (unsigned)len, buf[0],
             len > 1 ? buf[1] : 0, len > 2 ? buf[2] : 0,
             len > 3 ? buf[3] : 0);

    /* Press signature: first non-zero byte's offset + value. 0 = released. */
    uint32_t key = 0;
    for (uint16_t i = 0; i < len; i++) {
        if (buf[i] != 0) {
            key = ((uint32_t)(d - s_dev) << 24) | ((uint32_t)rep << 16) |
                  ((uint32_t)i << 8) | buf[i];
            break;
        }
    }

    if (key == d->reports[rep].active_key) return;   /* no state change */

    int released = d->reports[rep].active_key
                       ? sig_find(d->reports[rep].active_key) : -1;
    int btn = key ? sig_find_or_learn(key) : -1;    /* NVS write on learn —
                                                     * outside the spinlock */
    d->reports[rep].active_key = key;

    bool fire = false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (released >= 0) s_pressed_mask &= ~(1u << released);
    if (btn >= 0) {
        if (!(s_pressed_mask & (1u << btn)) &&
            now - s_last_press_us[btn] > (int64_t)PRESS_COOLDOWN_MS * 1000) {
            s_last_press_us[btn] = now;
            fire = true;
        }
        s_pressed_mask |= 1u << btn;
    }
    portEXIT_CRITICAL(&s_mux);

    if (fire) {
        ESP_LOGI(TAG, "button %c PRESS", 'A' + btn);
        if (s_press_cb) s_press_cb((uint8_t)btn);
    }
}

/* ---------- public API ---------- */

void ble_hid_client_init(void)
{
    s_inited = true;
    for (int i = 0; i < BLE_HID_MAX_REMOTES; i++) {
        s_dev[i].conn = BLE_HS_CONN_HANDLE_NONE;
    }
    s_sig_count = settings_load_button_sigs(s_sigs);
    if (s_sig_count) {
        ESP_LOGI(TAG, "restored %u learned button(s)", s_sig_count);
    }
}

void ble_hid_set_press_cb(ble_hid_press_cb_t cb) { s_press_cb = cb; }

bool ble_hid_bind(const uint8_t addr[6], uint8_t addr_type)
{
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);

    hid_dev_t *free_slot = NULL;
    for (int i = 0; i < BLE_HID_MAX_REMOTES; i++) {
        if (s_dev[i].bound && addr_equal(&s_dev[i].addr, &a)) {
            return true;                       /* already bound */
        }
        if (!s_dev[i].bound && !free_slot) free_slot = &s_dev[i];
    }
    if (!free_slot) {
        ESP_LOGW(TAG, "all %d remote slots in use", BLE_HID_MAX_REMOTES);
        return false;
    }

    free_slot->bound = true;
    free_slot->addr = a;
    free_slot->conn = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "bound remote %d: %02X:%02X:%02X:%02X:%02X:%02X (type %u)",
             (int)(free_slot - s_dev),
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
             (unsigned)addr_type);
    ble_central_mgr_select_stop();   /* choice made — close the window */
    ble_central_mgr_kick();
    return true;
}

void ble_hid_forget(void)
{
    ESP_LOGI(TAG, "forget all remotes");
    for (int i = 0; i < BLE_HID_MAX_REMOTES; i++) {
        hid_dev_t *d = &s_dev[i];
        if (!d->bound) continue;
        /* Drop the bond too so a re-pair starts clean. Best-effort. */
        ble_store_util_delete_peer(&d->addr);
        if (d->conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(d->conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        dev_reset_link(d);
        d->bound = false;
    }
    /* Different remotes will have different signatures — relearn. */
    s_sig_count = 0;
    settings_erase_button_sigs();
    portENTER_CRITICAL(&s_mux);
    s_pressed_mask = 0;
    portEXIT_CRITICAL(&s_mux);
    ble_central_mgr_kick();
}

void ble_hid_get(ble_hid_state_t *out)
{
    if (!out) return;
    uint8_t bound = 0, connected = 0;
    for (int i = 0; i < BLE_HID_MAX_REMOTES; i++) {
        if (s_dev[i].bound) bound++;
        if (s_dev[i].bound && s_dev[i].subscribed &&
            s_dev[i].conn != BLE_HS_CONN_HANDLE_NONE) {
            connected++;
        }
    }
    out->scanning = ble_central_mgr_selecting() == BLE_CENTRAL_SELECT_BUTTON;
    portENTER_CRITICAL(&s_mux);
    out->bound_count     = bound;
    out->connected_count = connected;
    out->button_count    = s_sig_count;
    out->pressed_mask    = s_pressed_mask;
    portEXIT_CRITICAL(&s_mux);
}
