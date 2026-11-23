"""Build the strip pictures of the console screens from game screenshots.

    python3 -I tools/make_strips.py ROMS

strips.json names six screenshots for each console and six for Recommended. ROMS is the roms
folder of a RetroPie card, where the screenshots of an entry without "thumbnails" sit in
<system>/snap/. An entry with "thumbnails" names a repository of libretro-thumbnails on
GitHub. The script downloads that picture from the repository's Named_Snaps folder, or from
the folder in "folder", into build/screenshots/ on the first run. When a card lacks the
screenshot of an entry, the picture in the entry's "or" takes its place, so every card gets
all strips.

The script writes build/strips/<console>/, one PNG per strip. It cuts the slice around the
entry's focus from the screenshot, a little wider than a strip on the screen. Pixel art at
its console's own resolution then grows four times with two passes of Scale2x, which rounds
the stair steps. Larger screenshots and 3D graphics scale with Lanczos resampling to about
the same height. strips.json describes the fields of an entry. setup.sh and tools/deploy.sh
copy build/strips into the launcher's folder, where pi/make_library.py lists the pictures for
the menu. Like the games, the screenshots stay out of this project.

Requires Pillow.
"""
import json
import os
import re
import shutil
import sys
import urllib.parse
import urllib.request

from PIL import Image, ImageEnhance

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build/strips')
DOWNLOADS = os.path.join(ROOT, 'build/screenshots')
THUMBNAILS = 'https://raw.githubusercontent.com/libretro-thumbnails/%s/master/%s/%s'
SLICE = 0.33            # slice width over height, a little wider than a strip on the console screen
PIXEL_ART_HEIGHT = 300  # screenshots up to this height are pixel art at their console's resolution
TALL = 960              # height of a strip picture cut from a larger screenshot
# The screen cuts off the outer part of the first and the last strip. Their slices put the
# focus in the middle of the part on screen, at this share of the slice's width.
FOCUS_AT = {1: 0.71, 6: 0.29}


def scale2x(image):
    """Double a picture with Scale2x (EPX), which rounds the stair steps of pixel art."""
    w, h = image.size
    src = image.load()
    out = Image.new(image.mode, (2 * w, 2 * h))
    dst = out.load()
    for y in range(h):
        for x in range(w):
            e = src[x, y]
            up, down = src[x, max(y - 1, 0)], src[x, min(y + 1, h - 1)]
            left, right = src[max(x - 1, 0), y], src[min(x + 1, w - 1), y]
            e0 = e1 = e2 = e3 = e
            if up != down and left != right:
                e0 = left if left == up else e
                e1 = right if up == right else e
                e2 = left if left == down else e
                e3 = right if down == right else e
            dst[2 * x, 2 * y], dst[2 * x + 1, 2 * y], dst[2 * x, 2 * y + 1], dst[2 * x + 1, 2 * y + 1] = e0, e1, e2, e3
    return out


def fetch(repository, folder, name):
    request = urllib.request.Request(THUMBNAILS % (repository, folder, urllib.parse.quote(name)),
                                     headers={'User-Agent': 'arcade-strips/1.0'})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read()


def download(repository, folder, name):
    """Return the path of a libretro-thumbnails picture, downloading it on first use."""
    path = os.path.join(DOWNLOADS, repository, folder, name)
    if os.path.isfile(path):
        return path
    data = fetch(repository, folder, name)
    # A picture that several games share is a git symlink, whose raw text names the file.
    if not data.startswith(b'\x89PNG') and len(data) < 300:
        data = fetch(repository, folder, data.decode('utf-8').strip())
    if not data.startswith(b'\x89PNG'):
        sys.exit('%s/%s/%s is not a PNG file' % (repository, folder, name))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + '.new', 'wb') as out:
        out.write(data)
    os.replace(path + '.new', path)
    return path


def strip_picture(path, strip, number):
    image = Image.open(path).convert('RGBA').convert('RGB')
    w, h = image.size
    pixel_art = h <= PIXEL_ART_HEIGHT and not strip.get('3d')
    top, bottom = (round(share * h) for share in strip.get('rows', (0, 1)))
    h = bottom - top
    width = min(w, round(h * SLICE))
    left = max(0, min(round(strip['focus'] * w - FOCUS_AT.get(number, 0.5) * width), w - width))
    image = image.crop((left, top, left + width, bottom))
    if pixel_art:
        image = scale2x(scale2x(image))
    else:
        image = image.resize((round(width * TALL / h), TALL), Image.LANCZOS)
    if strip.get('bright', 1) != 1:
        image = ImageEnhance.Brightness(image).enhance(strip['bright'])
    return image


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    roms = sys.argv[1]
    table = json.load(open(os.path.join(ROOT, 'strips.json'), encoding='utf-8'))['strips']
    shutil.rmtree(OUT, ignore_errors=True)
    missing = 0
    for console, strips in table.items():
        os.makedirs(os.path.join(OUT, console))
        for number, strip in enumerate(strips, 1):
            if 'thumbnails' in strip:
                path = download(strip['thumbnails'], strip.get('folder', 'Named_Snaps'), strip['file'])
            else:
                path = os.path.join(roms, strip['system'], 'snap', strip['file'])
                if not os.path.isfile(path) and 'or' in strip:
                    # The same game from libretro-thumbnails, with its own file and focus.
                    strip = dict(strip, **strip['or'])
                    path = download(strip['thumbnails'], strip.get('folder', 'Named_Snaps'), strip['file'])
                elif not os.path.isfile(path):
                    print('missing: ' + path)
                    missing += 1
                    continue
            name = re.sub(r'[^A-Za-z0-9]+', '-', os.path.splitext(strip['file'])[0]).strip('-')[:40]
            strip_picture(path, strip, number).save(os.path.join(OUT, console, '%d-%s.png' % (number, name)), optimize=True)
    print('strips of %d consoles in build/strips, %d screenshots missing' % (len(table), missing))


if __name__ == '__main__':
    main()
