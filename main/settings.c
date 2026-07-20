#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "vesc_can/comm_can.h"
#include "vesc_can/vesc_lisp_code.h"
#include "vesc_can/vesc_lisp_panel.h"
#include "vesc_can/vesc_lisp_poll.h"
#include "vesc_can/vesc_rt_data.h"

static const char *TAG = "settings";

#define NVS_NS         "helper"
#define KEY_CTRL_ID    "ctrl_id"
#define KEY_TARGET_ID  "target_id"
#define KEY_CAN_KBPS   "can_kbps"
#define KEY_REMOTES    "remotes"     /* [count][7 bytes × SETTINGS_MAX_REMOTES] */
#define KEY_BTN_ACTS   "btn_acts"    /* SETTINGS_BTN_SLOTS bytes: btn_action_t each */
#define KEY_BTN_FRAMES "btn_frames"  /* SETTINGS_BTN_SLOTS × btn_can_frame_t */
#define KEY_BTN_SIGS   "btn_sigs"    /* [u8 count][SETTINGS_BTN_SIGS × u32] */

static uint8_t  s_controller_id = CONFIG_VESC_CAN_CONTROLLER_ID;
static uint8_t  s_target_id     = CONFIG_VESC_CAN_TARGET_ID;
static uint16_t s_can_kbps      = CONFIG_VESC_CAN_SPEED_KBPS;

static bool can_kbps_valid(uint16_t k)
{
    return k == 125 || k == 250 || k == 500 || k == 1000;
}
/* Default: every button fires its custom CAN frame (the GUI only exposes the
 * custom-frame flow; built-in actions remain available to the protocol). */
static uint8_t s_btn_acts[SETTINGS_BTN_SLOTS] = {
    BTN_ACT_CUSTOM_CAN, BTN_ACT_CUSTOM_CAN, BTN_ACT_CUSTOM_CAN,
    BTN_ACT_CUSTOM_CAN, BTN_ACT_CUSTOM_CAN, BTN_ACT_CUSTOM_CAN,
    BTN_ACT_CUSTOM_CAN, BTN_ACT_CUSTOM_CAN,
};
/* Defaults: standard id 0x123, data = big-endian u16 command 1..8 (button
 * number). Matches the example handler in lisp/main.lisp: 0001 = throttle
 * toggle, 0002 = profile switch. */
static btn_can_frame_t s_btn_frames[SETTINGS_BTN_SLOTS] = {
    { .can_id = 0x123, .ext = 0, .len = 2, .data = {0x00, 0x01} },
    { .can_id = 0x123, .ext = 0, .len = 2, .data = {0x00, 0x02} },
    { .can_id = 0x123, .ext = 0, .len = 2, .data = {0x00, 0x03} },
    { .can_id = 0x123, .ext = 0, .len = 2, .data = {0x00, 0x04} },
    { .can_id = 0x123, .ext = 0, .len = 2, .data = {0x00, 0x05} },
    { .can_id = 0x123, .ext = 0, .len = 2, .data = {0x00, 0x06} },
    { .can_id = 0x123, .ext = 0, .len = 2, .data = {0x00, 0x07} },
    { .can_id = 0x123, .ext = 0, .len = 2, .data = {0x00, 0x08} },
};

/* Bound remotes, mirrored in NVS. */
static uint8_t s_remotes[SETTINGS_MAX_REMOTES][7];   /* [type][addr 6] */
static uint8_t s_remote_count;

void settings_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, KEY_CTRL_ID, &v) == ESP_OK && v >= 1 && v <= 254) {
        s_controller_id = v;
    }
    if (nvs_get_u8(h, KEY_TARGET_ID, &v) == ESP_OK && v >= 1 && v <= 254) {
        s_target_id = v;
    }
    uint16_t kbps;
    if (nvs_get_u16(h, KEY_CAN_KBPS, &kbps) == ESP_OK &&
        can_kbps_valid(kbps)) {
        s_can_kbps = kbps;
    }
    uint8_t acts[SETTINGS_BTN_SLOTS];
    size_t alen = sizeof(acts);
    if (nvs_get_blob(h, KEY_BTN_ACTS, acts, &alen) == ESP_OK &&
        alen == sizeof(acts)) {
        memcpy(s_btn_acts, acts, sizeof(s_btn_acts));
    }
    btn_can_frame_t frames[SETTINGS_BTN_SLOTS];
    size_t flen = sizeof(frames);
    if (nvs_get_blob(h, KEY_BTN_FRAMES, frames, &flen) == ESP_OK &&
        flen == sizeof(frames)) {
        memcpy(s_btn_frames, frames, sizeof(s_btn_frames));
    }
    uint8_t rem[1 + sizeof(s_remotes)];
    size_t rlen = sizeof(rem);
    if (nvs_get_blob(h, KEY_REMOTES, rem, &rlen) == ESP_OK &&
        rlen == sizeof(rem) && rem[0] <= SETTINGS_MAX_REMOTES) {
        s_remote_count = rem[0];
        memcpy(s_remotes, &rem[1], sizeof(s_remotes));
    }
    nvs_close(h);
    ESP_LOGI(TAG, "controller_id=%u target_vesc_id=%u can=%u kbps",
             s_controller_id, s_target_id, s_can_kbps);
}

uint8_t  settings_get_controller_id(void)  { return s_controller_id; }
uint8_t  settings_get_target_vesc_id(void) { return s_target_id; }
uint16_t settings_get_can_kbps(void)       { return s_can_kbps; }

static void nvs_put_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

void settings_set_controller_id(uint8_t id)
{
    if (id < 1 || id > 254 || id == s_controller_id) return;
    s_controller_id = id;
    nvs_put_u8(KEY_CTRL_ID, id);
    if (comm_can_reinit(id, s_can_kbps) != ESP_OK) {
        ESP_LOGW(TAG, "comm_can_reinit(ctrl=%u) failed", id);
    }
    ESP_LOGI(TAG, "controller_id -> %u", id);
}

void settings_set_can_kbps(uint16_t kbps)
{
    if (!can_kbps_valid(kbps) || kbps == s_can_kbps) return;
    s_can_kbps = kbps;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, KEY_CAN_KBPS, kbps);
        nvs_commit(h);
        nvs_close(h);
    }
    if (comm_can_reinit(s_controller_id, kbps) != ESP_OK) {
        ESP_LOGW(TAG, "comm_can_reinit(%u kbps) failed", kbps);
    }
    ESP_LOGI(TAG, "can bitrate -> %u kbps", kbps);
}

void settings_set_target_vesc_id(uint8_t id)
{
    if (id < 1 || id > 254 || id == s_target_id) return;
    s_target_id = id;
    nvs_put_u8(KEY_TARGET_ID, id);
    /* Same re-target fan-out as the P4 display's settings path: every poller
     * stores the id statically and its _init/set_target is idempotent. */
    vesc_rt_data_init(id, CONFIG_VESC_CAN_RT_INTERVAL_MS);
    vesc_rt_data_start();
    vesc_lisp_poll_init(id, CONFIG_VESC_CAN_LISP_INTERVAL_MS);
    vesc_lisp_code_set_target(id);
    vesc_lisp_panel_set_target(id);
    ESP_LOGI(TAG, "target_vesc_id -> %u", id);
}

static void remotes_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t blob[1 + sizeof(s_remotes)];
    blob[0] = s_remote_count;
    memcpy(&blob[1], s_remotes, sizeof(s_remotes));
    nvs_set_blob(h, KEY_REMOTES, blob, sizeof(blob));
    nvs_commit(h);
    nvs_close(h);
}

void settings_add_remote(const uint8_t addr[6], uint8_t addr_type)
{
    for (int i = 0; i < s_remote_count; i++) {
        if (s_remotes[i][0] == addr_type &&
            memcmp(&s_remotes[i][1], addr, 6) == 0) {
            return;                            /* already stored */
        }
    }
    if (s_remote_count >= SETTINGS_MAX_REMOTES) return;
    s_remotes[s_remote_count][0] = addr_type;
    memcpy(&s_remotes[s_remote_count][1], addr, 6);
    s_remote_count++;
    remotes_persist();
}

uint8_t settings_load_remotes(uint8_t addrs[SETTINGS_MAX_REMOTES][6],
                              uint8_t types[SETTINGS_MAX_REMOTES])
{
    for (int i = 0; i < s_remote_count; i++) {
        types[i] = s_remotes[i][0];
        memcpy(addrs[i], &s_remotes[i][1], 6);
    }
    return s_remote_count;
}

void settings_clear_remotes(void)
{
    s_remote_count = 0;
    memset(s_remotes, 0, sizeof(s_remotes));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, KEY_REMOTES);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t settings_get_button_action(uint8_t idx)
{
    return idx < SETTINGS_BTN_SLOTS ? s_btn_acts[idx] : BTN_ACT_NONE;
}

void settings_get_button_actions(uint8_t actions[SETTINGS_BTN_SLOTS])
{
    memcpy(actions, s_btn_acts, SETTINGS_BTN_SLOTS);
}

void settings_set_button_actions(const uint8_t actions[SETTINGS_BTN_SLOTS])
{
    memcpy(s_btn_acts, actions, SETTINGS_BTN_SLOTS);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, KEY_BTN_ACTS, s_btn_acts, SETTINGS_BTN_SLOTS);
    nvs_commit(h);
    nvs_close(h);
}

void settings_get_button_frame(uint8_t idx, btn_can_frame_t *out)
{
    if (!out) return;
    if (idx >= SETTINGS_BTN_SLOTS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_btn_frames[idx];
}

void settings_set_button_frame(uint8_t idx, const btn_can_frame_t *f)
{
    if (idx >= SETTINGS_BTN_SLOTS || !f) return;
    s_btn_frames[idx] = *f;
    if (s_btn_frames[idx].len > 8) s_btn_frames[idx].len = 8;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, KEY_BTN_FRAMES, s_btn_frames, sizeof(s_btn_frames));
    nvs_commit(h);
    nvs_close(h);
}

uint8_t settings_load_button_sigs(uint32_t sigs[SETTINGS_BTN_SIGS])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t blob[1 + SETTINGS_BTN_SIGS * 4];
    size_t  len = sizeof(blob);
    uint8_t count = 0;
    if (nvs_get_blob(h, KEY_BTN_SIGS, blob, &len) == ESP_OK &&
        len == sizeof(blob) && blob[0] <= SETTINGS_BTN_SIGS) {
        count = blob[0];
        memcpy(sigs, &blob[1], SETTINGS_BTN_SIGS * 4);
    }
    nvs_close(h);
    return count;
}

void settings_save_button_sigs(const uint32_t sigs[SETTINGS_BTN_SIGS],
                               uint8_t count)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t blob[1 + SETTINGS_BTN_SIGS * 4];
    blob[0] = count;
    memcpy(&blob[1], sigs, SETTINGS_BTN_SIGS * 4);
    nvs_set_blob(h, KEY_BTN_SIGS, blob, sizeof(blob));
    nvs_commit(h);
    nvs_close(h);
}

void settings_erase_button_sigs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, KEY_BTN_SIGS);
    nvs_commit(h);
    nvs_close(h);
}
