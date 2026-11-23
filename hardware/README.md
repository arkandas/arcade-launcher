# Wiring

The controls of both players connect to the GPIO header of the Pi. The driver `mk_arcade_joystick_rpi` reads the pins with `map=1,2`. The button numbers in the tables match `controllers.json`.

## Player 1

| Control | Board pin | GPIO | Name in the driver | Button number | In the menu |
|---------|-----------|------|--------------------|---------------|-------------|
| Up      | 11 | 17 | down   | axis | up |
| Down    | 7  | 4  | up     | axis | down |
| Left    | 13 | 27 | left   | axis | left |
| Right   | 15 | 22 | right  | axis | right |
| Start   | 19 | 10 | START  | 7 | |
| Select  | 21 | 9  | SELECT | 6 | add to or remove from Recommended |
| A       | 18 | 24 | B      | 1 | |
| B       | 16 | 23 | TR     | 5 | |
| X       | 10 | 15 | X      | 2 | previous letter or console |
| Y       | 8  | 14 | TL     | 4 | open or play |
| L       | 12 | 18 | Y      | 3 | back |
| R       | 22 | 25 | A      | 0 | next letter or console |

## Player 2

| Control | Board pin | GPIO | Name in the driver | Button number | In the menu |
|---------|-----------|------|--------------------|---------------|-------------|
| Up      | 29 | 5  | down   | axis | up |
| Down    | 23 | 11 | up     | axis | down |
| Left    | 31 | 6  | left   | axis | left |
| Right   | 33 | 13 | right  | axis | right |
| Start   | 35 | 19 | START  | 7 | add to or remove from Recommended |
| A       | 38 | 20 | B      | 1 | |
| B       | 36 | 16 | TR     | 5 | |
| X       | 26 | 7  | X      | 2 | previous letter or console |
| Y       | 24 | 8  | TL     | 4 | open or play |
| L       | 32 | 12 | Y      | 3 | back |
| R       | 40 | 21 | A      | 0 | next letter or console |

The driver reads a select button for player 2 on board pin 37 (GPIO 26).

## Power button

A button between board pin 5 (GPIO 3) and ground on pin 6 switches the Pi off and on. `pi/install.sh` adds the `gpio-shutdown` overlay to `config.txt` for the button.
