# VESC BLE Helper (Seeed XIAO ESP32-C3)

BLE helper for a VESC-based e-bike: bridges a **BLE camera-shutter button**
(HID keychain remote) and a **BLE cadence sensor** to the VESC controller over
the **CAN bus**.

* Button press — **throttle on/off** (the `throttle-on` flag in the LISP
  script running on the VESC, the same switch the P4 display's touchscreen
  flips).
* Cadence sensor — **PAS** (pedal assist): pedaling sets the motor current
  with ramps and delays; the setpoint streams to the VESC LISP arbiter at
  20 Hz with a watchdog chain behind it.
* Configuration — Python GUI (`tools/config_gui.py`) over BLE; the future
  Android app will use the same GATT protocol.
* Helper firmware updates — **OTA over BLE** from the GUI.
* NUS (Nordic UART) — a transparent **VESC Tool ↔ CAN** bridge: connect
  VESC Tool to the helper over BLE and configure the VESC itself.

The BLE/CAN/PAS code is ported from the `esp32p4-android-auto` project
(branch `pas-system`); the LISP script `lisp/main.lisp` is reused with one
addition (the atomic throttle-toggle command).

## Hardware

| What | How |
|---|---|
| Board | ESP32-C3 (4 MB flash, no PSRAM) |
| CAN transceiver | TJA1051 (use the **/3 variant with VIO = 3.3 V** — the plain 5 V-logic TJA1051T drives RXD to 5 V, which the C3 is not tolerant of) or SN65HVD230 |
| TWAI TX → TXD | **GPIO1** (default, same as VESC Express `hw_xp_t.h`; change in `menuconfig → VESC CAN`) |
| TWAI RX ← RXD | **GPIO0** |
| CAN bitrate | 500 kbps default; 125/250/500/1000 selectable in the GUI (persisted in NVS) |

> On a standard Seeed XIAO ESP32-C3 GPIO0/1 are not broken out — set
> TX=7 (D5) / RX=6 (D4) via Kconfig there.
> Note: GPIO0/1 double as ADC/XTAL_32K pins but have no boot-strapping role
> on the C3 (strapping pins are 2/8/9), so CAN on 0/1 is safe.

Default CAN IDs: helper **3**, P4 display **2**, VESC **10** (changeable in
the GUI / NVS).

## Build & flash

```bash
source ~/.espressif/v5.5.3/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

Subsequent updates go over BLE from the GUI ("Firmware" tab,
file `build/esp32c3_ble_helper.bin`).

## Configuration (Python GUI)

```bash
pip install bleak
python3 tools/config_gui.py
```

1. **Connect** — the GUI looks for a device named `VESC-BLE-Helper`.
2. **Binding** tab: "Scan for button" (press the remote's button so it wakes
   up and advertises) → pick it in the list → "Bind selected". Same for the
   cadence sensor (spin the crank to wake it).
3. **Parameters** tab: assist levels, cadence thresholds, max current, ramp,
   CAN IDs. "Read" / "Write".
4. **LISP** tab: upload `lisp/main.lisp` to the VESC (via the NUS bridge,
   VESC Tool protocol). A minimal teaching example is
   `lisp/throttle_toggle.lisp`.
5. **Status** tab: live cadence, button state, throttle, assist current.

Bindings and parameters persist in NVS across reboots; the button is bonded
(keys in NVS too), and both devices reconnect automatically after sleeping.

## How it works

```
[HID button 0x1812]───BLE──┐                            CAN 500k
[Cadence cad00002-…]──BLE──┼─ XIAO ESP32-C3 ─ TWAI ─[transceiver]── VESC (main.lisp)
[GUI / Android app]───BLE──┘   NUS bridge + config service + OTA
```

* **`main/ble_central_mgr.c`** — NimBLE allows only one outstanding outgoing
  connection attempt, but there are two sleepy peripherals; the manager keeps
  a single whitelist-filtered connect covering every bound address — whichever
  device wakes first connects instantly.
* **`main/ble_hid_client.c`** — HOGP host for **any HID devices** (shutter
  remotes, media buttons, mouse, …), up to **3 remotes bound and connected
  simultaneously**: bonding (Just Works), subscribes to every input report.
  Buttons are identified by their **press signature** (device + report +
  byte + value), because cheap remotes often pack both physical buttons into
  one report; signatures are learned on first press (first button you press
  becomes A, next B, … up to H across all remotes), persisted in NVS,
  cleared on unbind. 300 ms debounce per button. "Bind" adds a remote,
  "Unbind" clears them all.
* **Button → CAN command (GUI-configurable, NVS-persisted):** every button
  press transmits its configured raw CAN frame (hex ID + up to 8 data bytes).
  Frames with a standard 11-bit ID bypass the VESC protocol and land straight
  in the LISP script via `event-can-sid` — ALL button semantics live in LISP.
  Example wired into `lisp/main.lisp` (`proc-helper-btn`, commands are the
  data bytes as big-endian u16): ID `123` data `0001` toggles the throttle,
  `0002` switches the speed profile (with a beep); extend it with your own
  commands, or start from the standalone skeleton
  `lisp/can_button_skeleton.lisp`.
* **`main/ble_cadence_client.c`** — client for
  [payalneg/cadence-for-ebike](https://github.com/payalneg/cadence-for-ebike):
  custom characteristic `cad00002-…` (int16 LE centi-RPM, sign = direction).
* **`main/pas.c`** — cadence → current: SWITCH/PROPORTIONAL modes, levels,
  start delay, A/s ramp, stop delay.
* **`components/vesc_can/`** — TWAI + VESC protocol; the PAS setpoint rides
  in a `'VP' 0x05` frame (i32 mA) on top of `COMM_CUSTOM_APP_DATA`.
* **`lisp/main.lisp`** (on the VESC) — 100 Hz motor arbiter with priority
  "master-off > brake > throttle > cruise > PAS > coast".
* **Poll-free.** The helper never polls the VESC periodically: commands are
  fire-and-forget into the LISP `panel-handle` event handler — the throttle
  toggle is the atomic `'VP' 0x06` message (the script flips `throttle-on`
  itself and replies with fresh state), the PAS setpoint is `0x05` at 20 Hz
  only while pedaling. A one-shot state query runs at boot and when the GUI
  connects.

### Safety chain

1. No sensor notifications for 600 ms → PAS treats cadence as zero.
2. `vesc_lisp_panel` with no fresh setpoint for 400 ms → sends `0` once and
   goes quiet.
3. LISP: setpoint older than 0.4 s → coast.
4. Script dies → `app-disable-output` expires within 1.5 s, the stock
   throttle comes back, the motor stops via the command timeout.

### Sharing the bus with the P4 display

* There must be exactly **one** source of the PAS setpoint: if the display
  runs the `pas-system` branch, disable its on-device PAS (or flash the
  master build).
* All nodes need unique controller IDs (helper 3, display 2, VESC 10).
* Both the button and the touchscreen may flip the throttle toggle — the
  state lives on the VESC, last write wins.

## Config-service protocol (for the Android app)

Service `ab1e0001-b1e5-4e15-8ac3-5e00c0de15b7`; full frame formats are
documented in `main/ble_cfg_svc.h`. Characteristics: CTRL (commands +
responses), STATUS (~2 Hz notifications), SCAN (scan-result stream),
OTA-CTRL/OTA-DATA (firmware update).
