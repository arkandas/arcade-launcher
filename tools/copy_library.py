"""Copy the games and game settings of another RetroPie card to the card of the launcher.

    python3 -I tools/copy_library.py SOURCE_ROOT HOST [STEP ...]

SOURCE_ROOT is the root folder of the other card's files (an unpacked image of it), HOST the Pi, as
for tools/deploy.sh. ARCADE_SSH names another ssh command, as there. The steps, in this order
when none is named:

  roms         the ROM folders that consoles.json shows, with their pictures, gamelist.xml files
               and the arcade emulators' own folders, without saves, save states, high scores and
               memory files
  bios         RetroPie/BIOS without the saves and high scores that some emulators keep there
  emulators    the source card's emulator choices: each folder's default and the per-game choices of
               the arcade games that exist, with replacements for emulators that RetroPie dropped
  controllers  RetroArch's presets of the source card's controllers and the per-game button remaps
  bezels       RetroArch's overlay folder, each system's overlay lines and the per-game overlay
               settings, the latter under the name of the core that runs those games now
  shaders      the CRT shaders of shaders_glsl/crt
  layouts      the button remaps, emulator settings and per-game emulators of layouts/, which keep
               every game of the source card playing as it did there on the current emulators

A folder that runs as another system (runs_as in consoles.json) has no settings of its own on
the target card. Its games get the source folder's emulator one by one where that differs from the
other system's, and its bezel through a RetroArch override for its folder.

The emulators, controllers and bezels steps write into the settings files of the emulators, so
they belong after the emulators' installation (setup.sh). Every step can run again and sends only what
changed. The unpacked image marks names that differ only in case from a neighbour with
" (case conflict)" before the extension; those files get their own names back on the Pi.
"""
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SSH = shlex.split(os.environ.get('ARCADE_SSH', 'ssh'))
ROMS = '/home/pi/RetroPie/roms'
BIOS = '/home/pi/RetroPie/BIOS'
CONFIGS = '/opt/retropie/configs'
RETROARCH = CONFIGS + '/all/retroarch'
STEPS = ('roms', 'bios', 'emulators', 'controllers', 'bezels', 'shaders', 'layouts')

MARK = ' (case conflict)'
JUNK = ['.DS_Store', '._*']
# Saves stay behind: battery saves, save states, high scores, memory cards, the Sega CD's memory
# (.brm), Neo Geo Pocket saves (.flash), the memory folders of the arcade emulators and the high
# score and memory files of FB Alpha.
SAVES = ['*.srm', '*.state*', '*.hi', '*.nv', '*.fs', '*.sav', '*.rtc', '*.eep', '*.mcr', '*.mcd',
         '*.mpk', '*.brm', '*.flash', 'hi/', 'nvram/', 'memcard/', 'sta/', 'inp/', 'diff/']

ARCADE_FOLDERS = ('arcade', 'fba', 'mame-libretro', 'neogeo')
# A source card can run mame-mame4all's games with mame4all, which needs the graphics driver of
# Raspberry Pi OS before Bookworm. Its ROMs move to mame-libretro and run with lr-mame2000,
# which reads the same MAME 0.37b5 sets.
MOVED = ('mame-mame4all', 'mame-libretro')
# Emulators that RetroPie no longer offers on Bookworm, and what runs their games now.
REPLACED = {'mame4all': 'lr-mame2000', 'lr-fbalpha': 'lr-fbneo', 'pcsx-rearmed': 'lr-pcsx-rearmed'}

# RetroArch keeps per-game settings in a folder named after the core. Older core folder names and
# the RetroPie package whose core runs those games now; the new names come from the cores.
OLD_CORES = {
    'FB Alpha': 'lr-fbneo', 'FB Alpha 2012': 'lr-fbalpha2012', 'MAME 2000': 'lr-mame2000',
    'MAME 2003 (0.78)': 'lr-mame2003', 'MAME 2003-Plus': 'lr-mame2003-plus', 'MAME 2010': 'lr-mame2010',
    'MAME 2015': 'lr-mame2015', 'Genesis Plus GX': 'lr-genesis-plus-gx', 'PCSX-ReARMed': 'lr-pcsx-rearmed',
    'PicoDrive': 'lr-picodrive', 'FCEUmm': 'lr-fceumm', 'Nestopia': 'lr-nestopia', 'Gambatte': 'lr-gambatte',
    'Mednafen NeoPop': 'lr-beetle-ngp', 'Mednafen PCE Fast': 'lr-beetle-pce-fast',
    'Mednafen SuperGrafx': 'lr-beetle-supergrafx', 'Snes9x 2010': 'lr-snes9x2010', 'mGBA': 'lr-mgba',
}
# Core options of FB Alpha have other names in FinalBurn Neo, so only its .cfg files go over.
CONFIG_ONLY = ('FB Alpha',)

# The settings that a per-game or per-core file of the source card may carry over: bezel, picture
# shape, rotation, refresh rate and controller type. Some of those files are full dumps of an
# older RetroArch's settings, with drivers and folders that keep games from starting here.
GAME_SETTING = re.compile(r'^\s*(input_overlay\w*|aspect_ratio_index|video_aspect_ratio\w*|video_force_aspect|'
                          r'custom_viewport_\w+|video_scale_integer|video_smooth|video_crop_overscan|video_rotation|'
                          r'video_allow_rotate|video_shader_enable|video_refresh_rate|input_libretro_device_p\d+|'
                          r'input_player\d+_analog_dpad_mode)\s*=')

# Prints "package<TAB>core name" for the installed cores of the packages in argv.
CORE_NAMES = r'''
import ctypes, glob, sys
class Info(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char_p), ('version', ctypes.c_char_p), ('extensions', ctypes.c_char_p),
                ('need_fullpath', ctypes.c_bool), ('block_extract', ctypes.c_bool)]
for package in sys.argv[1:]:
    for core in glob.glob('/opt/retropie/libretrocores/%s/*_libretro.so' % package):
        try:
            info = Info()
            ctypes.CDLL(core).retro_get_system_info(ctypes.byref(info))
            print('%s\t%s' % (package, info.name.decode()))
        except (OSError, AttributeError) as error:
            print('# %s: %s' % (package, error))
'''

# Sets key = "value" lines from stdin in the file argv[1]. With argv[2] == "exists", a missing
# file gets reported in place of created. Values naming an emulator are checked against the
# emulators that the system's emulators.cfg lists (argv[3]).
SET_LINES = r'''
import os, re, sys
path, mode = sys.argv[1], sys.argv[2]
if not os.path.exists(path):
    if mode == 'exists':
        print('missing', path)
        sys.exit(0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    lines = []
else:
    lines = open(path, encoding='utf-8').read().splitlines()
known = None
if len(sys.argv) > 3:
    known = set(re.findall(r'^([\w.-]+)\s*=', open(sys.argv[3], encoding='utf-8').read(), re.M)) if os.path.exists(sys.argv[3]) else set()
new = {}
for line in sys.stdin.read().splitlines():
    key, value = line.split(' = ', 1)
    if known is not None and value.strip('"') not in known:
        print('not installed:', value, 'for', key)
        continue
    new[key] = value
kept = [line for line in lines if line.split('=', 1)[0].strip() not in new]
text = '\n'.join(kept + ['%s = %s' % pair for pair in new.items()]) + '\n'
with open(path + '.new', 'w', encoding='utf-8') as out:
    out.write(text)
os.replace(path + '.new', path)
print(len(new), 'settings in', path)
'''


def quoted(parts):
    return ' '.join(shlex.quote(part) for part in parts)


def rsync(source, target, excludes=(), options=()):
    """Copy source (a folder ending in / or a file) to target on the Pi.

    A copy that loses its connection starts again, up to five times. rsync then sends only what
    is missing and continues the file it was writing (--partial).
    """
    command = ['rsync', '-rlt', '--mkpath', '--partial', '--info=stats1', '-e', quoted(SSH)]
    command += list(options) + ['--exclude=' + pattern for pattern in excludes]
    command += [source, '%s:%s' % (HOST, target)]
    print('copy %s to %s' % (source, target), flush=True)
    for attempt in range(5):
        if subprocess.run(command).returncode == 0:
            return
        print('the copy broke off, try %d of 5 in 20 seconds' % (attempt + 2), flush=True)
        time.sleep(20)
    subprocess.run(command, check=True)


def remote(script, args=(), text=None):
    command = SSH + [HOST, 'python3 -I -c %s %s' % (shlex.quote(script), quoted(args))]
    return subprocess.run(command, input=text, capture_output=True, text=True, check=True).stdout


def set_lines(path, pairs, mode='create', emulators=None):
    if pairs:
        args = [path, mode] + ([emulators] if emulators else [])
        print(remote(SET_LINES, args, ''.join('%s = "%s"\n' % pair for pair in pairs)), end='', flush=True)


def own_names(old_top, new_top):
    """Copy the files that the unpacking marked with MARK under their own names."""
    for root, _, files in os.walk(old_top):
        for name in files:
            if MARK in name:
                relative = os.path.relpath(os.path.join(root, name), old_top)
                rsync(os.path.join(root, name), os.path.join(new_top, os.path.dirname(relative), name.replace(MARK, '')))


def settings(path):
    """Return the key = "value" pairs of a RetroArch or RetroPie settings file."""
    try:
        text = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        return {}
    return dict(re.findall(r'^\s*([\w.-]+)\s*=\s*"([^"]*)"', text, re.M))


def runs_as():
    """Return {ROM folder: the system it runs as} for the folders that consoles.json shows."""
    table = json.load(open(os.path.join(HERE, 'consoles.json'), encoding='utf-8'))
    entries = table['consoles'] if isinstance(table, dict) else table
    shown = {}
    for entry in entries:
        shown.setdefault(entry['system'], entry.get('runs_as', entry['system']))
        for folder in entry.get('also', []):
            shown.setdefault(folder, folder)
    return shown


def folders():
    """Return the ROM folders that consoles.json shows, in its order."""
    return list(runs_as())


def clean_name(name):
    """Return the key that runcommand.sh gives a game in emulators.cfg (its clean_name)."""
    return re.sub(r'[^a-zA-Z0-9_\-]', '', name.replace('/', '_'))


def old_default(folder):
    default = settings(os.path.join(SOURCE_ROOT, 'opt/retropie/configs', folder, 'emulators.cfg')).get('default', '')
    return REPLACED.get(default, default)


def game_files(folder):
    """Return the base names of the source folder's games: its files without saves and lists."""
    names = set()
    path = os.path.join(SOURCE_ROMS, folder)
    for name in os.listdir(path) if os.path.isdir(path) else []:
        base, ending = os.path.splitext(name)
        if os.path.isfile(os.path.join(path, name)) and '.state' not in name and ending.lower() not in (
                '.srm', '.hi', '.nv', '.sav', '.png', '.xml', '.txt', '.cfg'):
            names.add(base)
    return names


def roms():
    for folder in folders():
        if os.path.isdir(os.path.join(SOURCE_ROMS, folder)):
            rsync(os.path.join(SOURCE_ROMS, folder) + '/', '%s/%s/' % (ROMS, folder), JUNK + SAVES + ['*' + MARK + '*'])
            own_names(os.path.join(SOURCE_ROMS, folder), '%s/%s' % (ROMS, folder))
    old, new = MOVED
    rsync(os.path.join(SOURCE_ROMS, old) + '/', '%s/%s/' % (ROMS, new), options=['--include=*.zip', '--exclude=*'])


def bios():
    old = os.path.join(SOURCE_ROOT, 'home/pi/RetroPie/BIOS')
    rsync(old + '/', BIOS + '/', JUNK + SAVES + ['*' + MARK + '*'])
    own_names(old, BIOS)


def roms_in(folder):
    try:
        return {os.path.splitext(name)[0] for name in os.listdir(os.path.join(SOURCE_ROMS, folder))}
    except OSError:
        return set()


def emulators():
    old_configs = os.path.join(SOURCE_ROOT, 'opt/retropie/configs')
    for folder, system in runs_as().items():
        default = old_default(folder)
        if not default:
            continue
        if system == folder:
            path = '%s/%s/emulators.cfg' % (CONFIGS, folder)
            set_lines(path, [('default', default)], 'exists', path)
        elif default != old_default(system):
            pairs = [(clean_name('%s_%s' % (system, game)), default) for game in sorted(game_files(folder))]
            set_lines(CONFIGS + '/all/emulators.cfg', pairs, 'create', '%s/%s/emulators.cfg' % (CONFIGS, system))
    old, new = MOVED
    pins = {}
    for key, emulator in settings(os.path.join(old_configs, 'all/emulators.cfg')).items():
        folder, _, rom = key.partition('_')
        if folder in ARCADE_FOLDERS and rom in roms_in(folder):
            pins.setdefault(folder, []).append((key, REPLACED.get(emulator, emulator)))
    pins.setdefault(new, []).extend(('%s_%s' % (new, rom), 'lr-mame2000') for rom in sorted(roms_in(old)))
    # emulators.cfg in configs/all holds the choices of every folder. Each choice gets checked
    # against the emulators of its own folder.
    for folder, pairs in sorted(pins.items()):
        set_lines(CONFIGS + '/all/emulators.cfg', pairs, 'create', '%s/%s/emulators.cfg' % (CONFIGS, folder))


def package_cores(packages):
    """Return {package: core name} for the installed cores of these RetroPie packages."""
    lines = remote(CORE_NAMES, sorted(set(packages))).splitlines()
    return dict(line.split('\t', 1) for line in lines if '\t' in line)


def core_names():
    """Return {old core folder: core folder name on the Pi} for the installed cores."""
    names = package_cores(OLD_CORES.values())
    return {old: names[package] for old, package in OLD_CORES.items() if package in names}


def controllers():
    old = os.path.join(SOURCE_ROOT, 'opt/retropie/configs/all/retroarch/autoconfig')
    target = remote('import os; print(os.path.realpath("%s/all/retroarch-joypads"))' % CONFIGS).strip()
    # GPIOnext presets belong to another GPIO driver, which mk_arcade_joystick_rpi replaces here.
    rsync(old + '/', target + '/', JUNK + ['*' + MARK + '*', 'GPIOnext Joypad *'])
    own_names(old, target)
    # RetroPie keeps button remaps in each system's folder, one folder per core.
    names = core_names()
    old_configs = os.path.join(SOURCE_ROOT, 'opt/retropie/configs')
    for system in sorted(os.listdir(old_configs)):
        for core in sorted(names):
            folder = os.path.join(old_configs, system, core)
            if system != 'all' and os.path.isdir(folder) and any(n.endswith('.rmp') for n in os.listdir(folder)):
                rsync(folder + '/', '%s/%s/%s/' % (CONFIGS, system, names[core]), options=['--include=*.rmp', '--exclude=*'])


OVERLAY_ROOT = '/opt/retropie/configs/all/retroarch/overlay'
# Prefixes under which a card's settings name the overlay folder.
OVERLAY_PREFIXES = (OVERLAY_ROOT, '~/.config/retroarch/overlay', '/home/pi/.config/retroarch/overlay',
                    '/opt/retropie/emulators/retroarch/overlays')


def overlay_files(overlay):
    """Return {lower-case file name: path relative to the overlay folder} for the .cfg files of a
    card's overlay folder."""
    found = {}
    for top, _, files in os.walk(overlay):
        for name in files:
            if name.endswith('.cfg'):
                found.setdefault(name.lower(), os.path.relpath(os.path.join(top, name), overlay))
    return found


def repaired_overlay(lines, overlay, files):
    """Return a game's setting lines with a working bezel. A setting that names a missing overlay
    file gets the file of the nearest name: the same name in another folder of the overlay
    folder, the name without "full" (a colour overlay of a vector game, which also takes its
    opacity and input display along) or with "_" for "-". Without such a file the line goes."""
    target = None
    for line in lines:
        m = re.match(r'\s*input_overlay\s*=\s*"([^"]*)"', line)
        if m:
            target = m.group(1).strip()
    if target is None:
        return lines
    relative = next((target[len(prefix):].lstrip('/') for prefix in OVERLAY_PREFIXES if target.startswith(prefix)), None)
    if relative is not None and os.path.isfile(os.path.join(overlay, relative)):
        return lines
    base = os.path.basename(target).lower()
    full = base.endswith('full.cfg') and base[:-len('full.cfg')] + '.cfg' in files
    found = (files.get(base[:-len('full.cfg')] + '.cfg') if full else None) or files.get(base) or files.get(base.replace('-', '_'))
    kept = []
    for line in lines:
        if re.match(r'\s*input_overlay\s*=', line):
            if found:
                kept.append('input_overlay = "%s/%s"\n' % (OVERLAY_ROOT, found))
            continue
        if full and re.match(r'\s*input_overlay_(opacity|show_physical_inputs|hide_in_menu)\s*=', line):
            continue
        kept.append(line)
    return kept


def bezels():
    old_retroarch = os.path.join(SOURCE_ROOT, 'opt/retropie/configs/all/retroarch')
    files = overlay_files(os.path.join(old_retroarch, 'overlay'))
    rsync(os.path.join(old_retroarch, 'overlay') + '/', RETROARCH + '/overlay/', JUNK)
    # Source settings name the overlays through two links that RetroPie makes on every card.
    print(remote('import os\nfor path in ("/opt/retropie/emulators/retroarch/overlays", "/home/pi/.config/retroarch/overlay"):\n'
                 '    print(path, "found" if os.path.isdir(path) else "MISSING")'), end='')
    old_configs = os.path.join(SOURCE_ROOT, 'opt/retropie/configs')
    borrowed = {folder: old_default(folder) for folder, system in runs_as().items() if system != folder}
    cores = package_cores(filter(None, borrowed.values()))
    for folder, system in runs_as().items():
        old = settings(os.path.join(old_configs, folder, 'retroarch.cfg'))
        pairs = [(key, old[key]) for key in ('input_overlay_enable', 'input_overlay') if key in old]
        if system == folder:
            set_lines('%s/%s/retroarch.cfg' % (CONFIGS, folder), pairs, 'exists')
        elif pairs and borrowed[folder] in cores:
            # RetroArch reads config/<core>/<folder>.cfg for the games in a folder of that name.
            set_lines('%s/config/%s/%s.cfg' % (RETROARCH, cores[borrowed[folder]], folder), pairs)
        elif pairs:
            print('bezel left out, core not installed:', folder, borrowed[folder])
    names = core_names()
    with tempfile.TemporaryDirectory() as staging:
        for core in sorted(names):
            folder = os.path.join(old_retroarch, 'config', core)
            if not os.path.isdir(folder):
                continue
            out = os.path.join(staging, names[core])
            os.makedirs(out)
            for name in sorted(os.listdir(folder)):
                base, ending = os.path.splitext(name)
                # The settings of the whole core carry its name, which changed for some cores.
                own = (names[core] if base == core else base) + ending
                if ending == '.cfg':
                    kept = [line for line in open(os.path.join(folder, name), encoding='utf-8', errors='replace')
                            if GAME_SETTING.match(line)]
                    kept = repaired_overlay(kept, os.path.join(old_retroarch, 'overlay'), files)
                    with open(os.path.join(out, own), 'w', encoding='utf-8') as settings_file:
                        settings_file.write(''.join(kept) or '# The source card\'s settings here held nothing for this game.\n')
                elif ending == '.opt' and core not in CONFIG_ONLY:
                    shutil.copyfile(os.path.join(folder, name), os.path.join(out, own))
            rsync(out + '/', '%s/config/%s/' % (RETROARCH, names[core]), options=['--checksum'])
    missing = sorted(set(OLD_CORES) - set(names))
    if missing:
        print('per-game settings left out, core not installed:', ', '.join(missing))


def shaders():
    old = os.path.join(SOURCE_ROOT, 'opt/retropie/configs/all/retroarch/shaders/shaders_glsl')
    rsync(os.path.join(old, 'crt') + '/', RETROARCH + '/shaders/shaders_glsl/crt/', JUNK)
    # The CRT presets name ../stock.glsl for their plain passes.
    rsync(os.path.join(old, 'stock.glsl'), RETROARCH + '/shaders/shaders_glsl/stock.glsl')


def layouts():
    """Install layouts/: what keeps the games of the source card the same on current emulators.

    layouts/configs mirrors /opt/retropie/configs with button remaps and game option files.
    layouts/roms holds a link to the Neo Geo BIOS for the arcade folder, where MAME 2016 looks
    for it. layouts/options.cfg holds values for the shared core options file.
    layouts/emulators.cfg names the emulator of each game that its old emulator's successor
    cannot start, and layouts/systems.cfg the emulators a folder needs for those games.
    """
    folder = os.path.join(HERE, 'layouts')
    rsync(os.path.join(folder, 'configs') + '/', CONFIGS + '/')
    rsync(os.path.join(folder, 'roms') + '/', ROMS + '/')
    set_lines(CONFIGS + '/all/retroarch-core-options.cfg', sorted(settings(os.path.join(folder, 'options.cfg')).items()))
    for key, command in sorted(settings(os.path.join(folder, 'systems.cfg')).items()):
        system, emulator = key.split('.', 1)
        set_lines('%s/%s/emulators.cfg' % (CONFIGS, system), [(emulator, command)])
    choices = {}
    for key, emulator in settings(os.path.join(folder, 'emulators.cfg')).items():
        choices.setdefault(key.split('_', 1)[0], []).append((key, emulator))
    for system, pairs in sorted(choices.items()):
        set_lines(CONFIGS + '/all/emulators.cfg', pairs, 'create', '%s/%s/emulators.cfg' % (CONFIGS, system))


def main():
    global SOURCE_ROOT, SOURCE_ROMS, HOST
    if len(sys.argv) < 3 or any(step not in STEPS for step in sys.argv[3:]):
        sys.exit('usage: copy_library.py SOURCE_ROOT HOST [%s ...]' % ' | '.join(STEPS))
    SOURCE_ROOT, HOST = sys.argv[1], sys.argv[2]
    SOURCE_ROMS = os.path.join(SOURCE_ROOT, 'home/pi/RetroPie/roms')
    for step in sys.argv[3:] or STEPS:
        print('== %s' % step, flush=True)
        globals()[step]()


if __name__ == '__main__':
    main()
