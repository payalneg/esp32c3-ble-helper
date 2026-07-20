/* BLE OTA for the ESP32-C3 helper — see ble_ota.h for the wire protocol.
 *
 * The P4 display stages the whole image in PSRAM and flushes it on END; the
 * C3 has no PSRAM and ~300 KB of usable RAM against a ~1.5 MB image, so the
 * image streams straight to flash instead: the NimBLE host task pushes each
 * DATA write into a ring buffer and a worker task drains it into
 * esp_ota_write, hashing as it goes. esp_ota_begin erases the spare slot
 * up-front (a few seconds) — READY is only notified after that, and the host
 * must not send data before READY. */

#include "ble_ota.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"

static const char *TAG = "ble_ota";

/* ---- wire protocol constants ---- */
#define OTA_OP_BEGIN     0x01
#define OTA_OP_END       0x02
#define OTA_OP_ABORT     0x03

#define OTA_ST_READY     0x10
#define OTA_ST_PROGRESS  0x11
#define OTA_ST_DONE      0x12
#define OTA_ST_ERROR     0x1F

/* ERROR detail codes */
#define OTA_ERR_NO_PART  1   /* no spare OTA partition */
#define OTA_ERR_SIZE     2   /* image is zero / larger than the slot */
#define OTA_ERR_ALLOC    3   /* ring buffer overflow (host outran flash) */
#define OTA_ERR_SHA      4   /* SHA-256 of received bytes != BEGIN digest */
#define OTA_ERR_BEGIN    5   /* esp_ota_begin (erase) failed */
#define OTA_ERR_WRITE    6   /* esp_ota_write failed */
#define OTA_ERR_END      7   /* esp_ota_end / image verify failed */
#define OTA_ERR_BOOT     8   /* esp_ota_set_boot_partition failed */
#define OTA_ERR_PROTO    9   /* protocol / sequence error */

#define OTA_BEGIN_LEN     (1 + 4 + 32)   /* op + u32 total_len + sha256 */
#define OTA_PROGRESS_STEP (128 * 1024)   /* liveness cadence */

/* 16 KiB of in-flight data ≈ 30 MTU-sized chunks — enough slack for flash
 * program latency at BLE throughput. */
#define OTA_RB_SIZE       (16 * 1024)
#define OTA_DRAIN_TICK_MS 20

typedef enum { ST_IDLE, ST_RECEIVING, ST_FAILED } ota_state_t;

typedef enum { EV_BEGIN, EV_FINALIZE, EV_FAIL, EV_ABORT } ev_kind_t;
typedef struct {
    ev_kind_t kind;
    uint32_t  total_len;   /* EV_BEGIN */
    uint32_t  detail;      /* EV_FAIL: err code */
    uint8_t   sha[32];     /* EV_BEGIN */
} ota_evt_t;

static QueueHandle_t   s_q;
static RingbufHandle_t s_rb;
static TaskHandle_t    s_task;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_ctrl_handle;

static volatile ota_state_t s_state = ST_IDLE;

/* Worker-owned transfer state (host task only reads s_state). */
static esp_ota_handle_t s_ota;
static bool             s_ota_open;
static uint32_t         s_total;
static uint32_t         s_recv;
static uint32_t         s_next_progress;
static uint8_t          s_expect_sha[32];
static mbedtls_sha256_context s_sha;
static bool             s_sha_active;

/* ---- helpers ---- */

static void notify_status(uint8_t status, uint32_t detail)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_ctrl_handle == 0) return;
    uint8_t f[5] = {
        status,
        (uint8_t)detail, (uint8_t)(detail >> 8),
        (uint8_t)(detail >> 16), (uint8_t)(detail >> 24),
    };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(f, sizeof(f));
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn, s_ctrl_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify status=0x%02x rc=%d", status, rc);
}

static void rb_drain_discard(void)
{
    while (1) {
        size_t sz = 0;
        void *p = xRingbufferReceive(s_rb, &sz, 0);
        if (!p) break;
        vRingbufferReturnItem(s_rb, p);
    }
}

static void transfer_reset(void)
{
    if (s_ota_open) { esp_ota_abort(s_ota); s_ota_open = false; }
    if (s_sha_active) { mbedtls_sha256_free(&s_sha); s_sha_active = false; }
    rb_drain_discard();
    s_total = s_recv = s_next_progress = 0;
    s_state = ST_IDLE;
}

/* ---- worker-task handlers ---- */

static void do_begin(const ota_evt_t *ev)
{
    transfer_reset();

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        ESP_LOGE(TAG, "no next OTA partition");
        notify_status(OTA_ST_ERROR, OTA_ERR_NO_PART);
        return;
    }
    if (ev->total_len == 0 || ev->total_len > next->size) {
        ESP_LOGE(TAG, "bad size %u (slot %u)",
                 (unsigned)ev->total_len, (unsigned)next->size);
        notify_status(OTA_ST_ERROR, OTA_ERR_SIZE);
        return;
    }

    /* Erases the whole target range — takes a few seconds on a full slot.
     * The host waits for READY before streaming, so nothing is lost. */
    esp_err_t err = esp_ota_begin(next, ev->total_len, &s_ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        notify_status(OTA_ST_ERROR, OTA_ERR_BEGIN);
        return;
    }
    s_ota_open = true;

    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    s_sha_active = true;
    memcpy(s_expect_sha, ev->sha, 32);
    s_total = ev->total_len;
    s_recv = 0;
    s_next_progress = OTA_PROGRESS_STEP;

    s_state = ST_RECEIVING;
    ESP_LOGI(TAG, "BLE OTA begin: %u bytes -> %s", (unsigned)s_total,
             next->label);
    notify_status(OTA_ST_READY, 0);
}

/* Pull one chunk off the ring buffer into flash. Returns false when the
 * buffer was empty. */
static bool drain_one(TickType_t wait)
{
    size_t sz = 0;
    uint8_t *p = (uint8_t *)xRingbufferReceive(s_rb, &sz, wait);
    if (!p) return false;

    esp_err_t err = esp_ota_write(s_ota, p, sz);
    if (err == ESP_OK) {
        mbedtls_sha256_update(&s_sha, p, sz);
        s_recv += (uint32_t)sz;
    }
    vRingbufferReturnItem(s_rb, p);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write @%u: %s", (unsigned)s_recv,
                 esp_err_to_name(err));
        notify_status(OTA_ST_ERROR, OTA_ERR_WRITE);
        transfer_reset();
        return false;
    }
    if (s_recv >= s_next_progress) {
        notify_status(OTA_ST_PROGRESS, s_recv);
        s_next_progress += OTA_PROGRESS_STEP;
    }
    return true;
}

static void do_finalize(void)
{
    if (s_state != ST_RECEIVING) {
        notify_status(OTA_ST_ERROR, OTA_ERR_PROTO);
        transfer_reset();
        return;
    }

    /* END may overtake the last data chunks (ctrl queue vs data ringbuf) —
     * drain whatever is still in flight, bounded so a stalled host can't
     * wedge the worker. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while (s_recv < s_total && xTaskGetTickCount() < deadline) {
        if (!drain_one(pdMS_TO_TICKS(50)) && s_state != ST_RECEIVING) {
            return;              /* write error mid-drain already cleaned up */
        }
    }

    if (s_recv != s_total) {
        ESP_LOGE(TAG, "END with recv=%u want=%u", (unsigned)s_recv,
                 (unsigned)s_total);
        notify_status(OTA_ST_ERROR, OTA_ERR_PROTO);
        transfer_reset();
        return;
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&s_sha, digest);
    mbedtls_sha256_free(&s_sha);
    s_sha_active = false;
    if (memcmp(digest, s_expect_sha, 32) != 0) {
        ESP_LOGE(TAG, "sha256 mismatch — corrupted transfer");
        notify_status(OTA_ST_ERROR, OTA_ERR_SHA);
        transfer_reset();
        return;
    }

    esp_err_t err = esp_ota_end(s_ota);
    s_ota_open = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        notify_status(OTA_ST_ERROR, OTA_ERR_END);
        transfer_reset();
        return;
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        notify_status(OTA_ST_ERROR, OTA_ERR_BOOT);
        transfer_reset();
        return;
    }

    s_state = ST_IDLE;
    ESP_LOGW(TAG, "BLE OTA OK -> %s, rebooting in 1500 ms", next->label);
    notify_status(OTA_ST_DONE, 0);
    /* Let the DONE notification reach the air before the link drops. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void worker(void *arg)
{
    (void)arg;
    ota_evt_t ev;
    for (;;) {
        TickType_t wait = (s_state == ST_RECEIVING) ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_q, &ev, wait) == pdTRUE) {
            switch (ev.kind) {
                case EV_BEGIN:    do_begin(&ev);        break;
                case EV_FINALIZE: do_finalize();        break;
                case EV_FAIL:
                    ESP_LOGW(TAG, "OTA fail code=%u", (unsigned)ev.detail);
                    notify_status(OTA_ST_ERROR, ev.detail);
                    transfer_reset();
                    break;
                case EV_ABORT:
                    ESP_LOGW(TAG, "OTA aborted");
                    transfer_reset();
                    break;
            }
            continue;
        }
        if (s_state == ST_RECEIVING) {
            drain_one(pdMS_TO_TICKS(OTA_DRAIN_TICK_MS));
        }
    }
}

/* ---- public API ---- */

void ble_ota_init(void)
{
    if (s_q) return;
    s_q  = xQueueCreate(8, sizeof(ota_evt_t));
    s_rb = xRingbufferCreate(OTA_RB_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (!s_q || !s_rb) {
        ESP_LOGE(TAG, "alloc failed");
        return;
    }
    xTaskCreate(worker, "ble_ota", 4096, NULL, 5, &s_task);
    ESP_LOGI(TAG, "ble_ota ready (stream-to-flash, rb=%d B)", OTA_RB_SIZE);
}

void ble_ota_set_link(uint16_t conn, uint16_t ctrl_val_handle)
{
    s_conn = conn;
    s_ctrl_handle = ctrl_val_handle;
}

void ble_ota_on_disconnect(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    if (s_state == ST_RECEIVING) {
        ota_evt_t ev = { .kind = EV_ABORT };
        if (s_q) xQueueSend(s_q, &ev, 0);
    }
}

/* ---- routed from ble_cfg_svc access_cb (NimBLE host task) ---- */

void ble_ota_ctrl_write(const uint8_t *data, uint16_t len)
{
    if (len < 1 || !s_q) return;
    switch (data[0]) {
        case OTA_OP_BEGIN: {
            ota_evt_t ev = { .kind = EV_BEGIN };
            if (len < OTA_BEGIN_LEN) {
                ev.kind = EV_FAIL;
                ev.detail = OTA_ERR_PROTO;
                xQueueSend(s_q, &ev, 0);
                return;
            }
            ev.total_len = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
                           ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
            memcpy(ev.sha, data + 5, 32);
            s_state = ST_IDLE;
            xQueueSend(s_q, &ev, portMAX_DELAY);
            break;
        }
        case OTA_OP_END: {
            ota_evt_t ev = { .kind = EV_FINALIZE };
            xQueueSend(s_q, &ev, portMAX_DELAY);
            break;
        }
        case OTA_OP_ABORT: {
            ota_evt_t ev = { .kind = EV_ABORT };
            xQueueSend(s_q, &ev, 0);
            break;
        }
        default:
            break;
    }
}

void ble_ota_data_write(const uint8_t *data, uint16_t len)
{
    if (s_state != ST_RECEIVING || !len) return;
    /* 200 ms of backpressure tolerance — the worker normally drains far
     * faster than BLE delivers. A persistent overflow means flash writes
     * stalled; fail the transfer rather than corrupt it. */
    if (xRingbufferSend(s_rb, data, len, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(TAG, "ring buffer overflow at %u B", (unsigned)s_recv);
        s_state = ST_FAILED;
        ota_evt_t ev = { .kind = EV_FAIL, .detail = OTA_ERR_ALLOC };
        if (s_q) xQueueSend(s_q, &ev, 0);
    }
}
