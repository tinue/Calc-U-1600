#!/usr/bin/env python3
"""Generates the Calc-U-1600 app icon: a simplified PC-1600 (screen, "1600"
logo, QWERTY keys, number keys) with a stylized landscape demo on the LCD.

    python3 source-material/Calc-U-1600_icon.py

writes the master source-material/Calc-U-1600_icon.svg and the macOS Icon
Composer layer Qt6/resources/macos/AppIcon.icon/Assets/pc1600.svg (the same
without the backdrop; the icon.json fill replaces it). Then regenerate the
platform icons from the repo root:

    for s in 16 32 128 256; do
      rsvg-convert -w $s -h $s source-material/Calc-U-1600_icon.svg -o /tmp/i$s.png
    done
    magick /tmp/i16.png /tmp/i32.png /tmp/i128.png /tmp/i256.png \
      Qt6/resources/windows/calc-u-1600.ico
    cp /tmp/i256.png Qt6/resources/linux/calc-u-1600.png
    xcrun actool Qt6/resources/macos/AppIcon.icon --compile Qt6/resources/macos \
      --app-icon AppIcon --platform macosx --minimum-deployment-target 15.8 \
      --target-device mac --output-partial-info-plist /dev/null
"""
import pathlib

BACKDROP = """  <g id="backdrop">
    <path fill="{bg}" d="M 870.03 0.00 Q 873.25 0.32 876.49 0.57 C 954.63 6.69 1017.19 68.55 1023.40 147.26 Q 1023.66 150.48 1024.00 153.68 L 1024.00 870.15 Q 1023.65 873.94 1023.33 877.74 C 1016.82 955.10 955.20 1016.65 877.98 1023.31 Q 874.11 1023.64 870.24 1024.00 L 153.84 1024.00 Q 150.56 1023.67 147.25 1023.41 C 104.45 1020.07 65.28 999.76 37.70 967.07 Q 5.35 928.73 0.72 877.74 Q 0.38 874.02 0.00 870.32 L 0.00 153.86 Q 2.82 97.96 37.55 57.03 Q 82.38 4.19 153.74 0.00 Z"/>
  </g>"""


def digits_1600(x, y, h, stroke, color):
    w = h * 0.62
    gap = h * 0.28
    s = stroke / 2
    out = []
    cx = x + w * 0.62
    out.append(f'M {x + w*0.18:.1f} {y + h*0.22:.1f} L {cx:.1f} {y + s:.1f} V {y + h:.1f}')
    x += w * 0.62 + gap
    out.append(f'M {x + w - s:.1f} {y + s:.1f} H {x + s:.1f} V {y + h - s:.1f} H {x + w - s:.1f} '
               f'V {y + h*0.52:.1f} H {x + s:.1f}')
    x += w + gap
    for _ in range(2):
        out.append(f'M {x + s:.1f} {y + s:.1f} H {x + w - s:.1f} V {y + h - s:.1f} H {x + s:.1f} Z')
        x += w + gap
    d = ' '.join(out)
    return (f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{stroke}" '
            f'stroke-linejoin="miter" stroke-linecap="butt" '
            f'transform="translate({0.1763 * (y + h / 2):.1f} 0) skewX(-10)"/>'), x - gap


def keyrow(x0, y, widths, h, gap, fill, rx, special=None):
    special = special or {}
    out, x = [], x0
    for i, w in enumerate(widths):
        f = special.get(i, fill)
        out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" fill="{f}"/>')
        x += w + gap
    return out


def build(v):
    C = v['colors']
    BW, BH = 900, v['body_h']
    bx, by = (1024 - BW) / 2, (1024 - BH) / 2
    pad = 34
    split = bx + pad + v['left_w']
    rx0 = split + v['mid_gap']
    rx1 = bx + BW - pad
    top = by + pad
    bot = by + BH - pad
    parts = [f'<rect x="{bx}" y="{by}" width="{BW}" height="{BH}" rx="40" fill="{C["body"]}"/>']

    # Screen (top left)
    sh = v['screen_h']
    sx0 = bx + pad
    parts.append(f'<rect x="{sx0}" y="{top}" width="{split - sx0}" height="{sh}" rx="18" fill="{C["dark"]}"/>')
    b = v['bezel']
    lx, ly, lw, lh = sx0 + b, top + b, split - sx0 - 2 * b, sh - 2 * b
    parts.append(f'<rect x="{lx}" y="{ly}" width="{lw}" height="{lh}" rx="6" fill="{C["lcd"]}"/>')
    ink = C['ink']

    # Stylized PC-1600 landscape demo: ridge line, town silhouette, dotted ground.
    def X(x):
        return lx + 22 + (x - 155) / 890 * (lw - 44)

    def Y(f):
        return ly + f * lh

    def ridgeY(y):
        return Y(0.10 + (y - 105) / 95 * 0.46)
    ridge = [(185, 200), (160, 140), (275, 105), (400, 190), (428, 128), (455, 168), (560, 145),
             (628, 110), (700, 180), (750, 193), (850, 140), (880, 168), (905, 128), (935, 158),
             (960, 118), (1010, 128), (1040, 155), (1020, 200)]
    pts = ' '.join(f'{X(x):.1f},{ridgeY(y):.1f}' for x, y in ridge)
    parts.append(f'<polyline points="{pts}" fill="none" stroke="{ink}" stroke-width="{v["ls_stroke"]}" '
                 f'stroke-linejoin="round" stroke-linecap="round"/>')
    base = Y(0.80)
    for x0, x1, roof in [(185, 340, 0.56), (340, 415, 0.64), (460, 525, 0.64),
                         (575, 900, 0.56), (915, 1020, 0.52)]:
        parts.append(f'<rect x="{X(x0):.1f}" y="{Y(roof):.1f}" width="{X(x1) - X(x0):.1f}" '
                     f'height="{base - Y(roof):.1f}" fill="{ink}"/>')
    for tx in (437, 550):
        r = lh * 0.09
        cy = Y(0.56)
        parts.append(f'<circle cx="{X(tx):.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="{ink}"/>')
        parts.append(f'<rect x="{X(tx) - r*0.3:.1f}" y="{cy:.1f}" width="{r*0.6:.1f}" height="{base - cy:.1f}" fill="{ink}"/>')
    d = v['ls_dot']
    gy = Y(0.90)
    parts.append(f'<line x1="{X(165):.1f}" y1="{gy:.1f}" x2="{X(1040):.1f}" y2="{gy:.1f}" stroke="{ink}" '
                 f'stroke-width="{d}" stroke-dasharray="{d} {d}"/>')

    # Logo panel (top right)
    lph = v['logo_h']
    parts.append(f'<rect x="{rx0}" y="{top}" width="{rx1 - rx0}" height="{lph}" rx="12" fill="{C["dark"]}"/>')
    dh = min(lph, 112) * 0.58  # digits fit the panel width; a taller panel only adds space
    w = dh * 0.62; gap = dh * 0.28
    total = w * 0.62 + gap + 3 * w + 2 * gap
    dx = rx0 + (rx1 - rx0 - total) / 2 + dh * 0.05
    path, _ = digits_1600(dx, top + (lph - dh) / 2, dh, v['logo_stroke'], C['logo'])
    parts.append(path)

    # Number keys (bottom right)
    kgap = v['kgap']
    ny0 = top + lph + v['logo_gap']
    cols, rows = v['num_grid']
    kw = (rx1 - rx0 - (cols - 1) * kgap) / cols
    # the top row is shorter (like the real BS / SHIFT / OFF / ON row)
    t = v['num_top_ratio']
    # and set apart by an extra gap, which squashes the rows below it
    kh = (bot - ny0 - (rows - 1) * kgap) / (rows - 1 + t)
    top_h = kh * t
    kh = (bot - ny0 - top_h - v['num_top_gap'] - (rows - 1) * kgap) / (rows - 1)
    y = ny0
    for r in range(rows):
        h = top_h if r == 0 else kh
        special = {cols - 1: C['red']} if r == v['red_row'] else {}
        parts += keyrow(rx0, y, [kw] * cols, h, kgap, C['numkey'], 9, special)
        y += h + kgap + (v['num_top_gap'] if r == 0 else 0)

    # QWERTY keys (bottom left)
    qy0 = top + sh + v['screen_gap']
    qrows = v['q_rows']
    qh = (bot - qy0 - (qrows - 1) * kgap) / qrows
    avail = split - sx0
    n = v['q_cols']
    qw = (avail - (n - 1) * kgap) / n
    for r in range(qrows):
        y = qy0 + r * (qh + kgap)
        if r == qrows - 1:
            widths = [qw, qw, avail - 4 * qw - 3 * kgap, qw, qw]
            parts += keyrow(sx0, y, widths, qh, kgap, C['qkey'], 7, {0: C['orange']})
        else:
            off = (qw + kgap) * v['stagger'][r]
            m = n - (1 if v['stagger'][r] else 0)
            parts += keyrow(sx0 + off, y, [qw] * m, qh, kgap, C['qkey'], 7)

    body = '\n    '.join(parts)
    backdrop = BACKDROP.format(bg=C['bg']) + '\n' if v.get('backdrop', True) else ''
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">\n'
            f'{backdrop}  <g id="pc1600">\n    {body}\n  </g>\n</svg>\n')


BASE = dict(
    body_h=520, left_w=540, mid_gap=34, screen_h=235, bezel=22, screen_gap=30,
    logo_h=139, logo_gap=30, logo_stroke=11, kgap=14,
    num_grid=(4, 4), num_top_ratio=0.6, num_top_gap=30, red_row=1,
    q_rows=3, q_cols=7, stagger=[0, 0.5, 0], ls_stroke=11, ls_dot=12,
    colors=dict(bg='#2d271f', body='#b9bdc2', dark='#26282b', lcd='#a9b98a', ink='#26282b',
                qkey='#26282b', numkey='#f4f5f6', logo='#eceef0', orange='#ef8a1f', red='#d23a2c'),
)

if __name__ == '__main__':
    root = pathlib.Path(__file__).resolve().parent.parent
    (root / 'source-material/Calc-U-1600_icon.svg').write_text(build(BASE))
    (root / 'Qt6/resources/macos/AppIcon.icon/Assets/pc1600.svg').write_text(build(dict(BASE, backdrop=False)))
