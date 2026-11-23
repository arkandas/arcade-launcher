"""Write library.tsv, the game list of the arcade launcher, from the ROMs on the card.

arcade.sh runs it at startup and for the menu's "Look for new games":

    python3 -I make_library.py

Reads consoles.json, controllers.json and recommended.json next to this script, the strip
pictures of tools/make_strips.py in strips/, es_systems.cfg, the ROM folders with their
boxart/, snap/ and wheel/ art, scraped gamelist.xml files and the arcade DAT files of
lr-fbneo and lr-mame2003 (plus catver.ini).
Writes library.tsv (format in frontend/main.cpp), cache/arcade-names.tsv and the RetroArch
presets of the controllers in controllers.json in retroarch-joypads/. The other presets in
retroarch-joypads/ give their controllers' A, B and Select buttons to the menu.

Every run overwrites library.tsv. Edit the tables or this script to change it.
"""
import html
import json
import os
import re
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROMS = '/home/pi/RetroPie/roms'
CONFIGS = '/opt/retropie/configs'
ES_SYSTEMS = '/etc/emulationstation/es_systems.cfg'
GAMELISTS = ('/home/pi/.emulationstation/gamelists/%s/gamelist.xml', ROMS + '/%s/gamelist.xml')
JOYPADS = CONFIGS + '/all/retroarch-joypads'
PICTURES = (('box', 'boxart'), ('snap', 'snap'), ('wheel', 'wheel'))      # (menu field, art folder)

# Arcade DAT file per emulator. The default emulator of a folder names its games. The other
# DAT fills in the sets that the first one lacks.
ARCADE_LISTS = (
    ('lr-fbneo', '/opt/retropie/libretrocores/lr-fbneo/dats/FinalBurn Neo (ClrMame Pro XML, Arcade only).dat'),
    ('lr-mame2003', '/opt/retropie/libretrocores/lr-mame2003/metadata/mame2003.xml'),
)
ARCADE_CATEGORIES = '/opt/retropie/libretrocores/lr-mame2003/metadata/catver.ini'
ARCADE_CACHE = os.path.join(HERE, 'cache/arcade-names.tsv')


# ------------------------------------------------------------------ card contents

def table(name):
    return json.load(open(os.path.join(HERE, name), encoding='utf-8'))


def installed_systems():
    """Return {system: {'fullname': ..., 'endings': [...]}} from es_systems.cfg."""
    systems = {}
    for node in ET.parse(ES_SYSTEMS).getroot().findall('system'):
        name = node.findtext('name')
        systems[name] = {'fullname': node.findtext('fullname') or name,
                         'endings': (node.findtext('extension') or '').split()}
    return systems


def gamelist(system):
    """Return {ROM file name: metadata} from the scraped gamelist.xml files of a system."""
    facts = {}
    for pattern in GAMELISTS:
        path = pattern % system
        if not os.path.exists(path):
            continue
        try:
            games = ET.parse(path).getroot().findall('game')
        except ET.ParseError:
            continue
        for game in games:
            name = os.path.basename(game.findtext('path') or '')
            if name:
                facts[name] = {'title': repaired(game.findtext('name') or ''),
                               'year': (game.findtext('releasedate') or '')[:4],
                               'maker': repaired(game.findtext('publisher') or game.findtext('developer') or ''),
                               'category': repaired(game.findtext('genre') or '')}
    return facts


# A lead byte of UTF-8 read as a single-byte character, followed by a continuation byte read
# the same way (Latin-1 or Windows-1252): "Ã©" for é.
GARBLED = re.compile('[Â-ô][\u0080-¿ŒœŠšŸŽžƒˆ˜'
                     '–—‘-„†-•…‰‹›€™]|Ã ')


def single_bytes(text):
    """Return the bytes that each character of text stood for in Latin-1 or Windows-1252."""
    out = bytearray()
    for character in text:
        out += bytes([ord(character)]) if ord(character) < 256 else character.encode('cp1252')
    return bytes(out)


def repaired(text):
    """Return text whose UTF-8 a scraper stored as single bytes ("PokÃ©mon") as it was meant.

    Some text went through that twice ("KÃ…Âgyo" for Kōgyō). A no-break space in such text often
    became a plain one ("DÃ©jÃ Vu"), which the repair puts back. Text that does not decode stays
    as it is."""
    for _ in range(3):
        if not GARBLED.search(text):
            break
        try:
            text = single_bytes(text.replace('Ã ', 'Ã ')).decode('utf-8')
        except UnicodeError:
            break
    return text


def without_notes(name):
    """Strip bracketed tags: "Tetris (World) (Rev A)" becomes "Tetris"."""
    return re.sub(r'\s*[\(\[][^\)\]]*[\)\]]', '', name).strip() or name


def notes_of(name):
    """Join the version tags in round brackets: "(USA) (Rev A)" gives "USA · Rev A"."""
    return ' · '.join(note.strip() for note in re.findall(r'\(([^\)]*)\)', name) if note.strip())


def files_in(system, endings):
    """Return (name, file name) for each ROM of a system, the name without its extension."""
    folder = os.path.join(ROMS, system)
    found = []
    try:
        files = sorted(os.listdir(folder))
    except OSError:
        return found
    by_length = sorted(endings, key=len, reverse=True)
    for file_name in files:
        if file_name.startswith('.') or not os.path.isfile(os.path.join(folder, file_name)):
            continue
        for ending in by_length:
            if file_name.endswith(ending):
                found.append((file_name[:-len(ending)], file_name))
                break
    return found


def pictures_in(system):
    """Return {kind: {ROM name: path}} for the PNG files in the art folders of a system."""
    found = {}
    for kind, folder in PICTURES:
        path = '%s/%s/%s' % (ROMS, system, folder)
        try:
            found[kind] = {name[:-4]: path + '/' + name for name in os.listdir(path) if name.endswith('.png')}
        except OSError:
            found[kind] = {}
    return found


def default_emulator(system):
    """Return the default emulator from the emulators.cfg of a system, or '' if unset."""
    try:
        text = open('%s/%s/emulators.cfg' % (CONFIGS, system), encoding='utf-8').read()
    except OSError:
        return ''
    match = re.search(r'^default\s*=\s*"([^"]*)"', text, re.M)
    return match.group(1) if match else ''


# ------------------------------------------------------------------ arcade metadata

GAME = re.compile(r'<game ([^>]*)>\s*<description>([^<]*)</description>'
                  r'(?:\s*<year>([^<]*)</year>)?(?:\s*<manufacturer>([^<]*)</manufacturer>)?')


def arcade_sources():
    return [path for _, path in ARCADE_LISTS] + [ARCADE_CATEGORIES]


def signature():
    """Return path, size and mtime of each arcade source file, which an emulator update changes."""
    parts = []
    for path in arcade_sources():
        try:
            status = os.stat(path)
            parts.append('%s:%d:%d' % (path, status.st_size, int(status.st_mtime)))
        except OSError:
            parts.append(path + ':missing')
    return ' '.join(parts)


def build_arcade_cache():
    """Parse the arcade DAT files and catver.ini into cache/arcade-names.tsv.

    Line 1 is signature(). The other lines are tab-separated records:
        emulator  set  title  year  maker  'bios' or ''
        'category'  set  category
    """
    lines = [signature()]
    for emulator, path in ARCADE_LISTS:
        try:
            text = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for match in GAME.finditer(text):
            attributes, title, year, maker = match.groups()
            name = re.search(r'\bname="([^"]+)"', attributes)
            if not name:
                continue
            bios = 'isbios="yes"' in attributes or 'runnable="no"' in attributes
            lines.append('\t'.join([emulator, name.group(1), field(html.unescape(title)), field(year or ''),
                                    field(html.unescape(maker or '')), 'bios' if bios else '']))
    try:
        section = ''
        for line in open(ARCADE_CATEGORIES, encoding='utf-8', errors='replace'):
            line = line.strip()
            if line.startswith('['):
                section = line
            elif section == '[Category]' and '=' in line:
                name, category = line.split('=', 1)
                lines.append('\t'.join(['category', name, field(category.replace('* Mature *', ''))]))
    except OSError:
        pass
    os.makedirs(os.path.dirname(ARCADE_CACHE), exist_ok=True)
    write(ARCADE_CACHE, '\n'.join(lines) + '\n')


def arcade_names():
    """Return ({emulator: {set: (title, year, maker, is_bios)}}, {set: category}) from the cache."""
    try:
        current = open(ARCADE_CACHE, encoding='utf-8').readline().rstrip('\n') == signature()
    except OSError:
        current = False
    if not current:
        build_arcade_cache()
    names, categories = {}, {}
    with open(ARCADE_CACHE, encoding='utf-8') as cache:
        cache.readline()
        for line in cache:
            parts = line.rstrip('\n').split('\t')
            if parts[0] == 'category' and len(parts) >= 3:
                categories[parts[1]] = parts[2]
            elif len(parts) >= 6:
                names.setdefault(parts[0], {})[parts[1]] = (parts[2], parts[3], parts[4], parts[5] == 'bios')
    return names, categories


# ------------------------------------------------------------------ consoles and games

def games_of(entry, systems, arcade):
    """Return the games of one consoles.json entry, sorted by title."""
    games = []
    # Arcade sets carry the same short name in every folder, so a set without a picture in its
    # own folder takes the picture of its namesake in another folder of the console.
    shared = {kind: {} for kind, _ in PICTURES}
    if entry.get('names') == 'arcade':
        for system in [entry['system']] + entry.get('also', []):
            for kind, found in pictures_in(system).items():
                for name, path in found.items():
                    shared[kind].setdefault(name, path)
    for system in [entry['system']] + entry.get('also', []):
        runs_as = entry.get('runs_as', system) if system == entry['system'] else system
        if runs_as not in systems:
            continue                          # system not installed on this card
        files = files_in(system, systems[runs_as]['endings'] + entry.get('endings', []))
        if not files:
            continue
        scraped = gamelist(system)
        pictures = pictures_in(system)
        lists = []
        if entry.get('names') == 'arcade':
            names, categories = arcade()
            first = default_emulator(runs_as)
            lists = [names[first]] if first in names else []
            lists += [names[emulator] for emulator, _ in ARCADE_LISTS if emulator in names and emulator != first]
        for name, file_name in files:
            known = scraped.get(file_name, {})
            game = {'system': runs_as, 'folder': system, 'name': name, 'rom': '%s/%s/%s' % (ROMS, system, file_name),
                    'title': known.get('title') or without_notes(name), 'year': known.get('year', ''),
                    'maker': known.get('maker', ''), 'category': known.get('category') or notes_of(name), 'tag': ''}
            if entry.get('names') == 'arcade':
                listed = next((names_of[name] for names_of in lists if name in names_of), None)
                if any(name in names_of and names_of[name][3] for names_of in lists):
                    continue                  # BIOS or another set that cannot run alone
                if listed:
                    # Keep the first of several regional titles: "Windjammers / Flying Power Disc"
                    game['title'] = known.get('title') or without_notes(listed[0]).split(' / ')[0]
                    game['year'] = game['year'] or (listed[1] if re.fullmatch(r'\d{4}', listed[1]) else '')
                    game['maker'] = game['maker'] or without_notes(listed[2])
                game['category'] = known.get('category') or categories.get(name, '').strip()
            for kind, _ in PICTURES:
                game[kind] = pictures[kind].get(name, '') or shared[kind].get(name, '')
            games.append(game)
    games.sort(key=lambda game: (game['title'].lower(), game['name']))
    return games


def shots_for(games, patterns, used, most=6):
    """Pick up to `most` strip screenshots: one per matching pattern, then the first games.

    Screenshots in `used` belong to another console's strips and stay out, so every console
    screen shows its own games.
    """
    candidates = [(game['name'], game['snap']) for game in games if game['snap'] and game['snap'] not in used]
    chosen = []
    for pattern in list(patterns) + ['']:
        for name, path in candidates:
            if len(chosen) >= most:
                break
            if re.search(pattern, name, re.I) and path not in chosen:
                chosen.append(path)
                if pattern:
                    break
    return chosen


def cut_strips(key):
    """Return the strip pictures of tools/make_strips.py for a console, as paths under HERE."""
    try:
        names = sorted(name for name in os.listdir(os.path.join(HERE, 'strips', key)) if name.endswith('.png'))
    except OSError:
        return []
    return ['strips/%s/%s' % (key, name) for name in names]


def strip_lines(key, choose):
    """Return the library lines of a console's strips: S lines for the pictures of
    tools/make_strips.py, else X lines for the screenshots that choose() returns."""
    cut = cut_strips(key)
    return [['S', path] for path in cut] if cut else [['X', path] for path in choose()]


def recommended_shots(picks, used, most=6):
    """Pick strip screenshots for Recommended: games from no other console's strips, one console after another."""
    by_system = {}
    for game in picks:
        if game['snap'] and game['snap'] not in used:
            by_system.setdefault(game['system'], []).append(game['snap'])
    chosen = []
    while len(chosen) < most and any(by_system.values()):
        for shots in by_system.values():
            if shots and len(chosen) < most:
                chosen.append(shots.pop(0))
    return chosen


def art(folder, name):
    """Return name if tools/build_assets.py produced art/<folder>/<name>, else ''."""
    return name if name and os.path.exists(os.path.join(HERE, 'art', folder, name)) else ''


def library():
    """Return (console line, strip lines, games) per console with games, in menu order."""
    consoles = table('consoles.json')['consoles']
    systems = installed_systems()
    listed = set()
    for entry in consoles:
        listed.add(entry['system'])
        listed.update(entry.get('also', []))
    for name, system in systems.items():      # systems missing from consoles.json, except RetroPie's menu
        if name not in listed and name != 'retropie':
            consoles.append({'system': name, 'name': system['fullname'], 'group': 'MORE', 'maker': ''})

    cached = []

    # Loads arcade_names() on first use by an arcade console.
    def arcade():
        if not cached:
            cached.append(arcade_names())
        return cached[0]

    present = []
    used = set()
    for entry in consoles:
        games = games_of(entry, systems, arcade)
        if not games:
            continue
        key = entry['system']
        facts = ' · '.join(str(part) for part in (entry.get('year'), entry.get('tag')) if part)
        # A null "logo" in consoles.json makes the menu write the name as text.
        logo = art('logos', key + '.png') if entry.get('logo', key) else ''
        line = [key, entry['name'], entry.get('group', 'MORE'), entry.get('maker', ''), facts, 'console', logo,
                art('consoles', key + '.png')]
        strips = strip_lines(key, lambda: shots_for(games, entry.get('strips', []), used))
        used.update(path for _, path in strips)
        present.append((line, strips, games))
    return present


def recommended(present):
    """Return the recommended.json games on the card, in file order, tagged with their mode."""
    by_console = {line[0]: games for line, _, games in present}
    chosen = []
    for entry in table('recommended.json')['games']:
        for game in by_console.get(entry['console'], []):
            if re.search(entry['game'], game['name'], re.I) and game not in [g for g, _ in chosen]:
                chosen.append((game, entry.get('mode', '')))
                break
    return [dict(game, tag=mode) for game, mode in chosen]


# ------------------------------------------------------------------ output

def field(text):
    """Collapse whitespace so that a value cannot split a TSV line."""
    return re.sub(r'\s+', ' ', str(text)).strip()


def write(path, text):
    """Write path atomically through a rename of path + '.new'."""
    with open(path + '.new', 'w', encoding='utf-8') as out:
        out.write(text)
    os.replace(path + '.new', path)


PRESET_MARK = '# Written by make_library.py'
# The menu writes presets from SDL's controller database for pads that had none
# (frontend/main.cpp, write_sdl_preset) and reads those pads from SDL itself.
SDL_PRESET_MARK = "# Written by the arcade launcher from SDL's controller database"


def preset_pads(listed):
    """Return J lines for the RetroArch presets of controllers that controllers.json leaves out,
    such as pads set up in EmulationStation: A opens, B goes back, Select adds a game to
    Recommended or takes it out."""
    lines = []
    try:
        names = sorted(os.listdir(JOYPADS))
    except OSError:
        return lines
    for name in names:
        if not name.endswith('.cfg'):
            continue
        try:
            with open(os.path.join(JOYPADS, name), encoding='utf-8', errors='replace') as preset:
                text = preset.read()
        except OSError:
            continue
        if text.startswith(PRESET_MARK) or text.startswith(SDL_PRESET_MARK):
            continue
        values = dict(re.findall(r'^\s*(\w+)\s*=\s*"([^"]*)"', text, re.M))
        device = field(values.get('input_device', ''))
        a, b, select = (values.get(key, '') for key in ('input_a_btn', 'input_b_btn', 'input_select_btn'))
        if not device or device in listed or not a.isdigit() or not b.isdigit():
            continue
        listed.add(device)
        lines.append(['J', device, a, b, 1 if values.get('input_up_axis') == '+1' else 0, -1, -1,
                      select if select.isdigit() else -1])
    return lines


def write_library(present, picks):
    lines = []
    controllers = table('controllers.json')['controllers']
    for name, buttons in controllers.items():
        lines.append(['J', name, buttons.get('open', buttons['a']), buttons.get('back', buttons['b']),
                      1 if buttons.get('up_down_swapped') else 0,
                      buttons.get('next_letter', -1), buttons.get('previous_letter', -1),
                      buttons.get('favourite', buttons.get('select', -1))])
    lines.extend(preset_pads({field(name) for name in controllers}))

    def console(line, strips, games):
        lines.append(['C'] + line)
        lines.extend(strips)
        for game in games:
            lines.append(['G', game['system'], game['rom'], game['title'], game['year'], game['maker'],
                          game['category'], game['tag'], game['box'], game['snap'], game['wheel']])

    if picks:
        # Recommended has an empty photo field: the menu draws the box art of its first games there.
        # Its group heading is the name in brand.json, which the installation copies next to this script.
        used = {path for _, strips, _ in present for _, path in strips}
        try:
            heading = table('brand.json')['name'].upper()
        except (OSError, ValueError, KeyError):
            heading = 'RECOMMENDED'
        console(['recommended', 'Recommended', heading, '', '', 'recommended', '', ''],
                strip_lines('recommended', lambda: recommended_shots(picks, used)), picks)
    for line, strips, games in present:
        console(line, strips, games)
    console(['settings', 'Settings', 'MACHINE', '', 'WI-FI · SOUND · EMULATORS · POWER', 'settings', '',
             art('consoles', 'settings.png')], [], [])
    write(os.path.join(HERE, 'library.tsv'), ''.join('\t'.join(field(part) for part in line) + '\n' for line in lines))


def write_emulator_presets():
    """Write the RetroArch preset of every controller in controllers.json, on every run.

    The table decides how its controllers behave, so a preset that RetroPie wrote for one of
    them gets replaced. RetroPie's presets of other controllers stay untouched.
    """
    if not os.path.isdir(JOYPADS):
        return
    controllers = table('controllers.json')['controllers']
    for name, b in controllers.items():
        path = '%s/%s.cfg' % (JOYPADS, name)
        up, down = ('+1', '-1') if b.get('up_down_swapped') else ('-1', '+1')
        settings = [('input_device', name), ('input_driver', 'udev'),
                    ('input_up_axis', up), ('input_down_axis', down),
                    ('input_left_axis', '-0'), ('input_right_axis', '+0'),
                    # The stick also moves the left analog stick, for games that read that stick.
                    ('input_l_x_plus_axis', '+0'), ('input_l_x_minus_axis', '-0'),
                    ('input_l_y_plus_axis', down), ('input_l_y_minus_axis', up),
                    ('input_a_btn', b['a']), ('input_b_btn', b['b']), ('input_x_btn', b['x']), ('input_y_btn', b['y']),
                    ('input_l_btn', b['l']), ('input_r_btn', b['r']),
                    ('input_select_btn', b['select']), ('input_start_btn', b['start']),
                    # Hotkeys with Select held:
                    # A quits (with a second press, see install.sh), X pauses, L loads and R
                    # saves the state, left and right change the state slot, up and down the
                    # shader. Start+Select opens RetroArch's menu (install.sh).
                    ('input_enable_hotkey_btn', b['select']), ('input_exit_emulator_btn', b['a']),
                    ('input_pause_toggle_btn', b['x']),
                    ('input_load_state_btn', b['l']), ('input_save_state_btn', b['r']),
                    ('input_state_slot_decrease_axis', '-0'), ('input_state_slot_increase_axis', '+0'),
                    ('input_shader_next_axis', up), ('input_shader_prev_axis', down)]
        write(path, PRESET_MARK + ' of the arcade launcher from controllers.json, again on every\n'
              '# start of the menu. Change controllers.json to change this file.\n'
              + ''.join('%s = "%s"\n' % pair for pair in settings))


def main():
    present = library()
    picks = recommended(present)
    write_library(present, picks)
    write_emulator_presets()
    games = sum(len(games) for _, _, games in present)
    print('%d %s in %d %s, %d recommended' % (games, 'game' if games == 1 else 'games', len(present),
                                               'console' if len(present) == 1 else 'consoles', len(picks)))
    for line, _, games in present:
        print('  %s: %d' % (line[1], len(games)))


if __name__ == '__main__':
    main()
