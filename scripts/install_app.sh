#!/usr/bin/env bash
# Install the VESC BLE Helper configurator app onto a USB-connected Android
# phone.
#
# Picks the newest release/vesc_ble_helper-*.apk (or an explicit version /
# path) and `adb install -r` so the existing install and its data survive.
# adb requires the same signing key and a same-or-higher versionCode, so build
# the APK with scripts/release.sh (it bumps the build number each run).
#
# Usage:
#   scripts/install_app.sh                  # newest release/vesc_ble_helper-*.apk
#   scripts/install_app.sh 1.0.2            # release/vesc_ble_helper-1.0.2.apk
#   scripts/install_app.sh path/to/app.apk  # explicit file
#   ANDROID_SERIAL=<serial> scripts/install_app.sh   # choose phone when >1
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REL="$ROOT/release"
PKG="dev.vescble.vesc_ble_helper"

# Locate adb: PATH first, then the usual macOS SDK / Homebrew spots.
ADB="$(command -v adb || true)"
if [[ -z "$ADB" ]]; then
    for c in "$HOME/Library/Android/sdk/platform-tools/adb" \
             /opt/homebrew/bin/adb /usr/local/bin/adb; do
        if [[ -x "$c" ]]; then ADB="$c"; break; fi
    done
fi
[[ -n "$ADB" ]] || { echo "install_app: adb not found — install Android platform-tools" >&2; exit 1; }

# Resolve which APK to install.
arg="${1:-}"
if [[ -n "$arg" && -f "$arg" ]]; then
    APK="$arg"                              # explicit path
elif [[ -n "$arg" ]]; then
    APK="$REL/vesc_ble_helper-$arg.apk"     # version number
else
    # Newest by semantic version (sort -V on the X.Y.Z in the filename).
    APK="$(ls -1 "$REL"/vesc_ble_helper-*.apk 2>/dev/null | sort -V | tail -1 || true)"
    [[ -n "$APK" ]] || { echo "install_app: no release/vesc_ble_helper-*.apk — run scripts/release.sh first" >&2; exit 1; }
fi
[[ -f "$APK" ]] || { echo "install_app: $APK not found" >&2; exit 1; }

# Require exactly one device, unless ANDROID_SERIAL pins a specific one (adb
# reads ANDROID_SERIAL itself, so we only need to gate the ambiguous case).
n="$("$ADB" devices | awk 'NR>1 && $2=="device"' | grep -c . || true)"
if [[ "$n" -eq 0 ]]; then
    echo "install_app: no device. Plug in the phone, enable USB debugging, accept the prompt." >&2
    "$ADB" devices >&2
    exit 1
fi
if [[ "$n" -gt 1 && -z "${ANDROID_SERIAL:-}" ]]; then
    echo "install_app: $n devices connected — pin one with ANDROID_SERIAL=<serial>:" >&2
    "$ADB" devices >&2
    exit 1
fi

target="${ANDROID_SERIAL:-$("$ADB" devices | awk 'NR>1 && $2=="device"{print $1; exit}')}"
echo "install_app: $(basename "$APK") -> $target"
if ! "$ADB" install -r "$APK"; then
    {
        echo
        echo "install_app: install failed. Common causes:"
        echo "  - signature mismatch (APK signed with a different key than what's on the phone)"
        echo "  - version downgrade (the installed build is newer)"
        echo "  Force-replace (LOSES app data):"
        echo "      $ADB uninstall $PKG && scripts/install_app.sh $arg"
    } >&2
    exit 1
fi
echo "install_app: done."
