#!/bin/sh
# Configures a RetroPie card for the arcade launcher. setup.sh and tools/deploy.sh run it on
# the Pi as their last step. Manual run on the Pi: sh /opt/retropie/configs/all/arcade-launcher/install.sh
#
#   - builds the programs in frontend/ (log in /dev/shm/launcher-build.log)
#   - makes RetroPie's autostart.sh run arcade.sh in place of EmulationStation
#   - configures runcommand, RetroArch, the tty1 autologin and the kernel command line
#   - sets up the power button on pin 5
#   - installs the boot animation as a systemd service and in the initramfs
#   - restarts getty@tty1, which starts the new menu
#
# Repeated runs leave the card in the same state.
set -e

DIR=/opt/retropie/configs/all/arcade-launcher
CONFIGS=/opt/retropie/configs/all
CMDLINE=/boot/firmware/cmdline.txt
CONFIG_TXT=/boot/firmware/config.txt

# set_option FILE KEY VALUE writes KEY = "VALUE" into a RetroArch or runcommand settings
# file, in place of the line that sets KEY.
set_option() {
    if grep -q "^$2 *=" "$1" 2>/dev/null; then
        sed -i "s|^$2 *=.*|$2 = \"$3\"|" "$1"
    else
        echo "$2 = \"$3\"" >> "$1"
    fi
}

echo "   building the programs"
if ! make -C "$DIR/frontend" -j4 > /dev/shm/launcher-build.log 2>&1; then
    cat /dev/shm/launcher-build.log
    exit 1
fi

# RetroPie runs autostart.sh from the login shell on tty1.
cat > "$CONFIGS/autostart.sh" <<'EOF'
# Arcade launcher, written by pi/install.sh
bash /opt/retropie/configs/all/arcade-launcher/arcade.sh
EOF

# runcommand.cfg, also under Configuration / tools > runcommand in RetroPie-Setup:
#   disable_menu      skips the dialog that waits two seconds for a key before each launch,
#                     so the menu frame stays visible
#   disable_joystick  skips its joystick helper (4.6 s of a 7.7 s Game Boy launch on a
#                     Pi 3B+). The emulator chooser for a system without a default then
#                     needs a keyboard
for option in disable_menu disable_joystick; do
    set_option "$CONFIGS/runcommand.cfg" $option 1
done

# RetroArch settings of the launcher:
#   quit_press_twice                  the quit hotkey (Select+A) needs a second press
#   input_menu_toggle_gamepad_combo   Start+Select opens RetroArch's menu
#   savestate_auto_save, _auto_load   a game saves its state when it ends and continues from
#                                     there at its next start
#   savestate_thumbnail_enable        saved states carry a picture of the game
#   quick_menu_show_close_content     RetroArch's menu has the entry Close Content
#   menu_swap_ok_cancel_buttons       B confirms and A goes back in RetroArch's menu
#   input_playerN_analog_dpad_mode    the left analog stick moves the D-pad for players 1 and 2
# The other hotkeys sit in the controller presets that pi/make_library.py writes.
set_option "$CONFIGS/retroarch.cfg" quit_press_twice true
set_option "$CONFIGS/retroarch.cfg" input_menu_toggle_gamepad_combo 4
set_option "$CONFIGS/retroarch.cfg" savestate_auto_save true
set_option "$CONFIGS/retroarch.cfg" savestate_auto_load true
set_option "$CONFIGS/retroarch.cfg" savestate_thumbnail_enable true
set_option "$CONFIGS/retroarch.cfg" quick_menu_show_close_content true
set_option "$CONFIGS/retroarch.cfg" menu_swap_ok_cancel_buttons true
for mode in 1:1 2:1 3:0 4:0 5:0; do
    set_option "$CONFIGS/retroarch.cfg" "input_player${mode%%:*}_analog_dpad_mode" "${mode#*:}"
done
# MAME 2003 starts a game without its warning about imperfect emulation. PicoDrive gives both
# players a six-button pad. MAME 2003-Plus leaves button changes to RetroArch's remaps.
set_option "$CONFIGS/retroarch-core-options.cfg" mame2003_skip_warnings enabled
set_option "$CONFIGS/retroarch-core-options.cfg" picodrive_input1 "6 button pad"
set_option "$CONFIGS/retroarch-core-options.cfg" picodrive_input2 "6 button pad"
set_option "$CONFIGS/retroarch-core-options.cfg" mame2003-plus_mame_remapping disabled

# Picture settings of the games: 4:3 or the core's aspect, sharp pixels, overscan cut off,
# threaded video, yellow messages in the corner, 64 ms of audio buffer, games start without a
# shader. The overlay lines show bezels at 70 % opacity where a game or console has one.
while read -r key value; do
    set_option "$CONFIGS/retroarch.cfg" "$key" "$value"
done <<'EOF'
aspect_ratio_index 0
video_aspect_ratio -1.000000
video_aspect_ratio_auto true
video_force_aspect true
custom_viewport_width 1920
custom_viewport_height 1080
custom_viewport_x 0
custom_viewport_y 0
video_crop_overscan true
video_scale_integer false
video_smooth false
video_threaded true
video_vsync true
video_adaptive_vsync false
video_swap_interval 1
video_hard_sync false
video_hard_sync_frames 0
video_frame_delay 0
video_black_frame_insertion false
video_max_swapchain_images 3
video_refresh_rate 59.940060
video_rotation 0
video_allow_rotate true
video_force_srgb_disable false
video_shader_enable false
video_shader_delay 0
video_font_enable true
video_font_size 12.000000
video_message_color ffff00
video_message_pos_x 0.050000
video_message_pos_y 0.050000
video_msg_bgcolor_enable false
fps_show false
pause_nonactive true
audio_latency 64
input_overlay_enable true
input_overlay_enable_autopreferred true
input_overlay_opacity 0.700000
input_overlay_scale 1.000000
input_overlay_hide_in_menu true
input_overlay_auto_rotate false
input_overlay_show_physical_inputs false
EOF
# The handhelds stretch to 4:3. Game & Watch and the DS keep the aspect ratio of the core.
# A system's settings file exists once RetroPie has installed an emulator for the system.
for pair in gamegear:0 gb:0 gbc:0 ngp:0 ngpc:0 wonderswan:0 wonderswancolor:0 gameandwatch:22 nds:22; do
    file="/opt/retropie/configs/${pair%%:*}/retroarch.cfg"
    if [ -f "$file" ]; then
        set_option "$file" aspect_ratio_index "${pair#*:}"
    fi
done
# Genesis Plus GX gives both players of the Mega-CD a six-button pad (device 513).
if [ -f /opt/retropie/configs/segacd/retroarch.cfg ]; then
    set_option /opt/retropie/configs/segacd/retroarch.cfg input_libretro_device_p1 513
    set_option /opt/retropie/configs/segacd/retroarch.cfg input_libretro_device_p2 513
fi
# Select+Up and Select+Down step through the CRT shaders in shaders_glsl/crt (with
# ../stock.glsl) once that folder exists. tools/copy_library.py copies it from another card.
CRT_SHADERS="$CONFIGS/retroarch/shaders/shaders_glsl/crt"
if [ -d "$CRT_SHADERS" ]; then
    set_option "$CONFIGS/retroarch.cfg" video_shader_dir "$CRT_SHADERS"
fi

# .hushlogin silences the MOTD and the last-login line of the tty1 login.
touch /home/pi/.hushlogin
sudo mkdir -p /etc/systemd/system/getty@tty1.service.d
sudo tee /etc/systemd/system/getty@tty1.service.d/zz-launcher.conf > /dev/null <<'EOF'
# Arcade launcher, written by pi/install.sh: silent autologin of pi on tty1. The zz- prefix
# sorts this drop-in after RetroPie's autologin.conf, which it overrides.
# TTYVTDisallocate=no keeps systemd from clearing tty1, which would blank the boot
# animation for a moment.
[Service]
TTYVTDisallocate=no
ExecStart=
ExecStart=-/sbin/agetty --autologin pi --noclear --skip-login --noissue %I $TERM
EOF

# Kernel command line: quiet loglevel=2 hides boot messages, logo.nologo the kernel logo and
# vt.global_cursor_default=0 the cursor. Replacing console=tty1 with console=tty3 moves the
# boot lines that ignore the log level, such as the fsck result, to tty3 (Alt+F3 shows it
# when a boot fails). consoleblank=0 keeps the console, which shows the menu's frames between
# programs, from turning black. The usbhid.quirks entry splits a Xin-Mo USB encoder for two
# players into two controllers. The first change saves the previous line as
# cmdline.txt.before-launcher.
WORDS="quiet loglevel=2 logo.nologo vt.global_cursor_default=0 consoleblank=0 usbhid.quirks=0x16c0:0x05e1:0x040"
LINE=$(head -n 1 "$CMDLINE")
NEW=$(echo "$LINE" | sed 's/console=tty1\( \|$\)/console=tty3\1/')
for word in $WORDS; do
    case " $NEW " in
        *" $word "*) ;;
        *) NEW="$NEW $word" ;;
    esac
done
if [ "$NEW" != "$LINE" ]; then
    [ -f "$CMDLINE.before-launcher" ] || sudo cp "$CMDLINE" "$CMDLINE.before-launcher"
    echo "$NEW" | sudo tee "$CMDLINE" > /dev/null
    echo "   start options of the Pi changed, they count from the next start"
fi

# Power button between pin 5 (GPIO3) and ground: a press shuts the Pi down, and a press
# while it is off starts it again. The
# overlay turns the press into a power key, which systemd-logind answers with a shutdown.
if ! grep -q "^dtoverlay=gpio-shutdown" "$CONFIG_TXT"; then
    printf '\n# Arcade launcher: power button on pin 5\n[all]\ndtoverlay=gpio-shutdown\n' | sudo tee -a "$CONFIG_TXT" > /dev/null
    echo "   power button set up, it works from the next start"
fi

# Boot animation until the menu's first frame. The initramfs copy starts about two seconds
# after the kernel. This unit covers an initramfs without the program, since launcher-boot
# exits at once while another instance runs.
sudo tee /etc/systemd/system/launcher-boot.service > /dev/null <<EOF
# Arcade launcher, written by pi/install.sh
[Unit]
Description=Arcade launcher: the picture on the screen while the Pi starts
DefaultDependencies=no
ConditionPathExists=$DIR/frontend/launcher-boot

[Service]
Type=simple
ExecStart=$DIR/frontend/launcher-boot $DIR/art/boot

[Install]
WantedBy=sysinit.target
EOF
sudo systemctl daemon-reload
sudo systemctl --quiet enable launcher-boot.service

# RetroPie's splash video takes over the display mid-boot, covering the boot animation.
sudo systemctl disable asplashscreen.service > /dev/null 2>&1 || true

sudo tee /etc/initramfs-tools/hooks/launcher-boot > /dev/null <<'EOF'
#!/bin/sh
# Arcade launcher, written by pi/install.sh: adds launcher-boot, its pictures and vc4 to the
# initramfs.
PREREQ=""
prereqs() { echo "$PREREQ"; }
case "$1" in
    prereqs) prereqs; exit 0 ;;
esac
DIR=/opt/retropie/configs/all/arcade-launcher
[ -x "$DIR/frontend/launcher-boot" ] && [ -f "$DIR/art/boot/logo.pgm" ] || exit 0
mkdir -p "$DESTDIR/usr/bin" "$DESTDIR/usr/share/launcher"
cp "$DIR/frontend/launcher-boot" "$DESTDIR/usr/bin/launcher-boot"
cp "$DIR"/art/boot/* "$DESTDIR/usr/share/launcher/"
# vc4 replaces the firmware framebuffer, which blanks the display for a moment. From the card
# it loads about ten seconds into the boot, mid-animation. From here it loads before the
# first frame. It needs i2c-bcm2835 (HDMI EDID) and snd-soc-hdmi-codec (HDMI audio) in the
# image too. Without them it waits for the card, with the display dark for six seconds (measured).
. /usr/share/initramfs-tools/hook-functions
manual_add_modules vc4 i2c-bcm2835 snd-soc-hdmi-codec
EOF
sudo tee /etc/initramfs-tools/scripts/init-top/launcher-boot > /dev/null <<'EOF'
#!/bin/sh
# Arcade launcher, written by pi/install.sh: starts the boot animation while vc4 loads.
# launcher-boot waits for the vc4 framebuffer before its first frame.
PREREQ=""
prereqs() { echo "$PREREQ"; }
case "$1" in
    prereqs) prereqs; exit 0 ;;
esac
modprobe -q -a i2c-bcm2835 snd-soc-hdmi-codec vc4 > /dev/null 2>&1 &
[ -x /usr/bin/launcher-boot ] && /usr/bin/launcher-boot --early /usr/share/launcher < /dev/null > /dev/null 2>&1 &
exit 0
EOF
sudo chmod +x /etc/initramfs-tools/hooks/launcher-boot /etc/initramfs-tools/scripts/init-top/launcher-boot

# update-initramfs takes minutes on a Pi 3, so it runs only when launcher-boot, its pictures
# or the two files above change. Before the first rebuild, the loop below saves the current
# image as <image>.before-launcher (initramfs7.before-launcher on a Pi 3B+). If the Pi then
# fails to boot, copy that file over initramfs7 from another computer.
STAMP=/var/lib/launcher-boot.stamp
NOW=$(cat "$DIR/frontend/launcher-boot" "$DIR"/art/boot/* \
          /etc/initramfs-tools/hooks/launcher-boot /etc/initramfs-tools/scripts/init-top/launcher-boot | sha256sum | cut -d' ' -f1)
if [ "$NOW" != "$(cat "$STAMP" 2>/dev/null)" ]; then
    KERNEL=$(uname -r)
    for file in /boot/firmware/initramfs*; do
        case "$file" in
            *.before-launcher) continue ;;
        esac
        if [ ! -f "$file.before-launcher" ] && cmp -s "$file" "/boot/initrd.img-$KERNEL"; then
            sudo cp "$file" "$file.before-launcher"
        fi
    done
    echo "   putting the start picture into the first stage of the start (this takes a few minutes)"
    if ! sudo update-initramfs -u -k "$KERNEL" > /dev/shm/launcher-initramfs.log 2>&1; then
        cat /dev/shm/launcher-initramfs.log
        exit 1
    fi
    echo "$NOW" | sudo tee "$STAMP" > /dev/null
fi

echo "   restarting the screen"
sudo systemctl stop getty@tty1
sleep 1
sudo systemctl start getty@tty1
