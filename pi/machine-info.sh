#!/bin/sh
# Prints what the Settings screen of the menu shows about this machine, one fact per line:
#
#   address 192.168.1.20          the IPv4 address of the default route's interface
#   link wifi                     wifi or ethernet
#   network Office                the Wi-Fi network's name
#   signal 77                     Wi-Fi link quality in percent
#   bluetooth on                  on or off
#   device 8BitDo SN30 Pro+       one line per connected Bluetooth device
#   temperature 48                CPU temperature in degrees Celsius
#   clock 1400                    CPU clock in MHz
#   memory 266 921                memory in use and in total, in MB
#   card 23412345 29876543        free and total space of the card, in KB
#   uptime 8617                   seconds since the start
#   power 0x50000                 the flags of vcgencmd get_throttled
#
# The menu runs it on a helper thread every three seconds while Settings shows. A fact that
# this machine lacks has no line.
device=$(ip -4 route show default 2>/dev/null | awk '{ print $5; exit }')
if [ -n "$device" ]; then
    ip -4 -o addr show dev "$device" | awk '{ split($4, a, "/"); print "address", a[1]; exit }'
    case "$device" in
        wl*)
            echo "link wifi"
            connection=$(nmcli -t -g GENERAL.CONNECTION device show "$device" 2>/dev/null)
            [ -n "$connection" ] && nmcli -t -g 802-11-wireless.ssid connection show "$connection" 2>/dev/null |
                sed 's/^/network /'
            awk -v d="$device:" '$1 == d { printf "signal %d\n", $3 * 100 / 70 }' /proc/net/wireless
            ;;
        *)
            echo "link ethernet"
            ;;
    esac
fi
if bluetoothctl show 2>/dev/null | grep -q "Powered: yes"; then
    echo "bluetooth on"
    bluetoothctl devices Connected 2>/dev/null | cut -d' ' -f3- | sed 's/^/device /'
else
    echo "bluetooth off"
fi
awk '{ printf "temperature %d\n", $1 / 1000 + 0.5 }' /sys/class/thermal/thermal_zone0/temp 2>/dev/null
awk '{ printf "clock %d\n", $1 / 1000 }' /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null
awk '/^MemTotal:/ { t = $2 } /^MemAvailable:/ { a = $2 } END { if (t) printf "memory %d %d\n", (t - a) / 1024, t / 1024 }' /proc/meminfo
df -Pk "$HOME" | awk 'NR == 2 { print "card", $4, $2 }'
cut -d. -f1 /proc/uptime | sed 's/^/uptime /'
vcgencmd get_throttled 2>/dev/null | sed 's/^throttled=/power /'
