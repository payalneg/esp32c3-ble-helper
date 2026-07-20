#pragma once

/* Helper-wide settings persisted in NVS: CAN identity (this node's controller
 * id, the drive VESC's id) and the bound HID button address. PAS tuning and
 * the bound cadence sensor live in pas.c (namespace "pas") — this module owns
 * everything else.
 *
 * Setters apply live (re-target the CAN consumers / re-init TWAI) AND persist,
 * mirroring how the P4 display's dev_settings behaves. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load persisted values (or Kconfig defaults) into RAM. Call before
 * comm_can_start so the getters below return the right identity. */
void settings_init(void);

uint8_t  settings_get_controller_id(void);
uint8_t  settings_get_target_vesc_id(void);
uint16_t settings_get_can_kbps(void);

/* Apply + persist. controller_id / can_kbps re-init TWAI;
 * target_vesc_id re-targets the RT/LISP/panel pollers.
 * can_kbps accepts 125 / 250 / 500 / 1000 only. */
void settings_set_controller_id(uint8_t id);
void settings_set_target_vesc_id(uint8_t id);
void settings_set_can_kbps(uint16_t kbps);

/* Bound HID remotes (NimBLE native byte order + addr type), up to
 * SETTINGS_MAX_REMOTES. add appends (no-op for a known address), clear wipes
 * the list. Must stay equal to BLE_HID_MAX_REMOTES. */
#define SETTINGS_MAX_REMOTES 3

void    settings_add_remote(const uint8_t addr[6], uint8_t addr_type);
uint8_t settings_load_remotes(uint8_t addrs[SETTINGS_MAX_REMOTES][6],
                              uint8_t types[SETTINGS_MAX_REMOTES]);
void    settings_clear_remotes(void);

/* Per-button actions. A two-button shutter remote maps each physical button
 * to its own HID report; ble_hid_client indexes them 0, 1, … in GATT handle
 * order. The GUI assigns an action to each index; defaults: 0 → throttle
 * toggle, 1 → PAS on/off. */
typedef enum {
    BTN_ACT_NONE       = 0,
    BTN_ACT_THROTTLE   = 1,   /* toggle throttle-on on the VESC */
    BTN_ACT_PAS        = 2,   /* toggle PAS enabled */
    BTN_ACT_LEVEL      = 3,   /* cycle assist level 1..level_count */
    BTN_ACT_CUSTOM_CAN = 4,   /* send the button's custom CAN frame (below) */
} btn_action_t;

#define SETTINGS_BTN_SLOTS 8   /* matches SETTINGS_BTN_SIGS: buttons A..H */

uint8_t settings_get_button_action(uint8_t idx);
void    settings_set_button_actions(const uint8_t actions[SETTINGS_BTN_SLOTS]);
void    settings_get_button_actions(uint8_t actions[SETTINGS_BTN_SLOTS]);

/* Custom CAN frame per button (used when the action is BTN_ACT_CUSTOM_CAN).
 * ext=0 → standard 11-bit id: invisible to the VESC protocol, delivered to
 * LispBM via (event-enable 'event-can-sid) — see lisp/can_button_skeleton.lisp.
 * ext=1 → extended 29-bit id (addressable to any other CAN node). */
typedef struct {
    uint32_t can_id;
    uint8_t  ext;
    uint8_t  len;        /* 0..8 */
    uint8_t  data[8];
} btn_can_frame_t;

void settings_get_button_frame(uint8_t idx, btn_can_frame_t *out);
void settings_set_button_frame(uint8_t idx, const btn_can_frame_t *f);

/* Learned button signatures. Cheap remotes often put BOTH physical buttons
 * into the same HID report (different bytes/bits), so buttons are identified
 * by a press signature — (report index, byte offset, value) packed into a
 * u32 — learned on first press and persisted so indices stay stable across
 * reboots. Cleared when the button is unbound. */
#define SETTINGS_BTN_SIGS 8

uint8_t settings_load_button_sigs(uint32_t sigs[SETTINGS_BTN_SIGS]);
void    settings_save_button_sigs(const uint32_t sigs[SETTINGS_BTN_SIGS],
                                  uint8_t count);
void    settings_erase_button_sigs(void);

#ifdef __cplusplus
}
#endif
