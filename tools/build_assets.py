"""Render the images of the arcade launcher's menu.

    python3 -I tools/build_assets.py

Reads consoles.json, the brand in brand/ (brand.json with name and colours, logo.svg in white)
and assets/ (fonts, console photos in consoles/, logos of consoles that the theme lacks in
logos/, banner and QR code in credits/), plus the carbon-2021 console logos in build/theme/,
which setup.sh copies and tools/deploy.sh fetches from the card. LAUNCHER_BRAND names another
brand folder.
Writes frontend/brand.h, the brand's name and colours for the programs in frontend/, and
build/launcher/, which every run deletes and recreates:

  art/logos/, art/consoles/   console logos in white and cut-out console photos, at their
                              exact size in the menu on a 1920x1080 screen
  art/credits/                hidden screen: logo frames, achievement cup, QR code, credit bug
  art/*.png                   brand logo, screensaver images and the LOADING word
  art/boot/                   boot animation masks (PGM)
  art/icons/                  line icons of the facts about the machine on the settings screen
  art/sounds/                 the menu's sounds (WAV), synthesized by this script
  fonts/                      menu fonts

Requires rsvg-convert and Pillow, plus numpy and scipy for the sounds and for photos on a
white background.
The carbon-2021 theme is by Rookervik (CC BY-NC-SA). This script recolours its logos to white.
"""
import html
import io
import json
import os
import re
import shutil
import subprocess

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THEME = os.path.join(ROOT, 'build/theme')
OUT = os.path.join(ROOT, 'build/launcher')
FONTS = os.path.join(ROOT, 'assets/fonts')
BRAND_DIR = os.environ.get('LAUNCHER_BRAND', os.path.join(ROOT, 'brand'))
BRAND = json.load(open(os.path.join(BRAND_DIR, 'brand.json'), encoding='utf-8'))
LOGO_SVG = os.path.join(BRAND_DIR, 'logo.svg')
BRAND_HEADER = os.path.join(ROOT, 'frontend/brand.h')

WHITE = (255, 255, 255)

# Sizes in pixels of the 1920x1080 screen, matching frontend/main.cpp. Boxes are (width, height).
LOGO_ROOM = (900, 200)                # console logo on the console screen
PHOTO_ROOM = (520, 216)               # console photo below that logo
PHOTOS = os.path.join(ROOT, 'assets/consoles')
LOGOS = os.path.join(ROOT, 'assets/logos')    # logos of consoles that the theme lacks
START_LOGO = 760                      # width of the centred brand logo at startup
CORNER_LOGO = 178                     # width of the brand logo in the menu corner

# Logos drawn on a filled badge. coverage() keeps only their light parts, which removes the badge.
LIGHT_PARTS_ONLY = {'gba', 'megadrive', 'genesis', 'segacd'}


def render_svg(path, height):
    png = subprocess.run(['rsvg-convert', '-h', str(height), path], check=True, capture_output=True).stdout
    return Image.open(io.BytesIO(png)).convert('RGBA')


def coverage(image, light_parts_only=False):
    """Return the alpha mask cropped to the drawing, optionally fading out dark parts."""
    alpha = image.getchannel('A')
    if light_parts_only:
        luminance = image.convert('RGB').convert('L')
        key = luminance.point(lambda v: 0 if v <= 110 else 255 if v >= 200 else (v - 110) * 255 // 90)
        alpha = ImageChops.multiply(alpha, key)
    return alpha.crop(alpha.point(lambda a: 255 if a > 8 else 0).getbbox())


def fit(mask, size):
    scale = min(size[0] / mask.width, size[1] / mask.height)
    return mask.resize((max(1, round(mask.width * scale)), max(1, round(mask.height * scale))), Image.LANCZOS)


def tinted(mask, colour):
    out = Image.new('RGBA', mask.size, colour + (0,))
    out.putalpha(mask)
    return out


def logo_picture(name):
    path = os.path.join(THEME, 'systems', name + '.svg')
    if not os.path.exists(path):
        path = os.path.join(LOGOS, name + '.svg')
    if not os.path.exists(path):
        return None
    mask = coverage(render_svg(path, 900), name in LIGHT_PARTS_ONLY)
    return tinted(fit(mask, LOGO_ROOM), WHITE)


def clear_white_ground(image):
    """Cut the object out of a photo on white, with its soft shadow as translucent black.

    A flood fill from the border through bright, colourless, smooth pixels finds the white.
    """
    import numpy as np
    from scipy import ndimage
    rgb = np.asarray(image.convert('RGB')).astype(np.float32)
    darkest = rgb.min(axis=2)
    colour = rgb.max(axis=2) - darkest
    grey = rgb.mean(axis=2)
    down, across = np.gradient(ndimage.gaussian_filter(grey, 1.0))
    flat = (colour < 18) & (darkest > 120) & (np.hypot(across, down) < 6)
    seeds = np.zeros_like(flat)
    seeds[0, :] = seeds[-1, :] = seeds[:, 0] = seeds[:, -1] = True
    ground = ndimage.binary_propagation(seeds & flat, mask=flat)
    thing = ndimage.binary_erosion(~ground, iterations=1)         # one pixel in, past the white fringe
    alpha = ndimage.gaussian_filter(thing.astype(np.float32), 0.7)
    shade = np.clip((246 - grey) / 246, 0, 1) * (~thing)
    out = np.zeros(rgb.shape[:2] + (4,), np.float32)
    out[..., :3] = rgb * thing[..., None]
    out[..., 3] = np.maximum(alpha, shade * 0.85) * 255
    return Image.fromarray(out.clip(0, 255).astype(np.uint8), 'RGBA')


def console_photo(key):
    """Return the console photo cut out and scaled down to fit PHOTO_ROOM, or None.

    Opaque corners mark a photo on a white background.
    """
    path = os.path.join(PHOTOS, key + '.png')
    if not os.path.exists(path):
        return None
    image = Image.open(path).convert('RGBA')
    corners = [image.getpixel(at)[3] for at in ((0, 0), (image.width - 1, 0), (0, image.height - 1), (image.width - 1, image.height - 1))]
    if min(corners) > 200:
        image = clear_white_ground(image)
    image = image.crop(image.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox())
    scale = min(PHOTO_ROOM[0] / image.width, PHOTO_ROOM[1] / image.height, 1)
    return image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))), Image.LANCZOS)


def logo_frames(svg):
    """Return (text, frame count) for the spinning character-art logo of the profile banner.

    A line "F" starts a frame. Other lines hold row, column, shade and characters, tab-separated.
    """
    spans = []
    for frame in re.findall(r'<g class="f"[^>]*>(.*?)</g>', svg, flags=re.S):
        spans.append([])
        for y, line in re.findall(r'<text class="g" y="([0-9.]+)"[^>]*>(.*?)</text>', frame, flags=re.S):
            for x, shade, letters in re.findall(r'<tspan x="([0-9.]+)"[^>]*class="c([0-9])">(.*?)</tspan>', line, flags=re.S):
                spans[-1].append((float(y), float(x), int(shade), html.unescape(letters)))
    left = min(x for frame in spans for _, x, _, _ in frame)
    top = min(y for frame in spans for y, _, _, _ in frame)
    lines = []
    for frame in spans:
        lines.append('F')
        lines.extend('%d\t%d\t%d\t%s' % (round((y - top) / 8.5), round((x - left) / 4.2167), shade, letters)
                     for y, x, shade, letters in frame)
    return '\n'.join(lines) + '\n', len(spans)


# Achievement cup of the hidden screen, one character per pixel: X gold, L highlight, D shadow,
# '.' transparent. The gold matches the first band of the word ARCADE (WORD_BANDS).
CUP = ['..XXXXXXXXXXX..', 'XXXLLXXXXXXDXXX', 'X.XLLXXXXXXDX.X', 'X.XLLXXXXXXDX.X', 'XXXLLXXXXXXDXXX', '.XXLLXXXXXXDXX.',
       '..XLXXXXXXXDX..', '...XLXXXXXDX...', '....XXXXXXX....', '.....XXXXX.....', '......XDX......', '......XDX......',
       '.....XXXXX.....', '....XXXXXXX....', '...XLLXXXXDX...', '...XXXXXXXXX...']
CUP_COLOURS = {'X': (249, 209, 89), 'L': (255, 255, 135), 'D': (184, 155, 66)}

# Bug beside the credit line of the hidden screen: B shell in the orange of arkandas.com, S its
# shine, D the split of the wings, H head, G legs and antennae.
BUG = ['.G.......G.', '..G.....G..', '...HHHHH...', 'G..HHHHH..G', '.GBBBDBBBG.', '..BSBDBBB..', 'GGBSBDBBBGG',
       '..BBBDBBB..', '.GBBBDBBBG.', 'G..BBDBB..G', '....BBB....']
BUG_COLOURS = {'B': (255, 164, 91), 'S': (255, 206, 150), 'D': (178, 96, 42), 'H': (150, 85, 40), 'G': (164, 187, 183)}


def pixel_art(rows, colours):
    """Render pixel art at one pixel per character for the menu to scale up with sharp edges."""
    image = Image.new('RGBA', (len(rows[0]), len(rows)), (0, 0, 0, 0))
    for y, row in enumerate(rows):
        for x, letter in enumerate(row):
            if letter in colours:
                image.putpixel((x, y), colours[letter] + (255,))
    return image


def brand_logo(width):
    png = subprocess.run(['rsvg-convert', '-w', str(width), LOGO_SVG], check=True, capture_output=True).stdout
    return Image.open(io.BytesIO(png)).convert('RGBA')


def label_mask(text, size=21, spacing=0.08, curve=0.6):
    """Render text as a mask in the label style of the menu: JetBrains Mono, letter-spaced.

    curve is the edge gamma of frontend/text.cpp, so the label has the same weight as in the menu.
    """
    scale = 4                                              # 4x supersampling for exact glyph positions
    font = ImageFont.truetype(os.path.join(FONTS, 'JetBrainsMono-Regular.ttf'), size * scale)
    advance = font.getlength('M') + spacing * size * scale
    canvas = Image.new('L', (round(advance * len(text)) + 4 * scale, 2 * size * scale), 0)
    draw = ImageDraw.Draw(canvas)
    for i, letter in enumerate(text):
        draw.text((round(2 * scale + i * advance), round(1.5 * size * scale)), letter, font=font, fill=255, anchor='ls')
    small = canvas.resize((canvas.width // scale, canvas.height // scale), Image.BOX)
    small = small.point(lambda v: round(255 * (v / 255) ** curve))
    return small.crop(small.getbbox())


def pixel_word(text, unit):
    """Render text in Press Start 2P (8x8 glyphs) as a mask with unit x unit font pixels."""
    font = ImageFont.truetype(os.path.join(FONTS, 'PressStart2P-Regular.ttf'), 8 * unit)
    canvas = Image.new('L', (len(text) * 8 * unit, 8 * unit), 0)
    draw = ImageDraw.Draw(canvas)
    draw.fontmode = '1'                                     # 1-bit glyphs without antialiasing
    draw.text((0, 0), text, font=font, fill=255)
    return canvas.crop(canvas.getbbox())


# Colours of the word ARCADE in the start animation (frontend/looks.c, paint_strips): bands of
# font pixel rows from yellow to purple over a dark shadow one font pixel right and down. Each
# band is (colour, first row, end row exclusive).
WORD_BANDS = [((249, 209, 89), 0, 2), ((243, 160, 88), 2, 4), ((216, 115, 166), 4, 6), ((161, 118, 210), 6, 7)]
WORD_SHADOW = tuple(BRAND['colours']['ink'])
SAVER_LINE = 'PRESS ANY BUTTON TO CONTINUE'   # screensaver prompt below the word ARCADE
LOADING_WORD = 'LOADING'                       # loading screen of a game


def letter_pixels(text):
    """Render text in Press Start 2P as a mask at one pixel per font pixel."""
    big = pixel_word(text, 16)
    return big.resize((big.width // 16, big.height // 16), Image.NEAREST)


def pixel_line(text):
    """Render text in Press Start 2P in white at one pixel per font pixel, for the menu to tint."""
    picture = Image.new('RGBA', letter_pixels(text).size, WHITE + (0,))
    picture.putalpha(letter_pixels(text))
    return picture


def arcade_picture():
    """Render ARCADE in its start animation colours, one pixel per font pixel, for 16x scaling."""
    small = letter_pixels('ARCADE')
    if small.height != 7:
        raise SystemExit('the word ARCADE has %d rows of letter pixels, the colours expect 7' % small.height)
    picture = Image.new('RGBA', (small.width + 1, small.height + 1), (0, 0, 0, 0))
    picture.paste(WORD_SHADOW + (255,), (1, 1), small)
    for colour, first, after in WORD_BANDS:
        picture.paste(colour + (255,), (0, first), small.crop((0, first, small.width, after)))
    return picture


def logo_parts(logo):
    """Split the brand logo into mark and lettering, each on a canvas of the full logo.

    The split is the first fully transparent column from x = 8, as in frontend/looks.c.
    """
    alpha = logo.getchannel('A')
    split = next(x for x in range(8, logo.width) if alpha.crop((x, 0, x + 1, logo.height)).getbbox() is None)
    mark, letters = logo.copy(), logo.copy()
    ImageDraw.Draw(mark).rectangle((split, 0, logo.width, logo.height), fill=(0, 0, 0, 0))
    ImageDraw.Draw(letters).rectangle((0, 0, split - 1, logo.height), fill=(0, 0, 0, 0))
    return mark, letters


def write_pgm(mask, path):
    """Write a binary PGM (P5), which frontend/boot.c reads without an image library."""
    with open(path, 'wb') as out:
        out.write(b'P5\n%d %d\n255\n' % mask.size)
        out.write(mask.tobytes())


# ------------------------------------------------------------------ settings icons

# Line icons for the facts about the machine on the settings screen, on a 24-unit grid. The
# menu draws them at 24 to 30 canvas pixels and tints them with the colour of each fact.
ICON_SIZE = 64
ICONS = {
    'wifi': '<path d="M2 8.5a15 15 0 0 1 20 0"/><path d="M5.5 12a10 10 0 0 1 13 0"/><path d="M9 15.5a5 5 0 0 1 6 0"/>'
            '<circle cx="12" cy="19" r="1.3" fill="#fff" stroke="none"/>',
    'ethernet': '<rect x="5" y="9" width="14" height="10" rx="1.5"/><path d="M9 9V5h6v4M9 13v2M12 13v2M15 13v2"/>',
    'bluetooth': '<path d="M7 7l10 10-5 5V2l5 5L7 17"/>',
    'cpu': '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M10 10h4v4h-4z"/>'
           '<path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/>',
    'memory': '<rect x="2" y="7" width="20" height="9" rx="1.5"/><path d="M6 16v3M10 16v3M14 16v3M18 16v3"/>'
              '<path d="M6 10h2.5v3H6zM10.75 10h2.5v3h-2.5zM15.5 10H18v3h-2.5z"/>',
    'card': '<path d="M9 2h9a2 2 0 0 1 2 2v16a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V7z"/><path d="M10 6v3M13 6v3M16 6v3"/>',
    'clock': '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
    'gamepad': '<path d="M7 8h10a5 5 0 0 1 4.9 6l-.4 2a2.5 2.5 0 0 1-4.3 1.2L15.5 15h-7l-1.7 2.2A2.5 2.5 0 0 1 2.5 16'
               'l-.4-2A5 5 0 0 1 7 8z"/><path d="M7.5 10.5v3M6 12h3"/>'
               '<circle cx="16" cy="11" r="1" fill="#fff" stroke="none"/><circle cx="18" cy="13" r="1" fill="#fff" stroke="none"/>',
    'bolt': '<path d="M13 2L4.5 13.5H11L10 22l8.5-11.5H12z"/>',
}


def icon_picture(name):
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="2" '
           'stroke-linecap="round" stroke-linejoin="round">%s</svg>' % ICONS[name])
    png = subprocess.run(['rsvg-convert', '-w', str(ICON_SIZE), '-h', str(ICON_SIZE)], input=svg.encode(),
                         check=True, capture_output=True).stdout
    return Image.open(io.BytesIO(png))


# ------------------------------------------------------------------ menu sounds

SOUND_RATE = 48000
SOUND_LEVEL = 0.2               # peak of every sound, as a share of full scale (about -14 dB)


def sound_time(seconds):
    import numpy as np
    return np.arange(int(SOUND_RATE * seconds)) / SOUND_RATE


def triangle(phase):
    import numpy as np
    return 4 * np.abs(phase % 1.0 - 0.5) - 1


def tick(frequency, seconds, decay):
    """A short triangle blip that dies away, for moves in a list."""
    import numpy as np
    t = sound_time(seconds)
    return triangle(t * frequency) * np.minimum(1, t / 0.001) * np.exp(-t / decay)


def glide(start, end, seconds, decay):
    """A triangle tone that slides from one frequency to another, for console changes."""
    import numpy as np
    t = sound_time(seconds)
    phase = np.cumsum(start + (end - start) * t / seconds) / SOUND_RATE
    return triangle(phase) * np.minimum(1, t / 0.003) * np.exp(-t / decay)


def chip(notes, duty=0.25, decay=0.05):
    """Notes of a pulse wave as in 8-bit games, (frequency, seconds) each, softened by a low-pass."""
    import numpy as np
    from scipy.signal import lfilter
    parts = []
    for frequency, seconds in notes:
        t = sound_time(seconds)
        pulse = np.where((t * frequency) % 1.0 < duty, 1.0, -1.0)
        parts.append(pulse * np.minimum(1, t / 0.002) * np.exp(-t / decay))
    smoothing = np.exp(-2 * np.pi * 3500 / SOUND_RATE)
    return lfilter([1 - smoothing], [1, -smoothing], np.concatenate(parts))


# Notes: G5 784, B5 988, C6 1047, D6 1175, E6 1319, G6 1568, C7 2093 Hz.
SOUNDS = {
    'move': lambda: tick(1300, 0.03, 0.008),
    'console': lambda: glide(520, 780, 0.07, 0.03),
    'open': lambda: chip([(784, 0.045), (1175, 0.09)]),
    'back': lambda: chip([(1175, 0.045), (784, 0.09)]),
    'play': lambda: chip([(988, 0.06), (1319, 0.2)], duty=0.5, decay=0.09),
    'secret': lambda: chip([(1047, 0.07), (1319, 0.07), (1568, 0.07), (2093, 0.3)], decay=0.12),
}


def write_sound(path, signal):
    """Write a sound as a 16-bit mono WAV file at SOUND_LEVEL, ending in silence."""
    import wave
    import numpy as np
    fade = min(len(signal), int(SOUND_RATE * 0.004))
    signal[-fade:] *= np.linspace(1, 0, fade)
    signal = signal / np.max(np.abs(signal)) * SOUND_LEVEL
    with wave.open(path, 'wb') as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(SOUND_RATE)
        out.writeframes((signal * 32767).astype('<i2').tobytes())


def write_brand_header():
    """Write frontend/brand.h with the brand's name and colours, only when they changed, so that
    make rebuilds the programs only then."""
    lines = ['// Written by tools/build_assets.py from brand/brand.json. Change that file and run the build again.',
             '#ifndef LAUNCHER_BRAND_H', '#define LAUNCHER_BRAND_H', '#define BRAND_NAME %s' % json.dumps(BRAND['name'])]
    for key, value in BRAND['colours'].items():
        stops = value if key == 'ground' else [value]
        for i, colour in enumerate(stops):
            name = 'BRAND_GROUND_%d' % i if key == 'ground' else 'BRAND_' + key.upper()
            lines.append('#define %s %d, %d, %d' % ((name,) + tuple(colour)))
    lines.append('#endif')
    text = '\n'.join(lines) + '\n'
    old = open(BRAND_HEADER, encoding='utf-8').read() if os.path.exists(BRAND_HEADER) else ''
    if text != old:
        with open(BRAND_HEADER, 'w', encoding='utf-8') as out:
            out.write(text)


def main():
    table = json.load(open(os.path.join(ROOT, 'consoles.json'), encoding='utf-8'))['consoles']

    write_brand_header()
    shutil.rmtree(OUT, ignore_errors=True)
    for folder in ('art/logos', 'art/consoles', 'art/credits', 'art/boot', 'art/icons', 'art/sounds', 'fonts'):
        os.makedirs(os.path.join(OUT, folder))

    for name in os.listdir(FONTS):
        # This script renders all Press Start 2P text, so the font stays off the Pi.
        if name.endswith('.ttf') and not name.startswith('PressStart2P'):
            shutil.copyfile(os.path.join(FONTS, name), os.path.join(OUT, 'fonts', name))
    # make_library.py on the Pi names the Recommended group after the brand.
    shutil.copyfile(os.path.join(BRAND_DIR, 'brand.json'), os.path.join(OUT, 'brand.json'))

    # Both logos have twice their width on a 1920x1080 screen, for 4K screens. The corner logo
    # has a small file of its own because the menu shows it in its first frame, where a small
    # file loads faster.
    large = brand_logo(2 * START_LOGO)
    large.save(os.path.join(OUT, 'art/brand-logo.png'))
    brand_logo(2 * CORNER_LOGO).save(os.path.join(OUT, 'art/brand-logo-small.png'))

    # The screensaver recolours the mark of the logo at every bounce, so mark and lettering are
    # separate images.
    mark, letters = logo_parts(large)
    mark.save(os.path.join(OUT, 'art/brand-logo-mark.png'))
    letters.save(os.path.join(OUT, 'art/brand-logo-letters.png'))
    arcade_picture().save(os.path.join(OUT, 'art/arcade-word.png'))
    pixel_line(SAVER_LINE).save(os.path.join(OUT, 'art/saver-line.png'))
    pixel_line(LOADING_WORD).save(os.path.join(OUT, 'art/loading-word.png'))

    # Boot animation masks. The logo is the large one halved with a box filter, the reduction the
    # menu applies for its first frame, so both draw identical pixels.
    alpha = large.getchannel('A')
    write_pgm(alpha.resize((START_LOGO, (alpha.height + 1) // 2), Image.BOX), os.path.join(OUT, 'art/boot/logo.pgm'))
    write_pgm(label_mask('STARTING'), os.path.join(OUT, 'art/boot/label.pgm'))
    write_pgm(pixel_word('ARCADE', 16), os.path.join(OUT, 'art/boot/arcade-pixel.pgm'))

    logos = photos = 0
    for entry in table:
        key = entry['system']
        photo = console_photo(key)
        if photo:
            photo.save(os.path.join(OUT, 'art/consoles', key + '.png'))
            photos += 1
        # A null "logo" in consoles.json makes the menu write the name as text.
        logo_name = entry['logo'] if 'logo' in entry else key
        mark = logo_picture(logo_name) if logo_name else None
        if mark:
            mark.save(os.path.join(OUT, 'art/logos', key + '.png'))
            logos += 1

    # Settings has a photo too. For Recommended the menu draws the box art of its first games.
    photo = console_photo('settings')
    if photo:
        photo.save(os.path.join(OUT, 'art/consoles/settings.png'))

    # Hidden screen: logo frames and QR code from assets/credits, plus the achievement cup.
    credits = os.path.join(ROOT, 'assets/credits')
    frames = 0
    if os.path.exists(os.path.join(credits, 'arkandas-profile-banner.svg')):
        text, frames = logo_frames(open(os.path.join(credits, 'arkandas-profile-banner.svg'), encoding='utf-8').read())
        open(os.path.join(OUT, 'art/credits/logo.txt'), 'w', encoding='utf-8').write(text)
        shutil.copyfile(os.path.join(credits, 'github-qr.png'), os.path.join(OUT, 'art/credits/github-qr.png'))
    pixel_art(CUP, CUP_COLOURS).save(os.path.join(OUT, 'art/credits/cup.png'))
    pixel_art(BUG, BUG_COLOURS).save(os.path.join(OUT, 'art/credits/bug.png'))

    for name in ICONS:
        icon_picture(name).save(os.path.join(OUT, 'art/icons', name + '.png'))
    for name, make in SOUNDS.items():
        write_sound(os.path.join(OUT, 'art/sounds', name + '.wav'), make())

    print('%d consoles: %d logos, %d photos, %d frames of the spinning logo, %d sounds'
          % (len(table), logos, photos, frames, len(SOUNDS)))


if __name__ == '__main__':
    main()
