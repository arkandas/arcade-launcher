"""Download boxes and screenshots from libretro-thumbnails for games that have none.

    python3 -I tools/fill_pictures.py [LAUNCHER_DIR]

Runs on the Pi. LAUNCHER_DIR is the folder of the installed launcher, by default
/opt/retropie/configs/all/arcade-launcher. The script reads the game list library.tsv there
and, for every game without a box or a screenshot, looks for the picture on
libretro-thumbnails under the name of the game file. Arcade sets go by their titles in the game
lists of lr-fbneo and lr-mame2003 (cache/arcade-names.tsv). A picture found goes next to the
game, into boxart/ or snap/, under the name of the game file. Look for new games in Settings
then shows the new pictures.

Requires network access. Pictures already in place stay untouched.
"""
import concurrent.futures
import os
import re
import sys
import urllib.parse
import urllib.request

LAUNCHER_DIR = sys.argv[1] if len(sys.argv) > 1 else '/opt/retropie/configs/all/arcade-launcher'
URL = 'https://raw.githubusercontent.com/libretro-thumbnails/%s/master/%s/%s'
FOLDERS = {'box': ('Named_Boxarts', 'boxart'), 'snap': ('Named_Snaps', 'snap')}
ARCADE = ['FBNeo_-_Arcade_Games', 'MAME']
# The libretro-thumbnails repository of each console in consoles.json.
REPOSITORIES = {
    'arcade': ARCADE, 'neogeo': ARCADE,
    'nes': ['Nintendo_-_Nintendo_Entertainment_System'], 'famicom': ['Nintendo_-_Nintendo_Entertainment_System'],
    'fds': ['Nintendo_-_Family_Computer_Disk_System'], 'gameandwatch': ['Handheld_Electronic_Game'],
    'gb': ['Nintendo_-_Game_Boy'], 'gbc': ['Nintendo_-_Game_Boy_Color'], 'gba': ['Nintendo_-_Game_Boy_Advance'],
    'snes': ['Nintendo_-_Super_Nintendo_Entertainment_System'],
    'sfc': ['Nintendo_-_Super_Nintendo_Entertainment_System'],
    'sg-1000': ['Sega_-_SG-1000'], 'markiii': ['Sega_-_Master_System_-_Mark_III'],
    'mastersystem': ['Sega_-_Master_System_-_Mark_III'], 'megadrive': ['Sega_-_Mega_Drive_-_Genesis'],
    'megadrive-japan': ['Sega_-_Mega_Drive_-_Genesis'], 'segacd': ['Sega_-_Mega-CD_-_Sega_CD'],
    'sega32x': ['Sega_-_32X'], 'gamegear': ['Sega_-_Game_Gear'],
    'tg16': ['NEC_-_PC_Engine_-_TurboGrafx_16'], 'pcengine': ['NEC_-_PC_Engine_-_TurboGrafx_16'],
    'supergrafx': ['NEC_-_PC_Engine_SuperGrafx'], 'psx': ['Sony_-_PlayStation'],
    'ngp': ['SNK_-_Neo_Geo_Pocket'], 'ngpc': ['SNK_-_Neo_Geo_Pocket_Color'],
    'wonderswan': ['Bandai_-_WonderSwan'], 'wonderswancolor': ['Bandai_-_WonderSwan_Color'],
    'atari2600': ['Atari_-_2600'], 'atari7800': ['Atari_-_7800'],
}


def file_name(name):
    """libretro-thumbnails replaces these characters in its file names."""
    return re.sub(r'[&*/:`<>?\\|"]', '_', name) + '.png'


def arcade_titles():
    titles = {}
    try:
        for line in open(os.path.join(LAUNCHER_DIR, 'cache/arcade-names.tsv'), encoding='utf-8'):
            f = line.rstrip('\n').split('\t')
            if len(f) >= 3 and f[0] in ('lr-fbneo', 'lr-mame2003'):
                titles.setdefault(f[1], []).append(f[2])
    except OSError:
        pass
    return titles


def missing_pictures():
    """Yield (console, ROM path, kind) for each box or screenshot that a game lacks."""
    console = None
    seen = set()
    for line in open(os.path.join(LAUNCHER_DIR, 'library.tsv'), encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'C':
            console = f[1]
        elif f[0] == 'G' and console in REPOSITORIES and len(f) > 9:
            for kind, value in (('box', f[8]), ('snap', f[9])):
                if not value and (f[2], kind) not in seen:
                    seen.add((f[2], kind))
                    yield console, f[2], kind


def fetch(repository, folder, name):
    request = urllib.request.Request(URL % (repository, folder, urllib.parse.quote(file_name(name))),
                                     headers={'User-Agent': 'arcade-launcher-pictures/1.0'})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read()
    except Exception:
        return None
    # A picture that several games share is a git symlink, whose raw text names the file.
    if not data.startswith(b'\x89PNG') and len(data) < 300:
        try:
            linked = data.decode('utf-8').strip()
            with urllib.request.urlopen(urllib.request.Request(
                    URL % (repository, folder, urllib.parse.quote(linked)),
                    headers={'User-Agent': 'arcade-launcher-pictures/1.0'}), timeout=30) as response:
                data = response.read()
        except Exception:
            return None
    return data if data.startswith(b'\x89PNG') else None


def fill(job, titles):
    console, path, kind = job
    base = os.path.splitext(os.path.basename(path))[0]
    names = titles.get(base, []) if console in ('arcade', 'neogeo') else [base, re.sub(r'\s*\[[^\]]*\]', '', base)]
    remote_folder, local_folder = FOLDERS[kind]
    target = os.path.join(os.path.dirname(path), local_folder, base + '.png')
    if os.path.exists(target):
        return job, 'present'
    for repository in REPOSITORIES[console]:
        for name in dict.fromkeys(names):
            data = fetch(repository, remote_folder, name)
            if data:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target + '.part', 'wb') as out:
                    out.write(data)
                os.replace(target + '.part', target)
                return job, 'found'
    return job, 'not found'


def main():
    titles = arcade_titles()
    jobs = list(missing_pictures())
    counts = {}
    with concurrent.futures.ThreadPoolExecutor(6) as pool:
        for (console, _, kind), result in pool.map(lambda job: fill(job, titles), jobs):
            key = (console, kind)
            counts.setdefault(key, {'found': 0, 'not found': 0, 'present': 0})[result] += 1
    total = sum(c['found'] for c in counts.values())
    for (console, kind), c in sorted(counts.items()):
        print('%-16s %-4s %4d found, %4d not on libretro-thumbnails' % (console, kind, c['found'], c['not found']))
    print('%d pictures downloaded for %d missing. Choose Look for new games in Settings to show them.' % (total, len(jobs)))


if __name__ == '__main__':
    main()
