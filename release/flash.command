#!/usr/bin/env bash
# macOS flasher -- double-click in Finder. Runs flash.py from the same folder.
cd "$(dirname "$0")" || exit 1
PY=$(command -v python3 || command -v python)
if [ -z "$PY" ]; then
    echo "Python 3 not found. Install it: https://www.python.org/downloads/"
    read -r -p "Press Enter to exit..."
    exit 1
fi
exec "$PY" flash.py
