/* VESC BLE Helper — XIAO ESP32-C3.
 *
 * Bridges two sleepy BLE peripherals to a VESC over CAN:
 *   - a HID camera-shutter button (press → toggle the LISP `throttle-on`
 *     master switch on the VESC, same control the P4 dashboard's touchscreen
 *     flips);
 *   - a BLE cadence sensor (signed centi-RPM → PAS current setpoint, ramped
 *     by pas.c and streamed to the LISP arbiter at 20 Hz with a watchdog
 *     chain behind it).
 *
 * Configuration + OTA go over a dedicated GATT service (ble_cfg_svc, Python
 * GUI in tools/config_gui.py); the Nordic UART service stays a transparent
 * VESC Tool bridge onto the CAN bus. */

#include "esp_log.h"
#include "nvs_flash.h"

#include "ble_cfg_svc.h"
#include "ble_hid_client.h"
#include "ble_host.h"
#include "ble_nus.h"
#include "ble_ota.h"
#include "pas.h"
#include "settings.h"
#include "throttle_ctl.h"

#include "vesc_can/comm_can.h"
#include "vesc_can/vesc_lisp_code.h"
#include "vesc_can/vesc_lisp_panel.h"
#include "vesc_can/vesc_lisp_poll.h"
#include "vesc_can/vesc_rt_data.h"

static const char *TAG = "main";

/* Every reassembled VESC packet arriving over CAN fans out to each consumer;
 * they gate on their own command bytes. Runs on the CAN RX task. */
static void vesc_packet_dispatch(const uint8_t *data, unsigned int len)
{
    vesc_rt_data_process_response(data, len);
    vesc_lisp_poll_process_response(data, len);
    vesc_lisp_code_process_response(data, len);
    vesc_lisp_panel_process_response(data, len);
    ble_nus_forward_response(data, (uint16_t)len);
}

/* Two-button remotes map each physical button to its own HID report; the
 * client indexes them in handle order and the GUI assigns an action per
 * index (persisted in NVS). Runs on the NimBLE host task. */
static void on_button_press(uint8_t btn_idx)
{
    pas_settings_t s;
    switch (settings_get_button_action(btn_idx)) {
    case BTN_ACT_THROTTLE:
        throttle_ctl_toggle();
        break;
    case BTN_ACT_PAS:
        pas_get_settings(&s);
        pas_set_enabled(!s.enabled);
        break;
    case BTN_ACT_LEVEL:
        pas_get_settings(&s);
        pas_set_level(s.level >= s.level_count ? 1 : s.level + 1);
        break;
    case BTN_ACT_CUSTOM_CAN: {
        /* Skeleton for arbitrary commands: fire the button's configured CAN
         * frame. With a standard id the VESC LISP script receives it via
         * (event-enable 'event-can-sid) — see lisp/can_button_skeleton.lisp. */
        btn_can_frame_t f;
        settings_get_button_frame(btn_idx, &f);
        if (f.ext) comm_can_transmit_eid(f.can_id, f.data, f.len);
        else       comm_can_transmit_sid(f.can_id, f.data, f.len);
        ESP_LOGI(TAG, "button %u: custom CAN id=0x%X len=%u", btn_idx,
                 (unsigned)f.can_id, f.len);
        break;
    }
    default:
        ESP_LOGI(TAG, "button %u: no action assigned", btn_idx);
        break;
    }
    /* Push a status frame right away so the GUI shows the press without
     * waiting for the 500 ms tick. */
    ble_cfg_svc_notify_status_now();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    settings_init();

    /* ---- CAN side ---- */
    uint8_t ctrl_id = settings_get_controller_id();
    uint8_t tgt_id  = settings_get_target_vesc_id();
    err = comm_can_start(CONFIG_VESC_CAN_TX_GPIO, CONFIG_VESC_CAN_RX_GPIO,
                         ctrl_id, settings_get_can_kbps());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "comm_can_start failed: %s — CAN offline",
                 esp_err_to_name(err));
    }
    comm_can_set_packet_handler(vesc_packet_dispatch);

    vesc_rt_data_init(tgt_id, CONFIG_VESC_CAN_RT_INTERVAL_MS);
    vesc_lisp_poll_init(tgt_id, CONFIG_VESC_CAN_LISP_INTERVAL_MS);
    vesc_lisp_code_set_target(tgt_id);
    vesc_lisp_panel_init(tgt_id);
    /* Poll-free by design: periodic UI/STATE/DASH polling stays OFF. Commands
     * (button toggle, PAS setpoints, GUI actions) are fire-and-forget into the
     * LISP event handler; throttle state refreshes from the STATE replies the
     * script sends back. One boot-time query learns the initial state. */
    vesc_lisp_panel_query_state();
    vesc_rt_data_start_task();

    /* ---- BLE side ---- */
    ble_ota_init();
    ble_nus_init();
    ble_cfg_svc_init();
    ble_hid_set_press_cb(on_button_press);
    ESP_ERROR_CHECK(ble_host_init());

    /* PAS control loop: restores the bound cadence sensor from NVS and
     * feeds vesc_lisp_panel_set_pas. Must run after ble_host_init. */
    pas_init();

    /* Restore the bound remotes — the central manager picks them up. */
    uint8_t addrs[SETTINGS_MAX_REMOTES][6], types[SETTINGS_MAX_REMOTES];
    uint8_t n = settings_load_remotes(addrs, types);
    for (uint8_t i = 0; i < n; i++) {
        ble_hid_bind(addrs[i], types[i]);
    }
    if (n) ESP_LOGI(TAG, "restored %u bound remote(s) from NVS", n);

    ESP_LOGI(TAG, "VESC BLE Helper up: CAN id=%u -> VESC id=%u, "
             "TWAI TX=%d RX=%d @ %u kbps",
             ctrl_id, tgt_id, CONFIG_VESC_CAN_TX_GPIO, CONFIG_VESC_CAN_RX_GPIO,
             settings_get_can_kbps());
}
