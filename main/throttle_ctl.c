#include "throttle_ctl.h"

#include "esp_log.h"

#include "vesc_can/vesc_lisp_panel.h"

static const char *TAG = "throttle";

/* Fallback until the first reply from the VESC arrives. main.lisp boots with
 * throttle-on = 1. */
static bool s_shadow = true;

bool throttle_ctl_get(bool *valid)
{
    bool on;
    if (vesc_lisp_panel_get_throttle(&on)) {
        if (valid) *valid = true;
        return on;
    }
    if (valid) *valid = false;
    return s_shadow;
}

void throttle_ctl_set(bool on)
{
    s_shadow = on;
    vesc_lisp_panel_send_action(VLP_THROTTLE_CTRL_ID, on ? 1.0f : 0.0f);
    ESP_LOGI(TAG, "throttle -> %s", on ? "ON" : "off");
}

void throttle_ctl_toggle(void)
{
    /* Atomic on the VESC (VLP_MSG_THROTTLE_TOGGLE): the script flips its own
     * `throttle-on` and answers with STATE, which refreshes our snapshot — no
     * polling, no local guess about the current state. */
    s_shadow = !throttle_ctl_get(NULL);
    vesc_lisp_panel_send_throttle_toggle();
    ESP_LOGI(TAG, "throttle toggle sent");
}
