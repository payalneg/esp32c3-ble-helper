#!/usr/bin/env bash
# One-shot release helper: bump versions, build the helper firmware, bundle it
# into the Flutter companion app, build the APK, and stage versioned artifacts.
# Same flow as esp32p4-android-auto's scripts/release.sh, single board.
#
# Steps:
#   1. bump version.txt (helper firmware) and app/pubspec.yaml (app)
#   2. idf.py reconfigure + build   (reconfigure so cmake picks up PROJECT_VER
#      from version.txt)
#   3. tools/sync_app_assets.sh     (fresh bin + lisp/main.lisp + version string
#                                    → app/assets/)
#   4. flutter build apk --release  (bundles the firmware for over-BLE OTA)
#   5. copy artifacts → release/esp32c3_ble_helper-<fw>.bin
#                     + release/esp32c3_ble_helper-<fw>-merged.bin (USB image)
#                     + release/vesc_ble_helper-<app>.apk
#
# Usage:
#   scripts/release.sh                 # patch-bump both fw and app
#   scripts/release.sh 1.0.1 1.0.1     # explicit fw + app versions
#
# Does NOT commit. After it finishes, review and commit version.txt,
# app/pubspec.yaml, release/*, and your code changes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# --- ensure idf.py / flutter are available ------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
        # export.sh derives the venv name from the python3 currently in PATH
        # (a conda/base shell shifts it and the lookup fails); when the
        # derived venv is absent, point it at the newest installed one.
        if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
            py_minor="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null || true)"
            if ! compgen -G "$HOME/.espressif/python_env/idf*_py${py_minor}_env" >/dev/null; then
                for env in "$HOME/.espressif/python_env"/idf*_env; do
                    [[ -d "$env" ]] && export IDF_PYTHON_ENV_PATH="$env"
                done
            fi
        fi
        # shellcheck disable=SC1091
        . "$IDF_PATH/export.sh" >/dev/null
    fi
fi
command -v idf.py  >/dev/null 2>&1 || { echo "release: idf.py not found — run '. \$IDF_PATH/export.sh' first" >&2; exit 1; }
command -v flutter >/dev/null 2>&1 || { echo "release: flutter not found in PATH" >&2; exit 1; }

# esptool (bundled with ESP-IDF) builds the single-file merged flash image used
# by release/flash.py. 'merge_bin' (underscore) works on esptool v4 (IDF) and v5.
if   command -v esptool    >/dev/null 2>&1; then ESPTOOL=(esptool)
elif command -v esptool.py >/dev/null 2>&1; then ESPTOOL=(esptool.py)
else ESPTOOL=(python -m esptool); fi

# --- resolve versions ---------------------------------------------------------
bump_patch() {  # 1.2.3 -> 1.2.4
    local v="$1" major minor patch
    major="${v%%.*}"; v="${v#*.}"; minor="${v%%.*}"; patch="${v#*.}"
    echo "${major}.${minor}.$((patch + 1))"
}

CUR_FW="$(head -n1 version.txt | tr -d '[:space:]')"
CUR_APP_FULL="$(grep -E '^version:' app/pubspec.yaml | awk '{print $2}')"
CUR_APP="${CUR_APP_FULL%%+*}"
CUR_BUILD="${CUR_APP_FULL##*+}"

NEW_FW="${1:-$(bump_patch "$CUR_FW")}"
NEW_APP="${2:-$(bump_patch "$CUR_APP")}"
NEW_BUILD=$((CUR_BUILD + 1))

echo "==> firmware : ${CUR_FW} -> ${NEW_FW}"
echo "==> app      : ${CUR_APP}+${CUR_BUILD} -> ${NEW_APP}+${NEW_BUILD}"

# --- bump version files -------------------------------------------------------
printf '%s\n' "$NEW_FW" > version.txt
# portable in-place edit (BSD + GNU sed both accept -i with a backup suffix)
sed -i.bak -E "s/^version:.*/version: ${NEW_APP}+${NEW_BUILD}/" app/pubspec.yaml
rm -f app/pubspec.yaml.bak

# --- build the firmware --------------------------------------------------------
echo "==> idf.py reconfigure + build"
idf.py reconfigure >/dev/null
idf.py build

# --- bundle into the app + build the APK --------------------------------------
echo "==> staging firmware + lisp into the Flutter app"
tools/sync_app_assets.sh

echo "==> flutter build apk"
( cd app && flutter pub get && flutter build apk --release )

APK="app/build/app/outputs/flutter-apk/app-release.apk"
[[ -f "$APK" ]] || APK="$(find app/build -name 'app-release.apk' -print -quit 2>/dev/null || true)"
[[ -n "$APK" && -f "$APK" ]] || { echo "release: built APK not found" >&2; exit 1; }

# --- stage versioned artifacts (keep only the latest of each kind) ------------
echo "==> staging release artifacts"
mkdir -p release
rm -f release/esp32c3_ble_helper-*.bin release/vesc_ble_helper-*.apk
cp build/esp32c3_ble_helper.bin "release/esp32c3_ble_helper-${NEW_FW}.bin"

# Single-file flasher image (bootloader + partition table + otadata + app),
# written at offset 0x0 by release/flash.py. Offsets/files come straight from
# flasher_args.json; no flash flags are passed, so the mode/freq/size baked
# into the bootloader header by the build are kept.
pairs=$(python3 -c '
import json,sys
fa=json.load(open(sys.argv[1]))["flash_files"]; bdir=sys.argv[2]
print(" ".join("%s %s/%s"%(a,bdir,f) for a,f in sorted(fa.items(),key=lambda kv:int(kv[0],16))))
' build/flasher_args.json build)
# shellcheck disable=SC2086  # $pairs intentionally word-splits into <addr> <file> args
"${ESPTOOL[@]}" --chip esp32c3 merge_bin \
    -o "release/esp32c3_ble_helper-${NEW_FW}-merged.bin" \
    $pairs >/dev/null

cp "$APK" "release/vesc_ble_helper-${NEW_APP}.apk"

echo
cat <<EOF
==> done.
    firmware : release/esp32c3_ble_helper-${NEW_FW}.bin           (over-BLE OTA, also bundled in the APK)
    flasher  : release/esp32c3_ble_helper-${NEW_FW}-merged.bin    (USB, via release/flash.command / flash.bat)
    apk      : release/vesc_ble_helper-${NEW_APP}.apk             (release/install.sh)

Review, then commit:
    git add -A version.txt app/pubspec.yaml release/
    git commit -m "release: fw ${NEW_FW} + app ${NEW_APP}"
EOF
