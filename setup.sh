#!/bin/bash
# Installs the arcade launcher on a Raspberry Pi with RetroPie 4.8.12, from a clone of this
# repository on the Pi. Run it as the user pi in the folder of the clone:
#
#   ./setup.sh             everything
#   ./setup.sh --no-gpio   everything except the driver for sticks wired to the GPIO pins
#
# It asks sudo for the system steps and needs the internet for the packages, the emulators and
# the strip screenshots from libretro-thumbnails. A second run, for example after git pull or
# after copying games, installs the launcher again and skips the packages, emulators and driver
# that are in place. README.md describes the whole installation.
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
PI_DIR=/opt/retropie/configs/all/arcade-launcher
SETUP=/home/pi/RetroPie-Setup
THEME=/etc/emulationstation/themes/carbon-2021/art/systems
ROMS=/home/pi/RetroPie/roms
LOG=/dev/shm/launcher-setup.log

GPIO=1
for argument in "$@"; do
    case "$argument" in
        --no-gpio) GPIO=0 ;;
        *) echo "usage: ./setup.sh [--no-gpio]"; exit 2 ;;
    esac
done
if [ ! -x "$SETUP/retropie_packages.sh" ]; then
    echo "$SETUP/retropie_packages.sh is missing. setup.sh runs on a RetroPie card as the user pi."
    exit 1
fi

# packages ID [MODE...] runs a RetroPie-Setup package step and shows its log when it fails.
packages() {
    if ! sudo "$SETUP/retropie_packages.sh" "$@" > "$LOG" 2>&1; then
        cat "$LOG"
        echo "retropie_packages.sh $* failed"
        exit 1
    fi
}

echo "1. Pillow, numpy, scipy and rsvg-convert for the pictures and sounds of the menu"
missing=
for package in python3-pil python3-numpy python3-scipy librsvg2-bin; do
    dpkg -s "$package" > /dev/null 2>&1 || missing="$missing $package"
done
if [ -n "$missing" ]; then
    sudo apt-get update -q
    sudo apt-get install -y -q $missing
fi

# The RetroPie image lacks these emulators:
#   lr-mame2000, lr-mame2010, lr-mame2015, lr-mame2016, lr-fbalpha2012  arcade sets of those
#                                     versions, which tools/copy_library.py pins per game
#   lr-mame2003-plus                  arcade sets of MAME 2003-Plus
#   lr-gw                             Game & Watch
#   lr-beetle-wswan                   WonderSwan and WonderSwan Color
#   lr-stella                         Atari 2600
# _binary_ installs RetroPie's ready-made package, a few seconds each, where a build from
# source takes up to half an hour per emulator on a Pi 3B+.
echo "2. emulators beyond those of the RetroPie image"
for package in lr-mame2000 lr-mame2003-plus lr-mame2010 lr-mame2015 lr-mame2016 lr-fbalpha2012 \
               lr-gw lr-beetle-wswan lr-stella; do
    if [ ! -d "/opt/retropie/libretrocores/$package" ]; then
        echo "   $package"
        packages "$package" _binary_
    fi
done

# A folder without a default emulator makes RetroPie's runcommand ask for one at the first
# game, in a dialog that needs a keyboard. These folders get a default emulator when they
# have none.
echo "3. default emulators of the folders that have none"
for pair in arcade:lr-fbneo fba:lr-fbneo neogeo:lr-fbneo sega32x:lr-picodrive; do
    file="/opt/retropie/configs/${pair%%:*}/emulators.cfg"
    if [ -f "$file" ] && ! grep -q '^default *=' "$file"; then
        echo "   ${pair%%:*}: ${pair#*:}"
        echo "default = \"${pair#*:}\"" >> "$file"
    fi
done

# The driver mk_arcade_joystick_rpi reads the two sticks and their buttons from the GPIO pins
# (hardware/README.md). Its source predates kernel 6.12, which needs <linux/of.h> included
# before it builds. map=1,2 gives player 1 and player 2 the pins of the cabinet. modinfo
# lives in /sbin, which the PATH of the user pi leaves out.
echo "4. driver of the sticks on the GPIO pins"
if [ "$GPIO" = 0 ]; then
    echo "   left out (--no-gpio)"
elif /sbin/modinfo mk_arcade_joystick_rpi > /dev/null 2>&1; then
    echo "   in place"
else
    packages mkarcadejoystick depends
    packages mkarcadejoystick sources
    source=/opt/retropie/supplementary/mkarcadejoystick/mk_arcade_joystick_rpi.c
    grep -q "linux/of.h" "$source" || sudo sed -i '0,/^#include/s//#include <linux\/of.h>\n#include/' "$source"
    echo "options mk_arcade_joystick_rpi map=1,2" | sudo tee /etc/modprobe.d/mk_arcade_joystick_rpi.conf > /dev/null
    packages mkarcadejoystick build
    packages mkarcadejoystick configure
fi

echo "5. pictures, sounds and fonts of the menu"
mkdir -p "$ROOT/build/theme"
rm -rf "$ROOT/build/theme/systems"
cp -r "$THEME" "$ROOT/build/theme/systems"
python3 -I "$ROOT/tools/build_assets.py"

echo "6. strip pictures of the console screens"
python3 -I "$ROOT/tools/make_strips.py" "$ROMS"

echo "7. the launcher in $PI_DIR"
mkdir -p "$PI_DIR"
rsync -a "$ROOT/pi/" "$ROOT/consoles.json" "$ROOT/controllers.json" "$ROOT/recommended.json" \
    "$ROOT/build/launcher/brand.json" "$PI_DIR/"
# The excludes keep the programs built in place from --delete, so make rebuilds only what changed.
rsync -a --delete --exclude '*.o' --exclude launcher --exclude launcher-boot --exclude launcher-load \
    "$ROOT/frontend/" "$PI_DIR/frontend/"
rsync -a --delete "$ROOT/build/launcher/art" "$ROOT/build/launcher/fonts" "$ROOT/build/strips" "$PI_DIR/"
sh "$PI_DIR/install.sh"
