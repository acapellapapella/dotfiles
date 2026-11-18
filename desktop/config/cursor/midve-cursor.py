#!/usr/bin/env python3
"""Build the midve cursor theme into ~/.local/share/icons/midve.

The shapes are Bibata Modern (bibata-modern/, GPL-3.0): soft, rounded cursors
with every state drawn. Here they are recoloured into the desktop palette, a
plum body with a lilac rim, and the bright accents of the original (rainbow
spinner, red "no", green "+", blue badges) become lilac, violet and wine. The
SVGs are rendered with rsvg-convert at each nominal size and packed into
Xcursor files, animated ones frame by frame. Hotspots and X11 aliases come from
Bibata's own build config.

  midve-cursor.py                 build and install the theme
  midve-cursor.py --preview DIR   write a contact sheet at on-screen size
"""
import glob, io, math, os, shutil, struct, subprocess, sys, tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'bibata-modern')
DST = os.path.expanduser('~/.local/share/icons/midve')
# Nominal sizes; niri loads size × ceil(scale): 20 → 40 on both monitors.
SIZES = [20, 24, 28, 32, 40, 48, 56, 64, 72, 96]

PLUM, LILAC, WINE, VIOLET = '#2a1826', '#ece0ee', '#c46a80', '#b48ac6'
COLORS = {
    '#00FF00': PLUM,      # body
    '#0000FF': LILAC,     # rim
    '#FF0000': PLUM,      # spinner disc
    # the spinner's four segments
    '#32A0DA': '#dcc3e4', '#7EBA41': '#b48ac6', '#F05024': '#c46a80', '#FCB813': '#f3e6f6',
    # "no": not-allowed, no-drop, crosshair dot
    '#FE0000': WINE,
    # badges: copy, link, menu, help, move, corners
    '#06B231': VIOLET, '#179DD8': VIOLET, '#5F3BE4': VIOLET, '#606060': VIOLET,
    '#F27400': VIOLET, '#0A6857': VIOLET, '#2C2C2C': PLUM,
    '#4FADDF': VIOLET, '#96C865': VIOLET, '#F1613A': VIOLET, '#FDBE2A': VIOLET,
}

# CSS cursor names (what GTK4, Qt 6, browsers and niri ask for on Wayland) that
# Bibata's config doesn't already alias, mapped to the Bibata cursor to use.
EXTRA = {
    'default': 'left_ptr', 'pointer': 'hand2', 'text': 'xterm', 'wait': 'wait',
    'progress': 'left_ptr_watch', 'help': 'question_arrow', 'context-menu': 'context-menu',
    'crosshair': 'crosshair', 'cell': 'plus', 'vertical-text': 'vertical-text',
    'alias': 'link', 'copy': 'copy', 'move': 'move', 'all-scroll': 'fleur',
    'no-drop': 'dnd_no_drop', 'not-allowed': 'crossed_circle', 'grab': 'hand1',
    'grabbing': 'grabbing', 'col-resize': 'sb_h_double_arrow', 'row-resize': 'sb_v_double_arrow',
    'ew-resize': 'sb_h_double_arrow', 'ns-resize': 'sb_v_double_arrow',
    'nesw-resize': 'fd_double_arrow', 'nwse-resize': 'bd_double_arrow',
    'n-resize': 'top_side', 's-resize': 'bottom_side', 'e-resize': 'right_side',
    'w-resize': 'left_side', 'ne-resize': 'top_right_corner', 'nw-resize': 'top_left_corner',
    'se-resize': 'bottom_right_corner', 'sw-resize': 'bottom_left_corner',
    'zoom-in': 'zoom-in', 'zoom-out': 'zoom-out', 'dnd-move': 'grabbing', 'dnd-none': 'grabbing',
    'pointer-move': 'pointer-move', 'openhand': 'hand1', 'closedhand': 'grabbing',
    'hand': 'hand2', 'pointing_hand': 'hand2', 'arrow': 'left_ptr', 'top_left_arrow': 'left_ptr',
    'ibeam': 'xterm', 'pen': 'pencil', 'draft': 'pencil', 'color-picker': 'crosshair',
    'up-arrow': 'sb_up_arrow', 'down-arrow': 'sb_down_arrow', 'left-arrow': 'sb_left_arrow',
    'right-arrow': 'sb_right_arrow', 'split_h': 'sb_h_double_arrow', 'split_v': 'sb_v_double_arrow',
    'size_all': 'fleur', 'all-resize': 'fleur', 'forbidden': 'crossed_circle',
}


def recolor(svg_text):
    for a, b in COLORS.items():
        svg_text = svg_text.replace(a, b).replace(a.lower(), b)
    return svg_text


def load_config():
    with open(os.path.join(SRC, 'x.build.toml'), 'rb') as f:
        cfg = tomllib.load(f)['cursors']
    fb = cfg.pop('fallback_settings')
    cursors = {}
    for key, c in cfg.items():
        png = c['png']
        if '*' in png:  # animated: a directory of numbered frames
            stem = png.split('-*')[0]
            files = sorted(glob.glob(os.path.join(SRC, 'svg', stem, f'{stem}-*.svg')))
        else:
            files = [os.path.join(SRC, 'svg', png.replace('.png', '.svg'))]
        if not files or not os.path.exists(files[0]):
            continue
        cursors[c.get('x11_name', key)] = {
            'files': files,
            'hot': (c.get('x_hotspot', fb['x_hotspot']), c.get('y_hotspot', fb['y_hotspot'])),
            'delay': c.get('x11_delay', fb['x11_delay']),
            'aliases': c.get('x11_symlinks', []),
        }
    return cursors


# ── Rendering and Xcursor packing ───────────────────────────────────────────

def render(svg_text, px):
    from PIL import Image
    png = subprocess.run(['rsvg-convert', '-w', str(px), '-h', str(px)], input=svg_text.encode(),
                         capture_output=True, check=True).stdout
    return Image.open(io.BytesIO(png)).convert('RGBA')


def premultiplied_bgra(img):
    # Xcursor pixels are premultiplied ARGB32, little-endian (bytes B G R A).
    return img.convert('RGBa').tobytes('raw', 'BGRa')


def xcursor(frames_by_size, hot, delay):
    """frames_by_size: {nominal: [PIL images]}; hot in the 256-unit source grid."""
    chunks = []
    for nominal, frames in frames_by_size.items():
        k = nominal / 256
        hx, hy = min(nominal - 1, round(hot[0] * k)), min(nominal - 1, round(hot[1] * k))
        for img in frames:
            w, h = img.size
            chunks.append((nominal, struct.pack('<9I', 36, 0xfffd0002, nominal, 1, w, h, hx, hy, delay)
                           + premultiplied_bgra(img)))
    header = struct.pack('<4sIII', b'Xcur', 16, 0x10000, len(chunks))
    pos = 16 + 12 * len(chunks)
    toc, body = b'', b''
    for nominal, data in chunks:
        toc += struct.pack('<III', 0xfffd0002, nominal, pos + len(body))
        body += data
    return header + toc + body


def preview(out_dir):
    """Every cursor as it appears on the 1.4× monitor (48 px scaled to 34)."""
    from PIL import Image, ImageDraw
    cursors = load_config()
    names = sorted(cursors)
    cols, cw, ch = 12, 64, 92
    rows = math.ceil(len(names) / cols)
    sheet = Image.new('RGBA', (cols * cw, rows * ch), '#141016')
    d = ImageDraw.Draw(sheet)
    for i, name in enumerate(names):
        files = cursors[name]['files']
        svg_text = recolor(open(files[len(files) // 3]).read())
        im = render(svg_text, 48).resize((34, 34), Image.LANCZOS)
        x, y = (i % cols) * cw, (i // cols) * ch
        d.rectangle([x, y + 40, x + cw - 1, y + 78], fill='#ece0ee')
        sheet.alpha_composite(im, (x + 15, y + 3))
        sheet.alpha_composite(im, (x + 15, y + 42))
        d.text((x + 2, y + 79), name[:11], fill='#9a8a98')
    os.makedirs(out_dir, exist_ok=True)
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(os.path.join(out_dir, 'sheet.png'))
    print(os.path.join(out_dir, 'sheet.png'))


def main():
    if '--preview' in sys.argv:
        preview(sys.argv[sys.argv.index('--preview') + 1])
        return
    cursors = load_config()
    shutil.rmtree(DST, ignore_errors=True)
    out = os.path.join(DST, 'cursors')
    os.makedirs(out)
    for name, c in cursors.items():
        svgs = [recolor(open(f).read()) for f in c['files']]
        by_size = {s: [render(t, s) for t in svgs] for s in SIZES}
        with open(os.path.join(out, name), 'wb') as f:
            f.write(xcursor(by_size, c['hot'], c['delay'] if len(svgs) > 1 else 0))
    for name, c in cursors.items():
        for alias in c['aliases']:
            if not os.path.lexists(os.path.join(out, alias)):
                os.symlink(name, os.path.join(out, alias))
    for alias, target in EXTRA.items():
        if not os.path.lexists(os.path.join(out, alias)) and os.path.exists(os.path.join(out, target)):
            os.symlink(target, os.path.join(out, alias))
    with open(os.path.join(DST, 'index.theme'), 'w') as f:
        f.write('[Icon Theme]\nName=midve\nComment=Bibata Modern in plum and lilac\n')
    print(DST)


if __name__ == '__main__':
    main()
