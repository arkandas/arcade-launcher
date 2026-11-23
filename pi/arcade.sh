#!/bin/bash
# Runs the menu of the arcade launcher on tty1 in a loop. RetroPie's autostart.sh, written by
# install.sh, starts it after the autologin. Restart: sudo systemctl restart getty@tty1
#
# Before it exits, the menu writes a request to /dev/shm/launcher-action: a verb (game, tool,
# rescan, restart, shutdown or terminal) and up to two argument lines. The loop carries out
# the request before it restarts the menu. Menu stderr goes to /dev/shm/launcher.err.
#
# The menu also leaves its last frame in /dev/fb0. The kernel shows that framebuffer between
# DRM masters, so the frame stays visible until the emulator or tool draws. For a game that
# frame is the loading screen. Once the emulator draws, launcher-load replaces it with the
# game list frame ($PICTURE), which the display shows when the game ends. For a tool and a new
# look for games it is the wait screen, whose bar launcher-load keeps moving until the tool or
# the next menu draws.

DIR=/opt/retropie/configs/all/arcade-launcher
MENU=$DIR/frontend/launcher
ACTION=/dev/shm/launcher-action
PICTURE=/dev/shm/launcher-frame.fb
MESSAGES=/dev/shm/launcher.err
RUNCOMMAND=/opt/retropie/supplementary/runcommand/runcommand.sh
RETROPIE_SETUP=/home/pi/RetroPie-Setup/retropie_packages.sh
TOOLS=/home/pi/RetroPie/retropiemenu

hide_terminal() {
    stty -echo 2>/dev/null
    printf '\033[?25l'                    # cursor off
}

# Clearing the terminal also clears /dev/fb0, including any frame the menu left there.
empty_terminal() {
    printf '\033[2J\033[H'
}

# Restores tty1 for interactive use.
show_terminal() {
    stty sane 2>/dev/null
    cat /dev/zero > /dev/fb0 2>/dev/null
    printf '\033[?25h\033[2J\033[H'
}

# The terminal only draws whole text rows, so the strip below the last row would keep
# showing the menu frame under a RetroPie tool.
clear_below_text() {
    local rows height stride used
    rows=$(stty size 2>/dev/null | cut -d' ' -f1)
    height=$(cut -d, -f2 /sys/class/graphics/fb0/virtual_size 2>/dev/null)
    stride=$(cat /sys/class/graphics/fb0/stride 2>/dev/null)
    [ -n "$rows" ] && [ -n "$height" ] && [ -n "$stride" ] && [ "$rows" -gt 0 ] || return
    used=$((rows * (height / rows)))
    [ "$used" -lt "$height" ] && dd if=/dev/zero of=/dev/fb0 bs="$stride" seek="$used" count=$((height - used)) conv=notrunc 2>/dev/null
}

look_for_games() {
    python3 -I "$DIR/make_library.py" > /dev/shm/launcher-library.log 2>&1
}

# Prints the program that made the picture on the screen: [fbcon] for the console, which shows
# the menu's frames between programs, or the emulator. It reads which framebuffer the active
# plane shows from debugfs, then who allocated that framebuffer.
screen_owner() {
    sudo -n cat /sys/kernel/debug/dri/0/state /sys/kernel/debug/dri/0/framebuffer 2>/dev/null | awk '
        /^plane\[/ { in_plane = 1; on = 0; next }
        /^[a-z]+\[/ { in_plane = 0 }
        in_plane && /crtc=/ { on = $0 !~ /\(null\)/ }
        in_plane && on && /fb=/ && shown == "" { sub(/.*fb=/, ""); if ($0 != "0") shown = $0 }
        /^framebuffer\[/ { id = $0; gsub(/[^0-9]/, "", id) }
        /allocated by/ && id == shown { sub(/.*allocated by = /, ""); print; exit }'
}

# runcommand.sh does not report whether the emulator started. The watcher creates
# launcher-game-drew once the screen shows a picture of another program than the console, the
# menu or kmsprint-rp, a short-lived runcommand helper. Becoming the display's master is too
# early: an emulator does that up to a second before its first frame, and launcher-load
# would put the game list on the screen meanwhile. Without debugfs access, every launch
# counts as started.
watch_for_emulator() {
    rm -f /dev/shm/launcher-game-drew
    if [ -z "$(screen_owner)" ]; then
        touch /dev/shm/launcher-game-drew
        return
    fi
    while sleep 0.2; do
        case "$(screen_owner)" in
            '' | '[fbcon]' | launcher | launcher-* | kmsprint-rp) ;;
            *)
                touch /dev/shm/launcher-game-drew
                return
                ;;
        esac
    done
}

hide_terminal
# After power-on, the menu continues from the boot animation frame in /dev/fb0. It creates
# launcher-booted on its first frame, so only later runs clear the terminal.
[ -f /dev/shm/launcher-booted ] && empty_terminal

# Keep kernel warnings, such as undervoltage, off tty1. dmesg still records them.
sudo -n dmesg -n 1 2>/dev/null

look_for_games

# Development aids (README.md): touch /dev/shm/launcher-dev, then restart getty@tty1.
[ -f /dev/shm/launcher-dev ] && export LAUNCHER_DEV=1

failures=0
while true; do
    rm -f "$ACTION" "$PICTURE"
    "$MENU" 2>> "$MESSAGES"
    if [ ! -f "$ACTION" ]; then
        # The menu crashed or failed to start.
        failures=$((failures + 1))
        if [ "$failures" -ge 3 ]; then
            show_terminal
            echo "The arcade launcher does not start. Its last messages:"
            tail -n 20 "$MESSAGES"
            exit 1
        fi
        sleep 1
        continue
    fi
    failures=0
    { read -r what; read -r first; read -r second; } < "$ACTION"
    case "$what" in
        game)
            # first: system, second: ROM path. runcommand.sh output would print over the frame.
            watch_for_emulator &
            watcher=$!
            "$RUNCOMMAND" 0 _SYS_ "$first" "$second" > /dev/null 2>&1
            kill "$watcher" 2>/dev/null
            # launcher-load also exits once its state file is gone, which covers a failed start.
            rm -f /dev/shm/launcher-load
            # The emulator exited before it drew, for example because of incomplete game files.
            # The display then still shows the loading screen, so the game list frame goes back.
            if [ ! -f /dev/shm/launcher-game-drew ]; then
                echo "THIS GAME DID NOT START" > /dev/shm/launcher-note
                [ -f "$PICTURE" ] && cat "$PICTURE" > /dev/fb0 2>/dev/null
            fi
            ;;
        tool)
            # first: a .rp file in ~/RetroPie/retropiemenu. The cursor stays hidden, as it does
            # when EmulationStation opens these tools.
            stty sane 2>/dev/null
            clear_below_text
            sudo "$RETROPIE_SETUP" retropiemenu launch "$TOOLS/$first" < /dev/tty > /dev/tty
            hide_terminal
            empty_terminal
            # The menu saved its wait screen before the tool. It shows again, with its bar
            # moving, until the menu draws and removes the bar's state file.
            if [ -f "$PICTURE" ]; then
                cat "$PICTURE" > /dev/fb0 2>/dev/null
                [ -f /dev/shm/launcher-load ] && "$DIR/frontend/launcher-load" &
            fi
            ;;
        rescan)
            look_for_games
            # The menu shows the first log line, the summary of make_library.py.
            head -n 1 /dev/shm/launcher-library.log | tr '[:lower:]' '[:upper:]' > /dev/shm/launcher-note
            ;;
        # Both power actions sleep, which keeps the loop from restarting the menu during shutdown.
        restart)
            sudo systemctl reboot --no-wall
            sleep 60
            ;;
        shutdown)
            sudo systemctl poweroff --no-wall
            sleep 60
            ;;
        terminal)
            # F4 on a keyboard. The login shell then takes over tty1. Typing exit there makes
            # the autologin start the menu again.
            show_terminal
            exit 0
            ;;
    esac
done
