#!/usr/bin/env python3
"""Mantis Rally cockpits from cockpits_sheet.jpg (Panels 1-4). The glass becomes transparent; the dash keeps its
proportions at the bottom of the 160x120 frame and the pillars above it stretch to the top, so the interior takes
about a quarter of the screen. The steering wheel's pixels are marked so the game can turn it.
Writes src/cockpit_art.h. Run: python3 make_cockpits.py"""
import numpy as np, os
from PIL import Image
from scipy import ndimage
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'raw', 'cockpit_art.h')            # packed into src/art_packed.h by pack_art.py
PANELS = [(58, 25, 681, 343), (755, 25, 1363, 343), (58, 409, 681, 713), (755, 409, 1363, 713)]
W, H = 160, 120
DASH_ROWS = 31                         # how much of the screen the dash may take
# steering wheel (centre x, y, radius) in output pixels; the bike's bars don't spin
WHEEL = [(43, 105, 18), (47, 105, 27), (45, 94, 21), None]
from make_vehicles import quantize_palette

def build(box):
    im = Image.open(os.path.join(HERE, 'cockpits_sheet.jpg')).convert('RGB').crop(box)
    c = np.asarray(im).astype(np.float32)
    m = c.max(2); sat = m - c.min(2)
    glass = (m > 200) & (sat < 26)
    glass = ndimage.binary_opening(glass, iterations=1)
    rows = glass[:, c.shape[1] // 3: 2 * c.shape[1] // 3].mean(1)
    dash = max(y for y in range(c.shape[0]) if rows[y] > 0.5) + 1
    bot = Image.fromarray(c[dash:].astype(np.uint8))
    hb = DASH_ROWS
    top = Image.fromarray(c[:dash].astype(np.uint8)).resize((W, H - hb), Image.LANCZOS)
    botr = bot.resize((W, hb), Image.LANCZOS)
    gt = Image.fromarray((glass[:dash] * 255).astype(np.uint8)).resize((W, H - hb), Image.BILINEAR)
    gb = Image.fromarray((glass[dash:] * 255).astype(np.uint8)).resize((W, hb), Image.BILINEAR)
    rgb = np.vstack([np.asarray(top), np.asarray(botr)]).astype(np.float32)
    gl = np.vstack([np.asarray(gt), np.asarray(gb)]) > 110
    return rgb, gl

# ---- the Stingray's wheel ----------------------------------------------------------------------------------------
# The panels are scaled harder above the dash line than below it (to fit the glass and the dash to the screen), and
# the Stingray's wheel straddles that line: its top came out stretched into a pointed arch. The wheel alone is drawn
# again from the sheet at one even scale (the panel's own width scale, so it's the same size), sitting where it sat;
# every other pixel of the cockpit stays exactly as it was. What the old wheel covered and the new one doesn't takes
# the cockpit beside it in the same row.
VETTE_WHEEL = dict(cx=234.5, cy=619.8, rx=83.5, ry=85.2, rim=16.0,        # the rim (sheet pixels): centre, outer radii, width
                   spoke_y=(607, 631), hub=(234.0, 624.0, 20.0), bottom_x=(224, 245), bottom_y=(640, 693),
                   bottom_at=705.0, screen_bottom=120.0)

def _wheel_mask(X, Y, w):
    """Sheet pixels that are the wheel (rim, side spokes, hub, bottom spoke)."""
    dx, dy = (X - w['cx']) / w['rx'], (Y - w['cy']) / w['ry']
    d = np.sqrt(dx * dx + dy * dy)
    inner = 1.0 - w['rim'] / ((w['rx'] + w['ry']) * 0.5)
    rim = (d <= 1.0) & (d >= inner)
    inside = d < inner
    side = inside & (Y >= w['spoke_y'][0]) & (Y <= w['spoke_y'][1])
    hx, hy, hr = w['hub']
    hub = (X - hx) ** 2 + (Y - hy) ** 2 <= hr * hr
    bottom = inside & (X >= w['bottom_x'][0]) & (X <= w['bottom_x'][1]) & (Y >= w['bottom_y'][0]) & (Y <= w['bottom_y'][1])
    return rim | side | hub | bottom

def vette_wheel(q, pal, box):
    w = VETTE_WHEEL
    src = np.asarray(Image.open(os.path.join(HERE, 'cockpits_sheet.jpg')).convert('RGB')).astype(np.float32)
    x0, y0 = box[0], box[1]
    pw, ph = box[2] - box[0], box[3] - box[1]
    sx = W / pw
    # where the old (two-scale) layout put the sheet: the split row, as build() finds it
    c = src[y0:box[3], x0:box[2]]
    m = c.max(2); sat = m - c.min(2)
    glass = ndimage.binary_opening((m > 200) & (sat < 26), iterations=1)
    rows = glass[:, c.shape[1] // 3: 2 * c.shape[1] // 3].mean(1)
    dash = max(y for y in range(c.shape[0]) if rows[y] > 0.5) + 1
    def old_src_y(gy):                                   # screen row (continuous) -> sheet row, the old way
        top = H - DASH_ROWS
        return np.where(gy < top, gy * dash / top, dash + (gy - top) * (ph - dash) / DASH_ROWS) + y0
    def new_src_y(gy):                                   # the even scale, the wheel's bottom where it was
        return w['bottom_at'] + (gy - w['screen_bottom']) / sx
    S = 4                                                # samples per pixel, each way
    offs = (np.arange(S) + 0.5) / S
    newW = np.zeros((H, W), bool); oldW = np.zeros((H, W), bool); col = np.zeros((H, W, 3), np.float32)
    gx0 = int((w['cx'] - w['rx'] - 4 - x0) * sx); gx1 = int((w['cx'] + w['rx'] + 4 - x0) * sx) + 1
    for gy in range(60, H):
        for gx in range(max(0, gx0), min(W, gx1)):
            GX, GY = np.meshgrid(gx + offs, gy + offs)
            X = x0 + GX / sx
            mn = _wheel_mask(X, new_src_y(GY), w)
            if mn.mean() >= 0.5:
                Yn = new_src_y(GY)
                xi = np.clip(X[mn].astype(int), 0, src.shape[1] - 1); yi = np.clip(Yn[mn].astype(int), 0, src.shape[0] - 1)
                col[gy, gx] = src[yi, xi].mean(0); newW[gy, gx] = True
            if _wheel_mask(X, old_src_y(GY), w).mean() > 0.1: oldW[gy, gx] = True     # any real part of the old wheel
    # the old rim's outline against the glass, one pixel beyond it
    top = H - DASH_ROWS
    grow = ndimage.binary_dilation(oldW, structure=np.ones((3, 3), bool))
    grow[top:] = False
    oldW |= grow & (q > 0)
    out = q.copy()
    # the new wheel, in the cockpit's own colours
    ys, xs = np.nonzero(newW)
    d = ((col[ys, xs][:, None, :] - pal[None]) ** 2 * [0.3, 0.59, 0.11]).sum(2)
    out[ys, xs] = d.argmin(1) + 1
    # where only the old wheel was: the cockpit beside it in the same row
    for gy, gx in zip(*np.nonzero(oldW & ~newW)):
        for r in range(1, W):
            cand = [gx - r, gx + r]
            ok = [x for x in cand if 0 <= x < W and not oldW[gy, x] and not newW[gy, x]]
            if ok: out[gy, gx] = q[gy, ok[0]]; break
    return out

def main():
    out = []
    for k, box in enumerate(PANELS):
        rgb, gl = build(box)
        pal = quantize_palette(rgb[~gl].reshape(-1, 3), vivid_sat=60)
        d = ((rgb.reshape(-1, 1, 3) - pal[None]) ** 2 * [0.3, 0.59, 0.11]).sum(2)
        q = (d.argmin(1) + 1).reshape(H, W).astype(np.uint8)
        q[gl] = 0
        # speckle: lone opaque pixels in the glass go
        op = q > 0
        lone = op & (ndimage.convolve(op.astype(int), np.ones((3, 3)), mode='constant') <= 2)
        q[lone] = 0
        if k == 2: q = vette_wheel(q, pal, box)          # the Stingray's wheel, round again
        out.append((q, pal))
        P = np.vstack([[90, 140, 200], pal]).astype(np.uint8)
        Image.fromarray(P[q]).resize((480, 360), Image.NEAREST).save(os.path.join(HERE, 'cockpit_preview_%d.png' % k))
    with open(OUT, 'w') as f:
        f.write('// Generated by tools/sprites/make_cockpits.py from tools/sprites/cockpits_sheet.jpg. Do not edit.\n')
        f.write('#pragma once\n#include <stdint.h>\nnamespace mantis {\nnamespace art {\n')
        names = ['RALLY', 'JEEP', 'VETTE', 'BIKE']
        for (q, pal), nm, wh in zip(out, names, WHEEL):
            flat = q.ravel()
            data = ((flat[0::2] << 4) | flat[1::2]).tolist()
            f.write('static const uint8_t CK_%s_PAL[45] = {%s};\n' % (nm, ','.join(str(int(v)) for v in pal.ravel())))
            f.write('static const uint8_t CK_%s_PX[%d] = {\n' % (nm, len(data)))
            for i in range(0, len(data), 48): f.write('  ' + ','.join(str(v) for v in data[i:i + 48]) + ',\n')
            f.write('};\n')
        f.write('struct CockArt { const uint8_t* pal; const uint8_t* px; int8_t wx, wy, wr; };\n')
        f.write('static const CockArt COCK_ART[4] = {\n')
        for nm, wh in zip(names, WHEEL):
            wx, wy, wr = wh if wh else (0, 0, 0)
            f.write('  {CK_%s_PAL, CK_%s_PX, %d, %d, %d},\n' % (nm, nm, wx, wy, wr))
        f.write('};\n}\n}\n')

if __name__ == '__main__':
    main()
