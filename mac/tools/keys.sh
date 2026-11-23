#!/bin/sh
# Presses for the running menu, for checks from a terminal: tools/keys.sh up up a
# Words: up down left right a b select. Needs the menu started with LAUNCHER_DEV=1.
HERE=$(cd "$(dirname "$0")/.." && pwd)
echo "$*" > "$HERE/run/launcher-dev-keys.tmp" && mv "$HERE/run/launcher-dev-keys.tmp" "$HERE/run/launcher-dev-keys"
