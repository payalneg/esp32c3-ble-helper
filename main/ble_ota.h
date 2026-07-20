#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BLE OTA for the helper firmware.
 *
 * Lives on the config GATT service (ble_cfg_svc.c owns the chars and routes
 * writes here):
 *   OTA-CTRL CHR  ...0005  WRITE | NOTIFY   (host↔helper control)
 *   OTA-DATA CHR  ...0006  WRITE_NO_RSP     (host→helper firmware bytes)
 *
 * Wire protocol (same as the P4 display's BLE OTA, so tooling is shared):
 *
 *   CTRL write  (host → helper):
 *     0x01 BEGIN : [op][u32 total_len LE][32B sha256]   start a transfer
 *     0x02 END   : [op]                                  finalise + reboot
 *     0x03 ABORT : [op]                                  cancel
 *
 *   CTRL notify (helper → host), 5-byte frame [u8 status][u32 detail LE]:
 *     0x10 READY    detail=0            begin accepted — stream data now
 *     0x11 PROGRESS detail=bytes        liveness during receive
 *     0x12 DONE     detail=0            committed, rebooting
 *     0x1F ERROR    detail=err_code     see OTA_ERR_* in ble_ota.c
 *
 *   DATA write  (host → helper): raw firmware bytes, streamed sequentially
 *     after READY.
 *
 * Unlike the P4 (which stages the image in PSRAM), the C3 has no PSRAM and
 * not enough RAM for a full image — bytes stream through a ring buffer into
 * esp_ota_write on a worker task as they arrive. The flash partition is
 * erased during BEGIN, so wait for READY before sending data. */

void ble_ota_init(void);

/* ---- ble_cfg_svc wiring ---- */
void ble_ota_set_link(uint16_t conn_handle, uint16_t ctrl_val_handle);
void ble_ota_on_disconnect(void);

/* ---- routed from ble_cfg_svc access_cb (NimBLE host task) ---- */
void ble_ota_ctrl_write(const uint8_t *data, uint16_t len);
void ble_ota_data_write(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif
