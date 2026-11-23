#!/bin/sh
# The menu of the arcade launcher in a window on this Mac: the menu program of the cabinet
# project, built for the Mac, with the game list and the pictures of the cabinet's Pi.
#
#   ./run.sh
#
# The arrows move, Enter is A, Esc is B. Closing the window ends it. Games, RetroPie's tools
# and the animations of the start run on the cabinet only. When the menu asks for one of
# them, it comes back at once, and this terminal names what it asked for.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CABINET=${CABINET:-$HERE/..}
export CABINET
RUN=$HERE/run                     # what the Pi keeps in /dev/shm (the build names this folder)

# The pictures come first, because they also write frontend/brand.h, which the build needs.
if [ ! -d "$CABINET/build/launcher/art" ] || [ ! -f "$CABINET/frontend/brand.h" ]; then
    make -s -C "$HERE" art
fi
make -s -C "$HERE"
sh "$HERE/tools/sync.sh"

# a fresh run/ makes the menu start as after power-on
rm -rf "$RUN"
mkdir -p "$RUN"
export LAUNCHER_HOME=$HERE/home
# a chosen game shows its loading picture as long as a game takes to start on the Pi (3.4 s measured)
export LAUNCHER_LOAD_DEMO=${LAUNCHER_LOAD_DEMO:-3.4}
while true; do
    rm -f "$RUN/launcher-action"
    "$HERE/build/launcher" 2>> "$RUN/launcher.err" || true
    [ -f "$RUN/launcher-action" ] || break             # the window was closed
    what=$(sed -n 1p "$RUN/launcher-action")
    case "$what" in
        game) echo "On the cabinet this starts $(basename "$(sed -n 3p "$RUN/launcher-action")")" ;;
        tool) echo "On the cabinet this opens RetroPie's tool $(sed -n 2p "$RUN/launcher-action")" ;;
        rescan) sh "$HERE/tools/sync.sh" ;;
        restart|shutdown|terminal) break ;;
    esac
done
