#pragma once

/* Throttle master switch — the single place that flips the VESC LISP
 * `throttle-on` flag (quick-panel control id 1 in lisp/main.lisp, the same
 * control the P4 touchscreen toggles).
 *
 * Poll-free: toggle is VLP_MSG_THROTTLE_TOGGLE — the script flips its own
 * state atomically and replies with STATE, which refreshes our snapshot.
 * Until the first reply arrives a local shadow is used so a button press
 * right after boot still does something sensible. */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Current throttle-on state. Returns the VESC-reported value when the panel
 * model is fresh, else the local shadow; *valid tells which one you got. */
bool throttle_ctl_get(bool *valid);

/* Set / toggle. Queues a panel ACTION (sent from the CAN poll task) and
 * updates the shadow immediately. Safe from any task. */
void throttle_ctl_set(bool on);
void throttle_ctl_toggle(void);

#ifdef __cplusplus
}
#endif
