#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up NimBLE on the C3's native controller.
 *
 * Order: nimble_port_init → register host callbacks + GATT services (NUS
 * bridge + config service) → start NimBLE host task. On host sync,
 * advertising starts and the central manager arms the button/cadence
 * reconnects. Idempotent. */
esp_err_t ble_host_init(void);

/* True while a peer (VESC Tool / config GUI / app) is connected to the
 * NimBLE GATT server. Safe to call from any task. */
bool ble_host_is_connected(void);

#ifdef __cplusplus
}
#endif
