#!/usr/bin/env python3
"""Mantis Rally vehicle sprites from the definitive sprite sheet (vehicles_sheet.jpg).

Each vehicle's SIDE PROFILE and REAR VIEW are carved against each other into a voxel model (a visual hull),
textured from those same two drawings, and rendered with true perspective from the game's chase camera:
yaw steps (mirrored at runtime) x pitch (uphill / level / downhill), and lean for the bike. Output:
src/vehicle_art.h (4-bit frames + a 15-colour palette per vehicle). Re-run after editing the sheet:

    python3 make_vehicles.py            (needs numpy, scipy, pillow)
"""
import numpy as np, os, sys, math
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
SHEET = os.path.join(HERE, 'vehicles_sheet.jpg')
ANGLES = os.path.join(HERE, 'angles_sheet.png')
# TOP column of the angles sheet (middle row): box, and how it is turned ('r' front right, 'l' front left, 'u' front up)
TOPS = [((1420, 114, 1519, 179), 'r'), ((1419, 373, 1520, 443), 'l'), ((1414, 639, 1520, 699), 'l'), ((1436, 889, 1502, 956), 'u')]
OUT = os.path.join(HERE, '..', '..', 'src', 'vehicle_art.h')

# (side box, rear box) in the sheet, real length / width / height (m)
VEH = [
    ('STI',   (69, 139, 524, 311), (604, 115, 826, 311), 4.42, 1.80, 1.48, True),
    ('JEEP',  (67, 370, 493, 601), (593, 359, 837, 601), 3.90, 1.80, 1.95, True),
    ('VETTE', (49, 675, 536, 904), (581, 672, 849, 903), 4.60, 2.05, 1.85, True),
    ('BIKE',  (96, 968, 419, 1164), (650, 950, 790, 1164), 2.15, 0.82, 1.25, False),
]
# the game's chase camera (must match render.cpp)
F = 120.0                # chase focal length, framebuffer pixels
CAM_BACK = 6.6           # metres behind the vehicle's centre (~4.4 m behind a car's bumper)
CAM_UP = 1.65            # metres above the ground
CAM_PITCH = math.radians(-5.0)
BIKE_BACK = 3.9          # the bike's chase camera sits closer
SS = 4                   # supersampling

def load_view(img, box, fill):
    x0, y0, x1, y1 = box
    c = np.asarray(img.crop((x0 - 4, y0 - 4, x1 + 4, y1 + 8))).astype(np.float32)
    mx = c.max(2); sat = mx - c.min(2)
    fg = ~((mx > 150) & (sat < 30))
    # drop the ground line (rows that are mostly 'ink' across the whole crop)
    rows = fg.mean(1)
    y = fg.shape[0] - 1
    while y > fg.shape[0] * 0.75 and rows[y] < 0.05: y -= 1          # margin under the line
    while y > fg.shape[0] * 0.75 and rows[y] > 0.45: fg[y, :] = False; y -= 1
    fg = ndimage.binary_opening(fg, iterations=1)
    lab, n = ndimage.label(ndimage.binary_closing(fg, iterations=2))
    sizes = ndimage.sum(np.ones_like(lab), lab, range(1, n + 1))
    keep = np.zeros_like(fg)
    big = sizes.max()
    for i, s in enumerate(sizes):
        if s > big * 0.03:
            sl = ndimage.find_objects((lab == i + 1).astype(int))[0]
            if sl[0].start > 6 or s > big * 0.5: keep |= (lab == i + 1)
    m = keep & fg
    if fill: m = ndimage.binary_fill_holes(ndimage.binary_closing(m, iterations=3))
    ys, xs = np.where(m)
    m = m[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    c = c[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    return c, m

def load_top(vi, nx, nz):
    img = Image.open(ANGLES).convert('RGB')
    (x0, y0, x1, y1), turn = TOPS[vi]
    c = np.asarray(img.crop((x0, y0, x1, y1))).astype(np.float32)
    m = c.max(2) > 40
    m = ndimage.binary_fill_holes(ndimage.binary_closing(m, iterations=2))
    ys, xs = np.where(m)
    c = c[ys.min():ys.max() + 1, xs.min():xs.max() + 1]; m = m[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    if turn == 'l': c = c[:, ::-1]; m = m[:, ::-1]
    if turn == 'u': c = np.rot90(c, -1); m = np.rot90(m, -1)
    # image columns = length (rear -> front), rows = width (left -> right as seen from above, front right)
    T = np.asarray(Image.fromarray(c.astype(np.uint8)).resize((nx, nz), Image.LANCZOS)).astype(np.float32)
    Mt = np.asarray(Image.fromarray((m * 255).astype(np.uint8)).resize((nx, nz), Image.BILINEAR)) > 100
    return T, Mt

def build(name, side_box, rear_box, L, Wd, Hh, fill):
    img = Image.open(SHEET).convert('RGB')
    S, Ms = load_view(img, side_box, fill)
    if name == 'JEEP':                       # the spare tyre hangs off the back in profile; the rear view hides it
        cut = int(S.shape[1] * 0.085); S = S[:, cut:]; Ms = Ms[:, cut:]
    R, Mr = load_view(img, rear_box, fill)
    # voxel size: the side drawing's pixels at its height
    vox = Hh / S.shape[0]
    nx, ny = S.shape[1], S.shape[0]
    nz = max(4, int(round(Wd / vox)))
    # rear drawing resampled to (ny, nz)
    Rr = np.asarray(Image.fromarray(R.astype(np.uint8)).resize((nz, ny), Image.LANCZOS)).astype(np.float32)
    Mrr = np.asarray(Image.fromarray((Mr * 255).astype(np.uint8)).resize((nz, ny), Image.BILINEAR)) > 127
    # side drawing's length should match the real length (it is drawn true to scale already; keep its pixels)
    vi = [v[0] for v in VEH].index(name)
    T, Mt = load_top(vi, nx, nz)
    # the plan view only trims what's above the wheels' tops (a top view can't see the tyres' undersides)
    occ = Ms[:, :, None] & Mrr[:, None, :]           # [y, x, z], y = 0 at the roof
    upper = np.arange(ny)[:, None, None] < ny * 0.55
    occ &= ~(upper & ~Mt.T[None, :, :])
    if fill:                                          # holes the drawings leave (windows): darken, keep solid
        pass
    # surface + normals
    sm = ndimage.gaussian_filter(occ.astype(np.float32), 1.2)
    gy, gx, gz = np.gradient(sm)
    er = occ & ~ndimage.binary_erosion(occ)
    ys, xs, zs = np.where(er)
    n = -np.stack([gx[ys, xs, zs], -gy[ys, xs, zs], gz[ys, xs, zs]], 1)   # x fwd, y up, z right
    nn = np.linalg.norm(n, axis=1, keepdims=True) + 1e-6
    n = n / nn
    # colour by the facing view
    side_c = S[np.clip(ys, 0, ny - 1), xs]
    side_below = S[np.clip(ys + 3, 0, ny - 1), xs]
    rear_c = Rr[ys, np.clip(zs, 0, nz - 1)]
    ax, ayy, az = np.abs(n[:, 0]), n[:, 1], np.abs(n[:, 2])
    col = side_c.copy()
    # anything that faces backwards at all is projected from the rear drawing (it IS the view from behind);
    # sideways surfaces from the side profile; roofs, bonnets and boots from the rear drawing's roofline
    rear = n[:, 0] < -0.28
    col[rear] = rear_c[rear]
    rtop = np.zeros((nz, 3), np.float32)
    for zz in range(nz):
        colm = np.where(Mrr[:, zz])[0]
        rtop[zz] = Rr[min(colm.min() + 4, ny - 1), zz] if len(colm) else (60, 60, 60)
    tc = rtop[np.clip(zs, 0, nz - 1)]
    top = ~rear & (ayy > az * 0.8)
    topc = T[np.clip(zs, 0, nz - 1), xs]
    col[top] = topc[top] * 0.75 + tc[top] * 0.25
    # hole fill: background-coloured texels (windows the drawings show through) -> dark glass
    mxc = col.max(1); satc = mxc - col.min(1)
    bgt = (mxc > 150) & (satc < 30)
    win = Rr[np.clip(ys, 0, ny - 1), nz // 2]                        # what the rear drawing has there
    col[bgt] = win[bgt] * 0.8
    # gentle light from above / behind-right (the art is already shaded)
    Ld = np.array([-0.35, 0.8, 0.45]); Ld /= np.linalg.norm(Ld)
    lam = 0.86 + 0.24 * (n @ Ld)
    lam = np.where(n[:, 1] < -0.5, 0.62, lam)
    col = np.clip(col * lam[:, None], 0, 255)
    # vehicle space (metres): x forward from centre, y up from ground, z right
    P = np.stack([(xs - nx / 2) * vox, (ny - ys) * vox, (zs - nz / 2) * vox], 1)
    return P, col, vox, S, R, Mr

def render(P, col, vox, yaw, pitch, lean, w, h, back=None):
    CAM_BACK_ = back if back else CAM_BACK
    # rotate the vehicle: lean about x, pitch about z, yaw about y
    cl, sl = math.cos(lean), math.sin(lean)
    x, y, z = P[:, 0], P[:, 1], P[:, 2]
    y2 = y * cl - z * sl; z2 = y * sl + z * cl          # lean right = +lean
    cp, sp = math.cos(pitch), math.sin(pitch)
    x3 = x * cp - y2 * sp; y3 = x * sp + y2 * cp         # nose up = +pitch
    cy, sy = math.cos(yaw), math.sin(yaw)
    x4 = x3 * cy - z2 * sy; z4 = x3 * sy + z2 * cy       # + yaw: nose turns right
    # camera behind (-x) looking +x, pitched down
    cpc, spc = math.cos(CAM_PITCH), math.sin(CAM_PITCH)
    dx = x4 + CAM_BACK_; dy = y3 - CAM_UP
    d2 = dx * cpc + dy * spc
    u2 = -dx * spc + dy * cpc
    f = F * SS
    W, H = w * SS, h * SS
    g_d = CAM_BACK_ * cpc - CAM_UP * spc; g_u = -CAM_BACK_ * spc - CAM_UP * cpc   # the ground under the centre
    anchor = (h - 3) * SS
    sx = z4 / d2 * f + W / 2
    sy_ = anchor - (u2 / d2 - g_u / g_d) * f
    rad = vox / d2 * f * 0.75 + 0.6
    img = np.zeros((H, W, 3), np.float32); zb = np.full((H, W), 1e9, np.float32)
    order = np.argsort(-d2)
    for o in (-1, 0, 1):
        pass
    xs = sx[order]; ys = sy_[order]; dd = d2[order]; cc = col[order]; rr = rad[order]
    for dx in (-0.5, 0.0, 0.5):
        for dy in (-0.5, 0.0, 0.5):
            px = np.round(xs + dx * rr * 2).astype(int); py = np.round(ys + dy * rr * 2).astype(int)
            ok = (px >= 0) & (px < W) & (py >= 0) & (py < H)
            idx = py[ok] * W + px[ok]
            dk = dd[ok]; ck = cc[ok]
            # nearest wins: sort by (pixel, depth) and keep the first of each pixel
            o = np.lexsort((dk, idx)); idx, dk, ck = idx[o], dk[o], ck[o]
            first = np.ones(len(idx), bool); first[1:] = idx[1:] != idx[:-1]
            idx, dk, ck = idx[first], dk[first], ck[first]
            flat = zb.reshape(-1); fimg = img.reshape(-1, 3)
            better = dk < flat[idx]
            flat[idx[better]] = dk[better]; fimg[idx[better]] = ck[better]
    alpha = (zb < 1e8).astype(np.float32)
    return img, alpha

def quantize_palette(cols, n=15, vivid_sat=110):
    # 10 tones for the body of the art, 5 reserved for its saturated accents (stripes, lights, trim)
    def mc(c, k):
        pim = Image.fromarray(c.reshape(1, -1, 3).astype(np.uint8))
        q = pim.quantize(colors=k, method=Image.MEDIANCUT)
        return np.array(q.getpalette()[:k * 3]).reshape(k, 3)
    sat = cols.max(1) - cols.min(1)
    vivid = cols[sat > vivid_sat]
    if len(vivid) < 50: return mc(cols, n)
    return np.vstack([mc(cols, n - 5), mc(vivid, 5)])

def downsample(img, alpha, pal, w, h):
    # quantize the supersampled image to the palette, then take the most common index per block
    H, W = alpha.shape
    flat = img.reshape(-1, 3)
    d = ((flat[:, None, :] - pal[None, :, :]) ** 2 * np.array([0.3, 0.59, 0.11])).sum(2)
    q = d.argmin(1).reshape(H, W) + 1
    q[alpha < 0.5] = 0
    out = np.zeros((h, w), np.uint8)
    for y in range(h):
        for x in range(w):
            b = q[y * SS:(y + 1) * SS, x * SS:(x + 1) * SS].ravel()
            cnt = np.bincount(b, minlength=16)
            if cnt[0] > SS * SS * 0.55: continue
            cnt[0] = 0
            out[y, x] = cnt.argmax()
    return out

def outline(fr, pal):
    # pixel-art edge: silhouette pixels take the darkest nearby tone
    lum = (pal * [0.3, 0.59, 0.11]).sum(1)
    dark = int(np.argmin(lum)) + 1
    o = fr.copy()
    m = fr > 0
    edge = m & ~ndimage.binary_erosion(m)
    for y, x in zip(*np.where(edge)):
        if lum[fr[y, x] - 1] > lum[dark - 1] + 40: o[y, x] = dark
    return o

def main():
    sheet_out = []
    frames_c = []
    anchors = []
    pal_c = []
    for vi, (name, sb, rb, L, Wd, Hh, fill) in enumerate(VEH):
        P, col, vox, S, R, Mr = build(name, sb, rb, L, Wd, Hh, fill)
        bike = name == 'BIKE'
        w, h = (64, 72) if bike else (80, 56)
        pal = quantize_palette(np.concatenate([S.reshape(-1, 3)[::3], R.reshape(-1, 3)[::3], col[::2]]))
        frames = []
        if bike:
            # from behind a bike is nearly flat: leaning it is a rotation of the rear drawing about the tyre's
            # contact patch. Rotate the drawing itself at 4x and reduce: every detail of the art survives.
            th = int(round(Hh * F / BIKE_BACK))                  # bike height in game pixels at the bike camera
            sc = th * SS / R.shape[0]
            rw, rh = max(2, int(R.shape[1] * sc)), th * SS
            rgba = np.zeros((R.shape[0], R.shape[1], 4), np.uint8)
            rgba[..., :3] = R.astype(np.uint8); rgba[..., 3] = Mr * 255
            big = Image.fromarray(rgba).resize((rw, rh), Image.LANCZOS)
            W, H = w * SS, h * SS
            for lean in (0, 10, 20, 30, 40):
                canvas = Image.new('RGBA', (W, H), (0, 0, 0, 0))
                canvas.paste(big, (W // 2 - rw // 2, (h - 3) * SS - rh), big)
                rot = canvas.rotate(-lean, resample=Image.BICUBIC, center=(W // 2, (h - 3) * SS))
                ra = np.asarray(rot).astype(np.float32)
                frames.append(outline(downsample(ra[..., :3], (ra[..., 3] > 128).astype(np.float32), pal, w, h), pal))
        else:
            for pitch in (0, 7, -7):
                for yaw in (0, 7, 15, 25, 37, 52):
                    im, al = render(P, col, vox, math.radians(yaw), math.radians(pitch), 0.0, w, h)
                    frames.append(outline(downsample(im, al, pal, w, h), pal))
        # crop every frame to the union box; the anchor (ground under the centre, straight behind) is (w/2, h-3)
        u = np.zeros((h, w), bool)
        for fr in frames: u |= fr > 0
        ys_, xs_ = np.where(u)
        x0 = min(xs_.min(), w - 1 - xs_.max()); x1 = w - x0          # keep it symmetric about the centre
        y0, y1 = ys_.min(), ys_.max() + 1
        frames = [fr[y0:y1, x0:x1] for fr in frames]
        ax_, ay_ = w // 2 - x0, (h - 3) - y0
        w, h = x1 - x0, y1 - y0
        anchors.append((ax_, ay_))
        frames_c.append((name, w, h, frames))
        pal_c.append(pal)
        print(name, 'voxels', len(P), 'frames', len(frames), file=sys.stderr)
    # preview sheet (game pixels x3)
    cols = 6
    tiles = []
    for (name, w, h, frames), pal in zip(frames_c, pal_c):
        P3 = np.vstack([[200, 200, 210], pal]).astype(np.uint8)
        row = []
        for fr in frames[:18]:
            t = P3[fr]
            row.append(np.kron(t, np.ones((3, 3, 1), np.uint8)))
        tiles.append((w, h, row))
    Wp = max(len(r) for _, _, r in tiles)
    imgs = []
    for w, h, row in tiles:
        rows = [np.hstack(row[i:i + 6] + [np.full((h * 3, w * 3, 3), 200, np.uint8)] * (6 - len(row[i:i + 6]))) for i in range(0, len(row), 6)]
        imgs.append(np.vstack(rows))
    Wm = max(i.shape[1] for i in imgs)
    big = np.vstack([np.pad(i, ((0, 6), (0, Wm - i.shape[1]), (0, 0)), constant_values=60) for i in imgs])
    Image.fromarray(big).save(os.path.join(HERE, 'vehicles_preview.png'))
    # C header
    with open(OUT, 'w') as f:
        f.write('// Generated by tools/sprites/make_vehicles.py from tools/sprites/vehicles_sheet.jpg. Do not edit.\n')
        f.write('#pragma once\n#include <stdint.h>\nnamespace mantis {\nnamespace art {\n')
        for vi, ((name, w, h, frames), pal) in enumerate(zip(frames_c, pal_c)):
            f.write('// %s: %d frames %dx%d, 4 bits per pixel (0 = clear, 1..15 = palette)\n' % (name, len(frames), w, h))
            f.write('static const uint8_t %s_PAL[15 * 3] = {%s};\n' % (name, ','.join(str(int(v)) for v in pal.ravel())))
            data = []
            for fr in frames:
                flat = fr.ravel()
                if len(flat) & 1: flat = np.append(flat, 0)
                data.extend(((flat[0::2] << 4) | flat[1::2]).tolist())
            f.write('static const uint8_t %s_PX[%d] = {\n' % (name, len(data)))
            for i in range(0, len(data), 40): f.write('  ' + ','.join(str(v) for v in data[i:i + 40]) + ',\n')
            f.write('};\n')
        f.write('struct VehArt { uint8_t w, h, frames; int8_t ax, ay; const uint8_t* pal; const uint8_t* px; };\n')
        f.write('static const VehArt VEH_ART[4] = {\n')
        for (name, w, h, frames), (ax_, ay_) in zip(frames_c, anchors):
            f.write('  {%d, %d, %d, %d, %d, %s_PAL, %s_PX},\n' % (w, h, len(frames), ax_, ay_, name, name))
        f.write('};\n}\n}\n')

if __name__ == '__main__':
    main()
