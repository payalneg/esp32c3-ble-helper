#!/usr/bin/env bash
# Install the VESC BLE Helper configurator app onto a USB-connected Android
# phone — convenience entry point sitting next to the built APKs.
#
# Thin wrapper around scripts/install_app.sh (the maintained implementation:
# locates adb, picks the newest release/vesc_ble_helper-*.apk, `adb install -r`
# so app data survives, gates on a single connected device).
#
# Usage (run from anywhere):
#   release/install.sh                  # newest release/vesc_ble_helper-*.apk
#   release/install.sh 1.0.2            # a specific version
#   release/install.sh path/to/app.apk  # an explicit file
#   ANDROID_SERIAL=<serial> release/install.sh   # choose phone when >1
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$DIR/../scripts/install_app.sh" "$@"
