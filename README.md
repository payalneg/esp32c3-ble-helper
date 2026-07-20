# VESC BLE Helper

**English** | [Русский](README.ru.md)

A tiny ESP32-C3 board that bridges **sleepy BLE gadgets** — camera-shutter
remotes, media buttons, a BLE cadence sensor — to a **VESC motor controller
over the CAN bus**.

```
[HID remote(s) ×3]──BLE──┐                              CAN 500k
[Cadence sensor]────BLE──┼── ESP32-C3 ── TWAI ──[TJA1051]────── VESC (main.lisp)
[GUI / Android app]─BLE──┘   NUS bridge + config service + OTA      │
                                                              [P4 display]*
                                                              * optional
```

What it does:

* **Buttons → CAN commands.** Press a button on any bound HID remote and the
  helper transmits a raw CAN frame you configured for it. A LispBM script on
  the VESC receives the frame and acts: throttle on/off, speed-profile
  switch, or anything you script. All semantics live in LISP — the helper is
  a dumb, reliable bridge.
* **Pedal assist (PAS).** Reads a BLE cadence sensor
  ([payalneg/cadence-for-ebike](https://github.com/payalneg/cadence-for-ebike))
  and streams a ramped motor-current setpoint to the VESC's LISP arbiter at
  20 Hz, with a triple watchdog chain behind it.
* **VESC Tool bridge.** Connect VESC Tool over BLE (Nordic UART Service) and
  configure the VESC through the helper — the helper forwards the packets to
  CAN transparently.
* **Configurable over BLE.** A Python GUI (`tools/config_gui.py`) binds
  devices, tunes PAS, assigns button commands and updates the firmware OTA.
  A future Android app uses the same GATT protocol.

The CAN/BLE/PAS core is ported from the
[esp32p4-android-auto](../work/esp32p4-android-auto) project (branch
`pas-system`); `lisp/main.lisp` is that project's motor-control script with
helper-command handling added.

---

## Hardware

| Part | Notes |
|---|---|
| **ESP32-C3 board** | 4 MB flash, no PSRAM needed. Tested pinout = VESC Express (GPIO0/1). On a stock Seeed XIAO ESP32-C3 GPIO0/1 are **not broken out** — use D4/D5 instead (see below). |
| **CAN transceiver** | **TJA1051T/3** (VIO variant) — direct hookup. Field-tested: **TJA1050 modules and plain TJA1051** also work — add the resistor divider on RXD (below). SN65HVD230 (native 3.3 V) works too. |
| **Power** | 5 V for the transceiver; the ESP board from its own 5 V/USB input. Common ground with the VESC. |

### Wiring (default pins — VESC Express layout)

```
ESP32-C3                TJA1051T/3                    VESC / bus
─────────               ──────────                    ──────────
GPIO1  (CAN TX) ───────► TXD
GPIO0  (CAN RX) ◄─────── RXD
3V3     ───────────────► VIO   (logic level reference)
5V      ───────────────► VCC
GND     ───────────────► GND
                         S ────► GND  (silent mode OFF)
                         CANH ───────────────────────► CANH
                         CANL ───────────────────────► CANL
```

* **TJA1051 without the “/3” suffix and TJA1050 modules** run 5 V logic:
  their RXD swings to 5 V and the ESP32-C3 is **not 5-V-tolerant**. With a
  divider on RXD (e.g. 1 kΩ / 2 kΩ) both are field-tested working; TXD
  connects directly (its input threshold accepts 3.3 V). The /3 variant
  with VIO needs no divider.
* **Termination:** the CAN bus needs 120 Ω at both physical ends. If the
  helper is an end node, enable/fit the resistor.
* GPIO0/GPIO1 are safe for CAN on the C3 — its strapping pins are 2/8/9.

### VESC Express boards

The default pinout intentionally matches **VESC Express hardware**
(`hw_xp_t.h`: ESP32-C3, CAN TX=GPIO1/RX=GPIO0, onboard transceiver) — this
firmware should run on a VESC Express T / compatible board as a drop-in
replacement, no wiring at all: flash it over USB and the helper takes over
the board's CAN port. (Not yet verified on an original Trampa unit — reports
welcome.)

### Alternative pins (stock XIAO ESP32-C3)

GPIO0/1 are not exposed on the XIAO. Set in `idf.py menuconfig → VESC CAN`:

| Signal | XIAO pin | GPIO |
|---|---|---|
| CAN TX | D5 | GPIO7 |
| CAN RX | D4 | GPIO6 |

### Default CAN identities

| Node | ID |
|---|---|
| Drive VESC | **10** |
| Helper (this device) | **3** |
| P4 display (if present) | 2 |

Bitrate 500 kbps. IDs and bitrate (125/250/500/1000) are changeable in the
GUI and persist in NVS.

---

## Building & flashing

ESP-IDF v5.5+:

```bash
source ~/.espressif/v5.5.3/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor     # first flash over USB
```

Subsequent updates go **over BLE** from the GUI (Firmware tab, file
`build/esp32c3_ble_helper.bin`).

## First-time setup

```bash
pip install bleak
python3 tools/config_gui.py
```

1. **Connect** — the GUI finds the device advertising as `VESC-BLE-Helper`.
2. **LISP tab** — upload `lisp/main.lisp` to the VESC (goes through the NUS
   bridge using the VESC Tool protocol). This script owns the motor:
   throttle/brake/cruise/PAS arbiter + helper-command handling.
3. **Binding tab** — “Scan for button”, press a button on the remote so it
   wakes up, select it, “Bind selected”. Repeat for more remotes (up to 3).
   Same flow for the cadence sensor (spin the crank to wake it).
4. **Press each button once.** Buttons are learned by their press signature:
   the first button you ever press becomes **A**, the next **B**, … (up to
   H, across all remotes). Watch the Status tab.
5. **Parameters tab** — check the per-button CAN commands (defaults below),
   PAS tuning, CAN IDs/bitrate. “Write” persists everything to NVS.

Bindings, bonds, learned buttons and parameters survive reboots; the remotes
and the sensor reconnect automatically whenever they wake up.

## Button commands

Every button press transmits its configured CAN frame (hex ID + up to
8 data bytes). Frames with a standard 11-bit ID are invisible to the VESC
protocol and land directly in the LISP script via `event-can-sid`.

Defaults (matching `lisp/main.lisp`):

| Button | Frame | Action in `main.lisp` |
|---|---|---|
| A | id `123`, data `0001` | throttle master switch on/off |
| B | id `123`, data `0002` | switch speed profile (beeps per profile) |
| C… | id `123`, data `0003`… | printed — add your own in `proc-helper-btn` |

The command is the data bytes read as a big-endian u16. To add an action,
extend the `cond` in `proc-helper-btn`:

```lisp
((= cmd 3) (activate-cruise-control))   ; example: button C = cruise
```

A standalone minimal receiver lives in `lisp/can_button_skeleton.lisp`;
a minimal throttle-only script in `lisp/throttle_toggle.lisp`.

## Pedal assist

`pas.c` turns cadence into a current setpoint: SWITCH (fixed per level) or
PROPORTIONAL (scales with cadence) modes, assist levels, start delay,
initial-kick floor, A/s ramp, stop delay. The setpoint streams to the LISP
arbiter as a `'VP' 0x05` frame at 20 Hz.

Safety chain (each layer fails to coast):

1. No sensor notifications for 600 ms → cadence treated as zero.
2. No fresh setpoint for 400 ms → helper sends a single `0` and goes quiet.
3. LISP: setpoint older than 0.4 s → coast.
4. Script dies → `app-disable-output` expires in 1.5 s, stock throttle
   returns, motor stops via the command timeout.

## BLE interface (for app developers)

Advertised name `VESC-BLE-Helper`; the NUS UUID is in the advertisement (so
VESC Tool finds it), the name is in the scan response.

| Service | UUID | Purpose |
|---|---|---|
| Nordic UART | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` | transparent VESC Tool ↔ CAN bridge |
| Config | `ab1e0001-b1e5-4e15-8ac3-5e00c0de15b7` | everything below |

Config-service characteristics (`…0002`–`…0006`): CTRL (commands + acks),
STATUS (~2 Hz notify + instant on button events), SCAN (scan results
stream), OTA-CTRL / OTA-DATA (firmware update, stream-to-flash). Full frame
formats are documented in [`main/ble_cfg_svc.h`](main/ble_cfg_svc.h).

## Sharing the CAN bus with the P4 display

* Exactly **one** PAS setpoint source on the bus: if the display runs the
  `pas-system` firmware, disable its on-device PAS.
* Unique controller IDs for every node.
* The throttle switch state lives on the VESC — the display's touchscreen
  and the helper's buttons can both flip it, last write wins.

## Project layout

```
main/                   firmware application
  ble_host.c            NimBLE dual-role host, advertising
  ble_central_mgr.c     one shared whitelist connect for all central links
  ble_hid_client.c      HOGP host, ×3 remotes, signature-learned buttons
  ble_cadence_client.c  cadence sensor client (custom cad0000x service)
  ble_nus.c             VESC Tool bridge (Nordic UART ↔ CAN)
  ble_cfg_svc.c         config GATT service + OTA routing
  ble_ota.c             BLE OTA, stream-to-flash (no PSRAM staging)
  pas.c                 cadence → current control loop
  throttle_ctl.c        throttle master-switch commands
  settings.c            NVS: ids, bitrate, remotes, buttons, frames
components/vesc_can/    TWAI driver + VESC CAN protocol + 'VP' transport
lisp/
  main.lisp             VESC-side motor arbiter + panel + helper commands
  can_button_skeleton.lisp  minimal helper-command receiver
  throttle_toggle.lisp  minimal throttle on/off example
tools/config_gui.py     bleak + tkinter configurator
```

## Credits

* CAN/BLE/PAS core and `main.lisp` — ported from `esp32p4-android-auto`
  (branch `pas-system`).
* Cadence sensor firmware —
  [payalneg/cadence-for-ebike](https://github.com/payalneg/cadence-for-ebike)
  (the helper only speaks its BLE protocol).
* `components/vesc_can` protocol code — adapted from VESC firmware by
  Benjamin Vedder (GPL-3.0).
