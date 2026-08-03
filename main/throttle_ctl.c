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
    /* Which way to flip is decided on the CAN task, where the STATE replies are
     * parsed: known state → absolute set, unknown → ask and retry. Do NOT
     * pre-flip s_shadow from a guess — when the state was never learned that
     * guess used to drive the actual command, and getting it backwards turns
     * the LISP master switch off (pedal assist then coasts with a perfectly
     * healthy cadence sensor). */
    bool valid = false;
    bool on    = throttle_ctl_get(&valid);
    if (valid) s_shadow = !on;
    vesc_lisp_panel_send_throttle_toggle();
    ESP_LOGI(TAG, "throttle toggle sent (state %s)",
             valid ? (on ? "ON" : "off") : "unknown");
}
