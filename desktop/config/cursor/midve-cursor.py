#!/usr/bin/env python3
"""Build the midve cursor theme into ~/.local/share/icons/midve.

Every cursor is drawn here as SVG on a 32-unit grid, rendered with rsvg-convert
at each nominal size and packed into Xcursor files (animated ones frame by
frame). The look follows the desktop: a pale lilac gradient body, a thin deep
plum outline with round joins, and a soft shadow that falls slightly downward
like the window shadows. Safe to re-run.
"""
import math, os, shutil, struct, subprocess, sys

DST = os.path.expanduser('~/.local/share/icons/midve')
SIZES = [24, 32, 36, 48, 64, 72, 96]

PLUM = '#2a1826'
WINE = '#c46a80'

DEFS = '''
<defs>
  <linearGradient id="body" gradientUnits="userSpaceOnUse" x1="0" y1="4" x2="0" y2="28">
    <stop offset="0" stop-color="#fbf5fc"/><stop offset="1" stop-color="#e4d0e9"/>
  </linearGradient>
  <linearGradient id="accent" gradientUnits="userSpaceOnUse" x1="0" y1="4" x2="0" y2="28">
    <stop offset="0" stop-color="#e3c6ec"/><stop offset="1" stop-color="#b78fc8"/>
  </linearGradient>
  <linearGradient id="wine" gradientUnits="userSpaceOnUse" x1="0" y1="8" x2="0" y2="28">
    <stop offset="0" stop-color="#dd8ea0"/><stop offset="1" stop-color="#b75a71"/>
  </linearGradient>
  <filter id="shadow" x="-40%" y="-40%" width="180%" height="180%">
    <feGaussianBlur in="SourceAlpha" stdDeviation="0.85"/>
    <feOffset dy="1.1" result="b"/>
    <feFlood flood-color="#12070f" flood-opacity="0.42"/>
    <feComposite in2="b" operator="in" result="s"/>
    <feMerge><feMergeNode in="s"/><feMergeNode in="SourceGraphic"/></feMerge>
  </filter>
</defs>'''


def svg(body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">{DEFS}'
            f'<g filter="url(#shadow)">{body}</g></svg>')


# ── Building blocks ──────────────────────────────────────────────────────────

ARROW = 'M7 4.5 L24.3 15.9 L16.2 17.7 L11.3 25.7 Z'


def arrow(fill='url(#body)'):
    return (f'<path d="{ARROW}" fill="{fill}" stroke="{PLUM}" stroke-width="1.5" '
            f'stroke-linejoin="round"/>')


def line(d, w=1.5, color='url(#body)', cap='round'):
    """A lilac stroke with a plum edge: the plum stroke underneath is wider."""
    return (f'<path d="{d}" fill="none" stroke="{PLUM}" stroke-width="{w + 2.3}" '
            f'stroke-linecap="{cap}" stroke-linejoin="round"/>'
            f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{w}" '
            f'stroke-linecap="{cap}" stroke-linejoin="round"/>')


def head(x, y, angle, size=4.3):
    """Filled arrowhead pointing at angle (degrees) with its tip at (x, y)."""
    a = math.radians(angle)
    bx, by = x - size * math.cos(a), y - size * math.sin(a)
    px, py = -math.sin(a) * size * 0.78, math.cos(a) * size * 0.78
    return (f'M{x:.2f} {y:.2f} L{bx + px:.2f} {by + py:.2f} '
            f'L{bx - px:.2f} {by - py:.2f} Z')


def double_arrow(angle, length=17, extra=''):
    a = math.radians(angle)
    dx, dy = math.cos(a) * length / 2, math.sin(a) * length / 2
    x1, y1, x2, y2 = 16 - dx, 16 - dy, 16 + dx, 16 + dy
    sx, sy = math.cos(a) * 3.1, math.sin(a) * 3.1
    stem = f'M{x1 + sx:.2f} {y1 + sy:.2f} L{x2 - sx:.2f} {y2 - sy:.2f}'
    heads = head(x2, y2, angle) + ' ' + head(x1, y1, angle + 180)
    return (line(stem, 1.8) + extra +
            f'<path d="{heads}" fill="url(#body)" stroke="{PLUM}" stroke-width="1.3" '
            f'stroke-linejoin="round"/>')


def single_arrow(angle, length=14):
    a = math.radians(angle)
    x2, y2 = 16 + math.cos(a) * length / 2, 16 + math.sin(a) * length / 2
    x1, y1 = 16 - math.cos(a) * length / 2, 16 - math.sin(a) * length / 2
    sx, sy = math.cos(a) * 3.1, math.sin(a) * 3.1
    return (line(f'M{x1:.2f} {y1:.2f} L{x2 - sx:.2f} {y2 - sy:.2f}', 1.8) +
            f'<path d="{head(x2, y2, angle)}" fill="url(#body)" stroke="{PLUM}" '
            f'stroke-width="1.3" stroke-linejoin="round"/>')


def badge(inner, fill='url(#accent)'):
    """Arrow with a small round badge at its lower right."""
    return (arrow() +
            f'<circle cx="24.2" cy="23.6" r="5" fill="{fill}" stroke="{PLUM}" '
            f'stroke-width="1.3"/>' + inner)


def spinner(cx, cy, r, w, phase, arc_deg):
    """Faint track plus a lilac arc; phase and arc length animate."""
    a0 = math.radians(phase - 90)
    a1 = a0 + math.radians(arc_deg)
    x0, y0 = cx + r * math.cos(a0), cy + r * math.sin(a0)
    x1, y1 = cx + r * math.cos(a1), cy + r * math.sin(a1)
    large = 1 if arc_deg > 180 else 0
    return (f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="{PLUM}" '
            f'stroke-width="{w + 2.3}"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="none" stroke="#5a4258" '
            f'stroke-width="{w}"/>'
            f'<path d="M{x0:.2f} {y0:.2f} A{r} {r} 0 {large} 1 {x1:.2f} {y1:.2f}" '
            f'fill="none" stroke="url(#body)" stroke-width="{w}" stroke-linecap="round"/>')


def spin_frames(make, n=40):
    # One turn per second; the arc breathes between 60° and 230°.
    return [make(360 * i / n, 145 + 85 * math.sin(2 * math.pi * i / n)) for i in range(n)]


I_BEAM = ('M11.6 6.6 C14.2 6.6 16 7.4 16 9.4 L16 22.6 C16 24.6 14.2 25.4 11.6 25.4 '
          'M20.4 6.6 C17.8 6.6 16 7.4 16 9.4 M16 22.6 C16 24.6 17.8 25.4 20.4 25.4')

# ── Cursors: name -> (svg or [svg frames], hotspot in grid units) ────────────

CURSORS = {
    'default': (svg(arrow()), (7, 4.5)),
    'pointer': (svg(arrow('url(#accent)')), (7, 4.5)),
    'text': (svg(line(I_BEAM, 1.5)), (16, 16)),
    'vertical-text': (svg(f'<g transform="rotate(90 16 16)">{line(I_BEAM, 1.5)}</g>'), (16, 16)),
    'crosshair': (svg(line('M16 6 L16 12.6 M16 19.4 L16 26 M6 16 L12.6 16 M19.4 16 L26 16', 1.4) +
                      f'<circle cx="16" cy="16" r="1.5" fill="url(#body)" stroke="{PLUM}" stroke-width="1.1"/>'),
                  (16, 16)),
    'cell': (svg(line('M16 8.5 L16 23.5 M8.5 16 L23.5 16', 3.0, cap='round')), (16, 16)),
    'move': (svg(double_arrow(0, 19) + double_arrow(90, 19) +
                 f'<circle cx="16" cy="16" r="1.7" fill="url(#body)" stroke="{PLUM}" stroke-width="1.1"/>'),
             (16, 16)),
    'ew-resize': (svg(double_arrow(0)), (16, 16)),
    'ns-resize': (svg(double_arrow(90)), (16, 16)),
    'nesw-resize': (svg(double_arrow(-45)), (16, 16)),
    'nwse-resize': (svg(double_arrow(45)), (16, 16)),
    'col-resize': (svg(double_arrow(0, 20, line('M16 9.5 L16 22.5', 1.4))), (16, 16)),
    'row-resize': (svg(double_arrow(90, 20, line('M9.5 16 L22.5 16', 1.4))), (16, 16)),
    'up': (svg(single_arrow(-90)), (16, 9)),
    'down': (svg(single_arrow(90)), (16, 23)),
    'left': (svg(single_arrow(180)), (9, 16)),
    'right': (svg(single_arrow(0)), (23, 16)),
    'not-allowed': (svg(f'<circle cx="16" cy="16" r="7.6" fill="none" stroke="{PLUM}" stroke-width="5"/>'
                        f'<path d="M10.6 21.4 L21.4 10.6" stroke="{PLUM}" stroke-width="5" stroke-linecap="round"/>'
                        f'<circle cx="16" cy="16" r="7.6" fill="none" stroke="url(#wine)" stroke-width="2.4"/>'
                        f'<path d="M10.6 21.4 L21.4 10.6" stroke="url(#wine)" stroke-width="2.4" stroke-linecap="round"/>'),
                    (16, 16)),
    'no-drop': (svg(badge(f'<path d="M21.6 26.2 L26.8 21" stroke="{PLUM}" stroke-width="1.5" stroke-linecap="round"/>',
                          'url(#wine)')), (7, 4.5)),
    'grab': (svg(f'<circle cx="16" cy="16" r="6.4" fill="none" stroke="{PLUM}" stroke-width="4.4"/>'
                 f'<circle cx="16" cy="16" r="6.4" fill="none" stroke="url(#body)" stroke-width="2.1"/>'
                 f'<circle cx="16" cy="16" r="1.4" fill="url(#body)" stroke="{PLUM}" stroke-width="1"/>'),
             (16, 16)),
    'grabbing': (svg(f'<circle cx="16" cy="16" r="5.2" fill="url(#accent)" stroke="{PLUM}" stroke-width="1.4"/>'
                     f'<circle cx="16" cy="16" r="2.1" fill="#fbf5fc" opacity="0.55"/>'),
                 (16, 16)),
    'help': (svg(badge(f'<path d="M22.4 22 C22.4 20.3 26 20.3 26 22 C26 23.3 24.2 23.4 24.2 24.7" '
                       f'fill="none" stroke="{PLUM}" stroke-width="1.35" stroke-linecap="round"/>'
                       f'<circle cx="24.2" cy="26.6" r="0.8" fill="{PLUM}"/>')), (7, 4.5)),
    'context-menu': (svg(badge(f'<path d="M21.9 21.9 L26.5 21.9 M21.9 23.6 L26.5 23.6 M21.9 25.3 L26.5 25.3" '
                               f'stroke="{PLUM}" stroke-width="1.1" stroke-linecap="round"/>')), (7, 4.5)),
    'copy': (svg(badge(f'<path d="M24.2 21.2 L24.2 26 M21.8 23.6 L26.6 23.6" stroke="{PLUM}" '
                       f'stroke-width="1.4" stroke-linecap="round"/>')), (7, 4.5)),
    'alias': (svg(badge(f'<path d="M22.2 25.6 L26 21.8 M23.2 21.6 L26.2 21.6 L26.2 24.6" fill="none" '
                        f'stroke="{PLUM}" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/>')),
              (7, 4.5)),
    'zoom-in': (svg(line('M19.6 19.6 L25 25', 2.4) +
                    f'<circle cx="14.2" cy="14.2" r="6.6" fill="#fbf5fc" fill-opacity="0.18" stroke="{PLUM}" stroke-width="4.3"/>'
                    f'<circle cx="14.2" cy="14.2" r="6.6" fill="none" stroke="url(#body)" stroke-width="2"/>'
                    + line('M14.2 11.2 L14.2 17.2 M11.2 14.2 L17.2 14.2', 1.3)), (14, 14)),
    'zoom-out': (svg(line('M19.6 19.6 L25 25', 2.4) +
                     f'<circle cx="14.2" cy="14.2" r="6.6" fill="#fbf5fc" fill-opacity="0.18" stroke="{PLUM}" stroke-width="4.3"/>'
                     f'<circle cx="14.2" cy="14.2" r="6.6" fill="none" stroke="url(#body)" stroke-width="2"/>'
                     + line('M11.2 14.2 L17.2 14.2', 1.3)), (14, 14)),
    'pencil': (svg(f'<path d="M7 25 L8.4 19.8 L20.6 7.6 Q22 6.2 23.4 7.6 L24.4 8.6 Q25.8 10 24.4 11.4 '
                   f'L12.2 23.6 Z" fill="url(#body)" stroke="{PLUM}" stroke-width="1.4" stroke-linejoin="round"/>'
                   f'<path d="M18.8 9.4 L22.6 13.2" stroke="{PLUM}" stroke-width="1.1" stroke-linecap="round"/>'),
               (7, 25)),
    'wait': ([svg(spinner(16, 16, 7.4, 2.4, p, a)) for p, a in spin_frames(lambda p, a: (p, a))], (16, 16)),
    'progress': ([svg(arrow() + spinner(24, 23.6, 4.4, 1.9, p, a))
                  for p, a in spin_frames(lambda p, a: (p, a))], (7, 4.5)),
}

ALIASES = {
    'default': ['arrow', 'left_ptr', 'top_left_arrow', 'dnd-move', 'middle_finger', 'rock_and_roll'],
    'pointer': ['hand', 'hand1', 'hand2', 'pointing_hand', 'e29285e634086352946a0e7090d73106',
                '9d800788f1b08800ae810202380a0822'],
    'text': ['xterm', 'ibeam'],
    'crosshair': ['cross', 'cross_reverse', 'diamond_cross', 'target', 'tcross', 'color-picker', 'pipette'],
    'cell': ['plus'],
    'move': ['all-scroll', 'all-resize', 'fleur', 'size_all', '4498f0e0c1937ffe01fd06f973665830',
             '9081237383d90e509aa00f00170e968f'],
    'ew-resize': ['e-resize', 'w-resize', 'h_double_arrow', 'sb_h_double_arrow', 'size_hor',
                  'left_side', 'right_side', '028006030e0e7ebffc7f7070c0600140',
                  '14fef782d02440884392942c11205230'],
    'ns-resize': ['n-resize', 's-resize', 'v_double_arrow', 'sb_v_double_arrow', 'size_ver',
                  'top_side', 'bottom_side', '00008160000006810000408080010102',
                  '2870a09082c103050810ffdffffe0204'],
    'nesw-resize': ['ne-resize', 'sw-resize', 'fd_double_arrow', 'size_bdiag', 'top_right_corner',
                    'bottom_left_corner', 'fcf1c3c7cd4491d801f1e1c78f100000'],
    'nwse-resize': ['nw-resize', 'se-resize', 'bd_double_arrow', 'size_fdiag', 'top_left_corner',
                    'bottom_right_corner', 'c7088f0f3e6c8088236ef8e1e3e70000'],
    'col-resize': ['split_h'],
    'row-resize': ['split_v'],
    'up': ['up-arrow', 'sb_up_arrow'],
    'down': ['down-arrow', 'sb_down_arrow'],
    'left': ['left-arrow', 'sb_left_arrow'],
    'right': ['right-arrow', 'sb_right_arrow'],
    'not-allowed': ['circle', 'crossed_circle', 'forbidden', 'X_cursor', 'pirate', 'kill', 'skull',
                    'dnd-none', '03b6e0fcb3499374a867c041f52298f0'],
    'no-drop': ['dnd-no-drop'],
    'grab': ['openhand', '5aca4d189052212118709018842178c0', '9141b49c8149039304290b508d208c40'],
    'grabbing': ['closedhand', 'dnd-drag', '208530c400c041818281048008011002'],
    'help': ['question_arrow', 'whats_this', 'left_ptr_help', 'dnd-help', 'dnd-ask',
             '5c6cd98b3f3ebcb1f9c7f1c204630408', 'd9ce0ab605698f320427677b458ad60b'],
    'copy': ['dnd-copy', '1081e37283d90000800003c07f3ef6bf', '6407b0e94181790501fd1e167b474872',
             'b66166c04f8c3109214a4fbd64a50fc8'],
    'alias': ['link', 'dnd-link', '3085a0e285430894940527032f8b26df', '640fb0e74195791501fd1ed57b41487f',
              'a2a266d0498c3104214a47bd64ab0fc8'],
    'pencil': ['pen', 'draft'],
    'wait': ['watch'],
    'progress': ['left_ptr_watch', 'half-busy', '08e8e1c95fe2fc01f976f1e063a24ccd',
                 '3ecb610c1bf2410f44200f48c40d3599', '00000000000000020006000e7e9ffc3f'],
}


# ── Rendering and Xcursor packing ───────────────────────────────────────────

def render(svg_text, px):
    png = subprocess.run(['rsvg-convert', '-w', str(px), '-h', str(px)], input=svg_text.encode(),
                         capture_output=True, check=True).stdout
    from PIL import Image
    import io
    return Image.open(io.BytesIO(png)).convert('RGBA')


def premultiplied_bgra(img):
    # Xcursor pixels are premultiplied ARGB32, little-endian (bytes B G R A).
    return img.convert('RGBa').tobytes('raw', 'BGRa')


def xcursor(frames_by_size, hot, delay):
    """frames_by_size: {nominal: [PIL images]} -> Xcursor bytes."""
    chunks = []
    for nominal, frames in frames_by_size.items():
        k = nominal / 32
        hx, hy = round(hot[0] * k), round(hot[1] * k)
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


def main():
    shutil.rmtree(DST, ignore_errors=True)
    os.makedirs(os.path.join(DST, 'cursors'))
    for name, (art, hot) in CURSORS.items():
        frames = art if isinstance(art, list) else [art]
        by_size = {s: [render(f, s) for f in frames] for s in SIZES}
        with open(os.path.join(DST, 'cursors', name), 'wb') as f:
            f.write(xcursor(by_size, hot, 25 if len(frames) > 1 else 0))
        for alias in ALIASES.get(name, []):
            os.symlink(name, os.path.join(DST, 'cursors', alias))
    with open(os.path.join(DST, 'index.theme'), 'w') as f:
        f.write('[Icon Theme]\nName=midve\nComment=midve desktop cursors: lilac on plum\n')
    print(DST)


if __name__ == '__main__':
    main()
