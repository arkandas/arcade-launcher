#!/bin/sh
# A screenshot of the running menu, for checks from a terminal: tools/shot.sh NAME
# Needs the menu started with LAUNCHER_DEV=1. Writes run/NAME.png.
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUN=$HERE/run
rm -f "$RUN/launcher-shot.ppm"
touch "$RUN/launcher-shot-request"
for i in $(seq 1 50); do
    [ ! -e "$RUN/launcher-shot-request" ] && [ -s "$RUN/launcher-shot.ppm" ] && break
    /bin/sleep 0.1
done
sips -s format png "$RUN/launcher-shot.ppm" --out "$RUN/$1.png" > /dev/null 2>&1 || echo "no screenshot"
