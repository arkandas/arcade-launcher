#!/bin/sh
# Copies what the menu shows from the cabinet's Pi into home/ of this project:
#   home/library.tsv   the game list that the Pi wrote, with the paths of its pictures made local
#   home/roms/         the pictures of the games in that list (box, screenshot, logo)
#   home/machine-info.txt  the facts about the machine that Settings shows, with
#                      home/machine-info.sh, which prints them in place of the Pi's script
#   home/strips/       the strip pictures of the console screens, which setup.sh builds on the Pi
# home/art and home/fonts point to the build folder of the project, which draws them.
#
#   tools/sync.sh                          asks pi@retropie.local
#   ARCADE_HOST=pi@10.0.0.7 tools/sync.sh  another address, or several separated by spaces
#
# ssh uses its usual keys; ARCADE_KEY names a key file of its own. When the Pi does not
# answer, home/ keeps its last copy.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
CABINET=${CABINET:-$HERE/..}
LOCAL=$HERE/home
PI_DIR=/opt/retropie/configs/all/arcade-launcher
ROMS=/home/pi/RetroPie/roms
KEY=${ARCADE_KEY:-}

mkdir -p "$LOCAL"
ln -sfn "$CABINET/build/launcher/art" "$LOCAL/art"
ln -sfn "$CABINET/build/launcher/fonts" "$LOCAL/fonts"
# An older version of this script linked home/strips to the build folder.
if [ -L "$LOCAL/strips" ]; then
    rm "$LOCAL/strips"
fi

# The Pi's host key stays in home/known_hosts under one name for all of its addresses.
SSH="ssh -o BatchMode=yes -o ConnectTimeout=5 -o LogLevel=ERROR -o HostKeyAlias=arcade-launcher-pi"
SSH="$SSH -o UserKnownHostsFile=$LOCAL/known_hosts -o StrictHostKeyChecking=accept-new"
if [ -n "$KEY" ] && [ -f "$KEY" ]; then
    SSH="$SSH -o IdentitiesOnly=yes -i $KEY"
fi

HOST=
CHANGED=
for candidate in ${ARCADE_HOST:-pi@retropie.local}; do
    if error=$($SSH "$candidate" true 2>&1); then
        HOST=$candidate
        break
    fi
    case "$error" in
        *"Host key verification failed"*) CHANGED=1 ;;
    esac
done
if [ -z "$HOST" ]; then
    # A new card in the Pi comes with new host keys.
    if [ -n "$CHANGED" ]; then
        echo "The Pi's host key differs from the one in home/known_hosts, as after a change of card." >&2
        echo "Remove home/known_hosts and run this again." >&2
    fi
    if [ -f "$LOCAL/library.tsv" ]; then
        echo "The Pi does not answer. The menu shows the copy of $(date -r "$LOCAL/library.tsv" '+%e %B %H:%M')."
        exit 0
    fi
    echo "The Pi does not answer, and home/ is still empty." >&2
    exit 1
fi

# the logos of RetroPie's theme, which tools/build_assets.py of the cabinet project draws from
if [ ! -d "$CABINET/build/theme/systems" ]; then
    mkdir -p "$CABINET/build/theme"
    rsync -a --timeout=30 -e "$SSH" "$HOST:/etc/emulationstation/themes/carbon-2021/art/systems" "$CABINET/build/theme/"
fi

# A copy that stalls on the network ends after 30 seconds without data and gets one more try.
copy() {
    rsync -a --timeout=30 -e "$SSH" "$@" || rsync -a --timeout=30 -e "$SSH" "$@"
}

$SSH "$HOST" "cat $PI_DIR/library.tsv" > "$LOCAL/library-pi.tsv"
tr '\t' '\n' < "$LOCAL/library-pi.tsv" | grep "^$ROMS/.*\.png$" | sed "s#^$ROMS/##" | sort -u > "$LOCAL/pictures.txt"
copy --files-from="$LOCAL/pictures.txt" "$HOST:$ROMS/" "$LOCAL/roms/"
copy --delete "$HOST:$PI_DIR/strips/" "$LOCAL/strips/"
sed "s#$ROMS/#$LOCAL/roms/#g" "$LOCAL/library-pi.tsv" > "$LOCAL/library.tsv"
# The facts about the machine that Settings shows, as the Pi reports them during this copy.
$SSH "$HOST" "sh $PI_DIR/machine-info.sh" > "$LOCAL/machine-info.txt" 2> /dev/null || true
printf '#!/bin/sh\n# The facts of the cabinet Pi that tools/sync.sh copied.\ncat "$(dirname "$0")/machine-info.txt"\n' > "$LOCAL/machine-info.sh"
rm -rf "$LOCAL/options"
echo "Copied from the Pi ($HOST): $(grep -c '^G' "$LOCAL/library.tsv") entries of games and $(wc -l < "$LOCAL/pictures.txt" | tr -d ' ') pictures."
