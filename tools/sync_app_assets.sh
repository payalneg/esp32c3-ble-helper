#!/bin/sh
# Stage the payloads bundled into the Android app: the freshly-built helper
# firmware (for over-BLE OTA), lisp/main.lisp (for the LISP tab) and the
# firmware version string the app shows next to the bundled image. Run after
# `idf.py build` or after editing lisp/main.lisp, before `flutter build apk`
# (scripts/release.sh does all of that in one shot).
set -e
cd "$(dirname "$0")/.."

if [ ! -f build/esp32c3_ble_helper.bin ]; then
    echo "sync_app_assets: build/esp32c3_ble_helper.bin not found — run 'idf.py build' first" >&2
    exit 1
fi

mkdir -p app/assets/firmware app/assets/lisp
cp build/esp32c3_ble_helper.bin app/assets/firmware/esp32c3_ble_helper.bin
cp lisp/main.lisp app/assets/lisp/main.lisp
# First line of version.txt only (strip whitespace) — keeps the version the
# app displays in lockstep with what was built (PROJECT_VER reads the same file).
head -n1 version.txt | tr -d '[:space:]' > app/assets/firmware/version.txt

echo "sync_app_assets: firmware $(wc -c < app/assets/firmware/esp32c3_ble_helper.bin | tr -d ' ') bytes, version $(cat app/assets/firmware/version.txt)"
echo "sync_app_assets: lisp     $(wc -c < app/assets/lisp/main.lisp | tr -d ' ') bytes"
