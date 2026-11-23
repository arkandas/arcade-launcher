#!/bin/sh
# Deploys the arcade launcher to a RetroPie Pi over SSH.
#
#   tools/deploy.sh [user@host]        default: pi@retropie.local
#
#   ARCADE_SSH     ssh command, for example a wrapper that selects a key (default: ssh)
#   ARCADE_PYTHON  Python 3 with Pillow, numpy and scipy (default: python3)
#
# Writes build/ locally and /opt/retropie/configs/all/arcade-launcher on the Pi. The last step runs
# pi/install.sh there. Needs rsync and rsvg-convert. The Pi needs RetroPie with its
# mkarcadejoystick package (see README.md).
set -e

HOST=${1:-pi@retropie.local}
SSH=${ARCADE_SSH:-ssh}
PYTHON=${ARCADE_PYTHON:-python3}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PI_DIR=/opt/retropie/configs/all/arcade-launcher
THEME=/etc/emulationstation/themes/carbon-2021/art

mkdir -p "$ROOT/build/theme"

echo "1. console logos of RetroPie's theme, from the card"
rsync -a -e "$SSH" "$HOST:$THEME/systems" "$ROOT/build/theme/"

echo "2. pictures"
"$PYTHON" -I "$ROOT/tools/build_assets.py"

echo "3. copy to the card"
$SSH "$HOST" "mkdir -p $PI_DIR"
rsync -a -e "$SSH" "$ROOT/pi/" "$ROOT/consoles.json" "$ROOT/controllers.json" "$ROOT/recommended.json" \
    "$ROOT/build/launcher/brand.json" "$HOST:$PI_DIR/"
# The excludes protect the Pi's build outputs from --delete, so make rebuilds only what changed.
rsync -a --delete --exclude '*.o' --exclude launcher --exclude launcher-boot --exclude launcher-load -e "$SSH" "$ROOT/frontend/" "$HOST:$PI_DIR/frontend/"
rsync -a --delete -e "$SSH" "$ROOT/build/launcher/art" "$ROOT/build/launcher/fonts" "$HOST:$PI_DIR/"
if [ -d "$ROOT/build/strips" ]; then
    rsync -a --delete -e "$SSH" "$ROOT/build/strips" "$HOST:$PI_DIR/"
fi

echo "4. build, configuration and restart of the screen"
$SSH "$HOST" "sh $PI_DIR/install.sh"
