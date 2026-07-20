#include "ble_cfg_svc.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

#include "ble_cadence_client.h"
#include "ble_hid_client.h"
#include "ble_ota.h"
#include "pas.h"
#include "settings.h"
#include "throttle_ctl.h"
#include "vesc_can/vesc_lisp_panel.h"

static const char *TAG = "ble_cfg";

/* ---- commands / responses ---- */
#define CMD_SCAN          0x01
#define CMD_BIND_BUTTON   0x02
#define CMD_BIND_CADENCE  0x03
#define CMD_UNBIND        0x04
#define CMD_GET_PARAMS    0x05
#define CMD_SET_PARAMS    0x06
#define CMD_SET_THROTTLE  0x07
#define CMD_SET_BINDING   0x08
#define CMD_GET_BINDING   0x09
#define RSP(cmd)          (0x80 | (cmd))

/* SET/GET_BINDING payload: [u8 idx][u8 ext][u8 len][u32 can_id LE][8B data] */
#define BINDING_LEN       (3 + 4 + 8)

#define WHAT_BUTTON       1
#define WHAT_CADENCE      2

#define PARAMS_VER        3
#define PARAMS_LEN        (25 + SETTINGS_BTN_SLOTS + 2)   /* + u16 can_kbps */
#define STATUS_VER        2
#define STATUS_LEN        12

/* ---- UUIDs: ab1e00XX-b1e5-4e15-8ac3-5e00c0de15b7, NimBLE LE order ---- */
#define CFG_UUID_TAIL_LE                                                \
    0xb7, 0x15, 0xde, 0xc0, 0x00, 0x5e, 0xc3, 0x8a,                     \
    0x15, 0x4e, 0xe5, 0xb1

static const ble_uuid128_t CFG_SVC_UUID = BLE_UUID128_INIT(
    CFG_UUID_TAIL_LE, 0x01, 0x00, 0x1e, 0xab);
static const ble_uuid128_t CFG_CTRL_UUID = BLE_UUID128_INIT(
    CFG_UUID_TAIL_LE, 0x02, 0x00, 0x1e, 0xab);
static const ble_uuid128_t CFG_STATUS_UUID = BLE_UUID128_INIT(
    CFG_UUID_TAIL_LE, 0x03, 0x00, 0x1e, 0xab);
static const ble_uuid128_t CFG_SCAN_UUID = BLE_UUID128_INIT(
    CFG_UUID_TAIL_LE, 0x04, 0x00, 0x1e, 0xab);
static const ble_uuid128_t CFG_OTA_CTRL_UUID = BLE_UUID128_INIT(
    CFG_UUID_TAIL_LE, 0x05, 0x00, 0x1e, 0xab);
static const ble_uuid128_t CFG_OTA_DATA_UUID = BLE_UUID128_INIT(
    CFG_UUID_TAIL_LE, 0x06, 0x00, 0x1e, 0xab);

/* ---- state ---- */

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;

static uint16_t s_ctrl_val_handle;
static uint16_t s_status_val_handle;
static uint16_t s_scan_val_handle;
static uint16_t s_ota_ctrl_val_handle;
static uint16_t s_ota_data_val_handle;

static volatile bool s_status_subscribed;
static volatile bool s_scan_subscribed;

/* ---- LE pack/unpack ---- */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint16_t get_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- notify helper (host task or cfg status task; NimBLE APIs lock) ---- */

static void notify(uint16_t val_handle, const uint8_t *data, uint16_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || val_handle == 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn, val_handle, om);
    if (rc != 0) ESP_LOGD(TAG, "notify handle=%u rc=%d", val_handle, rc);
}

static void ctrl_ack(uint8_t cmd, uint8_t status)
{
    uint8_t f[2] = { (uint8_t)RSP(cmd), status };
    notify(s_ctrl_val_handle, f, sizeof(f));
}

/* ---- params / status blobs ---- */

static void pack_params(uint8_t out[PARAMS_LEN])
{
    pas_settings_t s;
    pas_get_settings(&s);
    int i = 0;
    out[i++] = PARAMS_VER;
    out[i++] = s.enabled ? 1 : 0;
    out[i++] = s.reverse ? 1 : 0;
    out[i++] = s.level;
    out[i++] = s.level_count;
    out[i++] = s.mode;
    out[i++] = s.start_current_pct;
    put_u16(&out[i], s.start_delay_ms);    i += 2;
    put_u16(&out[i], s.stop_delay_ms);     i += 2;
    put_u16(&out[i], s.min_cadence_rpm);   i += 2;
    put_u16(&out[i], s.full_cadence_rpm);  i += 2;
    put_u32(&out[i], (uint32_t)(s.max_current_a * 1000.0f)); i += 4;
    put_u32(&out[i], (uint32_t)(s.ramp_up_aps * 1000.0f));   i += 4;
    out[i++] = settings_get_controller_id();
    out[i++] = settings_get_target_vesc_id();
    settings_get_button_actions(&out[i]);
    i += SETTINGS_BTN_SLOTS;
    put_u16(&out[i], settings_get_can_kbps());
    i += 2;
}

static bool apply_params(const uint8_t *p, uint16_t len)
{
    if (len < PARAMS_LEN || p[0] != PARAMS_VER) return false;
    pas_settings_t s;
    pas_get_settings(&s);
    int i = 1;
    s.enabled           = p[i++] != 0;
    s.reverse           = p[i++] != 0;
    s.level             = p[i++];
    s.level_count       = p[i++];
    s.mode              = p[i++];
    s.start_current_pct = p[i++];
    s.start_delay_ms    = get_u16(&p[i]); i += 2;
    s.stop_delay_ms     = get_u16(&p[i]); i += 2;
    s.min_cadence_rpm   = get_u16(&p[i]); i += 2;
    s.full_cadence_rpm  = get_u16(&p[i]); i += 2;
    s.max_current_a     = get_u32(&p[i]) / 1000.0f; i += 4;
    s.ramp_up_aps       = get_u32(&p[i]) / 1000.0f; i += 4;
    pas_set_settings(&s);
    settings_set_controller_id(p[i++]);
    settings_set_target_vesc_id(p[i++]);
    settings_set_button_actions(&p[i]);
    i += SETTINGS_BTN_SLOTS;
    settings_set_can_kbps(get_u16(&p[i]));
    i += 2;
    return true;
}

static void pack_status(uint8_t out[STATUS_LEN])
{
    pas_telem_t    t;
    pas_settings_t s;
    ble_hid_state_t b;
    pas_get_telem(&t);
    pas_get_settings(&s);
    ble_hid_get(&b);
    bool thr_valid = false;
    bool thr_on    = throttle_ctl_get(&thr_valid);

    uint8_t flags = 0;
    if (t.sensor_bound)            flags |= 1 << 0;
    if (t.sensor_connected)        flags |= 1 << 1;
    if (b.bound_count)             flags |= 1 << 2;
    if (b.connected_count)         flags |= 1 << 3;
    if (t.scanning || b.scanning)  flags |= 1 << 4;
    if (thr_on)                    flags |= 1 << 5;
    if (thr_valid)                 flags |= 1 << 6;
    if (s.enabled)                 flags |= 1 << 7;

    int i = 0;
    out[i++] = STATUS_VER;
    out[i++] = flags;
    put_u16(&out[i], (uint16_t)t.centi_rpm); i += 2;
    out[i++] = t.battery;
    out[i++] = s.level;
    put_u32(&out[i], (uint32_t)(int32_t)(t.assist_a * 1000.0f)); i += 4;
    out[i++] = b.pressed_mask;
    out[i++] = b.button_count;
}

void ble_cfg_svc_notify_status_now(void)
{
    if (!s_status_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t st[STATUS_LEN];
    pack_status(st);
    notify(s_status_val_handle, st, sizeof(st));
}

/* ---- scan-result fan-in (NimBLE host task) ---- */

static void send_scan_result(uint8_t what, const uint8_t addr[6],
                             uint8_t addr_type, const char *name, int8_t rssi)
{
    if (!s_scan_subscribed) return;
    uint8_t f[10 + 31];
    size_t  nlen = name ? strlen(name) : 0;
    if (nlen > 31) nlen = 31;
    int i = 0;
    f[i++] = what;
    f[i++] = addr_type;
    memcpy(&f[i], addr, 6); i += 6;
    f[i++] = (uint8_t)rssi;
    f[i++] = (uint8_t)nlen;
    memcpy(&f[i], name, nlen); i += nlen;
    notify(s_scan_val_handle, f, (uint16_t)i);
}

static void cadence_scan_cb(const uint8_t addr[6], uint8_t addr_type,
                            const char *name, int8_t rssi)
{
    send_scan_result(WHAT_CADENCE, addr, addr_type, name, rssi);
}

static void hid_scan_cb(const uint8_t addr[6], uint8_t addr_type,
                        const char *name, int8_t rssi)
{
    send_scan_result(WHAT_BUTTON, addr, addr_type, name, rssi);
}

/* ---- command dispatch (NimBLE host task) ---- */

static void handle_ctrl(const uint8_t *p, uint16_t len)
{
    if (len < 1) return;
    uint8_t cmd = p[0];
    switch (cmd) {
    case CMD_SCAN:
        if (len < 2) { ctrl_ack(cmd, 2); return; }
        if (p[1] == WHAT_BUTTON)       ble_hid_scan_start();
        else if (p[1] == WHAT_CADENCE) ble_cadence_scan_start();
        else { ctrl_ack(cmd, 2); return; }
        ctrl_ack(cmd, 0);
        break;

    case CMD_BIND_BUTTON:
        /* ADDS a remote (up to BLE_HID_MAX_REMOTES); status 3 = list full. */
        if (len < 8) { ctrl_ack(cmd, 2); return; }
        if (!ble_hid_bind(&p[2], p[1])) { ctrl_ack(cmd, 3); return; }
        settings_add_remote(&p[2], p[1]);
        ctrl_ack(cmd, 0);
        break;

    case CMD_BIND_CADENCE:
        if (len < 8) { ctrl_ack(cmd, 2); return; }
        pas_sensor_select(&p[2], p[1]);
        ctrl_ack(cmd, 0);
        break;

    case CMD_UNBIND:
        if (len < 2) { ctrl_ack(cmd, 2); return; }
        if (p[1] == WHAT_BUTTON) {
            settings_clear_remotes();
            ble_hid_forget();
        } else if (p[1] == WHAT_CADENCE) {
            pas_sensor_forget();
        } else { ctrl_ack(cmd, 2); return; }
        ctrl_ack(cmd, 0);
        break;

    case CMD_GET_PARAMS: {
        uint8_t f[1 + PARAMS_LEN];
        f[0] = (uint8_t)RSP(CMD_GET_PARAMS);
        pack_params(&f[1]);
        notify(s_ctrl_val_handle, f, sizeof(f));
        break;
    }

    case CMD_SET_PARAMS:
        ctrl_ack(cmd, apply_params(&p[1], len - 1) ? 0 : 2);
        break;

    case CMD_SET_THROTTLE:
        if (len < 2) { ctrl_ack(cmd, 2); return; }
        if (p[1] == 0xFF) throttle_ctl_toggle();
        else              throttle_ctl_set(p[1] != 0);
        ctrl_ack(cmd, 0);
        break;

    case CMD_SET_BINDING: {
        if (len < 1 + BINDING_LEN || p[1] >= SETTINGS_BTN_SLOTS) {
            ctrl_ack(cmd, 2);
            return;
        }
        btn_can_frame_t f = {
            .ext    = p[2],
            .len    = p[3] > 8 ? 8 : p[3],
            .can_id = get_u32(&p[4]),
        };
        memcpy(f.data, &p[8], 8);
        settings_set_button_frame(p[1], &f);
        ctrl_ack(cmd, 0);
        break;
    }

    case CMD_GET_BINDING: {
        if (len < 2 || p[1] >= SETTINGS_BTN_SLOTS) { ctrl_ack(cmd, 2); return; }
        btn_can_frame_t f;
        settings_get_button_frame(p[1], &f);
        uint8_t rsp[1 + 1 + BINDING_LEN];
        int i = 0;
        rsp[i++] = (uint8_t)RSP(CMD_GET_BINDING);
        rsp[i++] = p[1];
        rsp[i++] = f.ext;
        rsp[i++] = f.len;
        put_u32(&rsp[i], f.can_id); i += 4;
        memcpy(&rsp[i], f.data, 8); i += 8;
        notify(s_ctrl_val_handle, rsp, (uint16_t)i);
        break;
    }

    default:
        ESP_LOGW(TAG, "unknown cmd 0x%02X", cmd);
        ctrl_ack(cmd, 1);
        break;
    }
}

/* ---- GATT plumbing ---- */

static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == s_status_val_handle ||
            (s_status_val_handle == 0 &&
             ble_uuid_cmp(ctxt->chr->uuid, &CFG_STATUS_UUID.u) == 0)) {
            uint8_t st[STATUS_LEN];
            pack_status(st);
            return os_mbuf_append(ctxt->om, st, sizeof(st)) == 0
                       ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }

    uint8_t  buf[512];
    uint16_t out_len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (attr_handle == s_ctrl_val_handle) {
        handle_ctrl(buf, out_len);
    } else if (attr_handle == s_ota_ctrl_val_handle) {
        ble_ota_ctrl_write(buf, out_len);
    } else if (attr_handle == s_ota_data_val_handle) {
        ble_ota_data_write(buf, out_len);
    }
    return 0;
}

static const struct ble_gatt_svc_def s_cfg_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &CFG_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &CFG_CTRL_UUID.u,
                .access_cb  = chr_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE |
                              BLE_GATT_CHR_F_WRITE_NO_RSP |
                              BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ctrl_val_handle,
            },
            {
                .uuid       = &CFG_STATUS_UUID.u,
                .access_cb  = chr_access_cb,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_val_handle,
            },
            {
                .uuid       = &CFG_SCAN_UUID.u,
                .access_cb  = chr_access_cb,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_scan_val_handle,
            },
            {
                .uuid       = &CFG_OTA_CTRL_UUID.u,
                .access_cb  = chr_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ota_ctrl_val_handle,
            },
            {
                .uuid       = &CFG_OTA_DATA_UUID.u,
                .access_cb  = chr_access_cb,
                .flags      = BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_ota_data_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

const struct ble_gatt_svc_def *ble_cfg_svc_get_svcs(void)
{
    return s_cfg_svcs;
}

void ble_cfg_svc_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt,
                                   void *arg)
{
    (void)ctxt; (void)arg;
    /* val_handle pointers in the svc def capture everything we need. */
}

/* ---- status notify task (~2 Hz while subscribed) ---- */

static void status_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_status_subscribed && s_conn != BLE_HS_CONN_HANDLE_NONE) {
            uint8_t st[STATUS_LEN];
            pack_status(st);
            notify(s_status_val_handle, st, sizeof(st));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---- ble_host wiring ---- */

void ble_cfg_svc_on_connect(uint16_t conn_handle)
{
    s_conn = conn_handle;
    s_status_subscribed = false;
    s_scan_subscribed = false;
    /* Query-on-demand instead of polling: refresh the throttle snapshot once
     * so the GUI's status shows the real VESC state right away. */
    vesc_lisp_panel_query_state();
}

void ble_cfg_svc_on_disconnect(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_status_subscribed = false;
    s_scan_subscribed = false;
    ble_ota_on_disconnect();
}

void ble_cfg_svc_on_subscribe(uint16_t attr_handle, bool cur_notify)
{
    if (attr_handle == s_status_val_handle) {
        s_status_subscribed = cur_notify;
    } else if (attr_handle == s_scan_val_handle) {
        s_scan_subscribed = cur_notify;
    } else if (attr_handle == s_ota_ctrl_val_handle) {
        ble_ota_set_link(cur_notify ? s_conn : BLE_HS_CONN_HANDLE_NONE,
                         s_ota_ctrl_val_handle);
    }
}

void ble_cfg_svc_init(void)
{
    ble_cadence_set_scan_cb(cadence_scan_cb);
    ble_hid_set_scan_cb(hid_scan_cb);
    xTaskCreate(status_task, "cfg_status", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "config service ready");
}
