# VESC BLE Helper — Android configurator

Flutter port of `tools/config_gui.py`: connects over BLE to the
`VESC-BLE-Helper` device (XIAO ESP32-C3) and provides the same five tabs —
Binding, Parameters, Status, Firmware (OTA) and LISP upload — plus the
scrolling log.

The helper firmware image and the VESC LISP script are **bundled into the
APK** as assets, so the phone needs no file transfer:

| Asset | Source | Used by |
|---|---|---|
| `assets/firmware/esp32c3_ble_helper.bin` | `../build/esp32c3_ble_helper.bin` | Firmware tab (OTA) |
| `assets/lisp/main.lisp` | `../lisp/main.lisp` | LISP tab (NUS bridge) |

## Build

One-shot release (bump versions, build firmware + APK, stage versioned
artifacts into `../release/`):

```sh
../scripts/release.sh              # patch-bump fw + app
../scripts/release.sh 1.0.1 1.0.1  # explicit versions
../scripts/install_app.sh          # adb install the newest release APK
```

Manual build of just the app:

```sh
# refresh bundled payloads after `idf.py build` or editing lisp/main.lisp
../tools/sync_app_assets.sh

flutter build apk --release
# → build/app/outputs/flutter-apk/app-release.apk
```

Install on a USB-connected phone with `flutter install`, or copy the APK.
End users flash a bricked/new board over USB with `../release/flash.command`
(Mac) / `flash.bat` (Windows); day-to-day firmware updates go over BLE from
the app's Firmware tab.

## Code map

- `lib/src/protocol/` — pure-Dart protocol layer (packed structs, CRC16,
  VESC framing, LISP blob). Layouts mirror the struct formats in
  `tools/config_gui.py`; golden-vector tests in `test/protocol_test.dart`
  were generated with the same Python `struct.pack` calls.
- `lib/src/ble/` — `HelperBleClient` (flutter_blue_plus): scan by the
  advertised NUS service UUID (the device name only appears in the scan
  response), MTU 512, notification routing, OTA and LISP transfer flows.
- `lib/src/ui/` — the five tabs + log panel.

Bluetooth runtime permissions are requested by flutter_blue_plus itself;
the manifest declares `BLUETOOTH_SCAN` (`neverForLocation`) /
`BLUETOOTH_CONNECT` for Android 12+ and the legacy set for Android ≤ 11.
