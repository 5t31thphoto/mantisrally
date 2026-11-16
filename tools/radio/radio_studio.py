#!/usr/bin/env python3
"""Mantis Rally - Radio Studio

Makes the car radio's DJ voices and Mantis Motors ads.

  1. Every clip the radio can play has a slot: its station, what kind of line it is, and a script to read.
  2. Record each script with whatever you like (phone, laptop, DAW), then load the file into its slot.
  3. "Build SD card" processes every clip (trim, radio EQ, compression, level), squeezes it down to 4-bit ADPCM at
     6 kHz (or 8 kHz), and writes /radio/radio.bin (plus a readable manifest) onto the card. The game reads it at
     boot; with no file each station plays a short musical ident between songs instead.

Run:      python3 radio_studio.py                      (the window; needs tkinter, which comes with Python)
Or:       python3 radio_studio.py --build project.json /path/to/sdcard     (no window: builds the card from a project)
          python3 radio_studio.py --new project.json                       (writes a project with every slot's script)

Input: WAV files (8/16/24/32-bit PCM, any rate, mono or stereo). Other formats (mp3, m4a, ogg, flac, float WAV)
work when ffmpeg is installed and on the PATH. Only the Python standard library is needed; numpy makes it faster.

The file format (shared with src/radio.cpp):
  "MRAD", u16 version = 2, u16 count, then count x 20-byte entries
      {u8 station (0 = any, 1 lo-fi, 2 math rock, 3 country), u8 kind, u16 sample rate, u32 samples,
       u32 offset from the end of the table, u32 bytes, u16 biomes (bit b: plays in biome b; 0 = every biome),
       u16 reserved}
  then the clips: IMA ADPCM, 4 bits a sample, low nibble first, the decoder starting at predictor 0, index 0.
  (The game still reads version 1 banks: 16-byte entries, every line in every biome.)
"""
import json, math, os, struct, subprocess, sys, tempfile, wave, shutil, array

try:
    import numpy as np
except ImportError:
    np = None

VERSION = 2
STATIONS = ["Any station", "88.7 LO-FI MANTIS", "96.1 THE ODD METER", "101.5 BACKROADS"]
KINDS = ["Station ID", "Talk", "Morning", "Daytime", "Evening", "Night", "Rain", "Snow", "Fog", "Clear skies", "Ad"]
KIND_HELP = {
    0: "Says the station's name. Plays at every fourth break.",
    1: "Anything: back-announcing, chatter. The most common break.",
    2: "Played when it's morning in the game.", 3: "Played in the daytime.", 4: "Played at dusk.", 5: "Played at night.",
    6: "Played when it's raining.", 7: "Played when it's snowing.", 8: "Played in fog.", 9: "Played in clear weather.",
    10: "A Mantis Motors ad. Plays every third break, on any station.",
}
BUDGET_BYTES = 2560 * 1024           # what the game keeps in PSRAM (it reads at most 2.5 MB of the file)
RATES = [6000, 8000]
# the game's biomes, in its order (B_* in src/journey.h); a line with none ticked plays in every biome
BIOMES = ["Pine Forest", "Redwood Cathedral", "Lake Country", "Rain Moor", "Devil's Pass", "Alpine Summit",
          "Red Canyon", "Coast Highway", "Wildflower Meadow", "Autumn Ridge", "Bayou Mud", "Frost Tundra"]
BIOME_SHORT = ["Pines", "Redwood", "Lakes", "Moor", "Devils", "Summit", "Canyon", "Coast", "Meadow", "Autumn", "Bayou", "Tundra"]
# where the game's own features turn up (from its land numbers), for lines about them
BIOME_PRESETS = [
    ("Every biome", []),
    ("Farm country", [3, 8, 10]),          # crops, pastures and cattle: Rain Moor, Wildflower Meadow, Bayou Mud
    ("Geyser country", [3, 4, 11]),        # Rain Moor, Devil's Pass, Frost Tundra
    ("Red rock arches", [6]),              # Red Canyon
    ("Forest", [0, 1, 9]),                 # Pine Forest, Redwood Cathedral, Autumn Ridge
    ("Mountains", [4, 5]),                 # Devil's Pass, Alpine Summit (tunnels, switchbacks)
]
PAUSE_MAX = 0.25                     # a pause inside a line longer than this is tightened to it (seconds)

def biome_mask(slot):
    m = 0
    for b in slot.get("biomes", []) or []:
        if 0 <= int(b) < len(BIOMES): m |= 1 << int(b)
    return m

def biome_label(slot):
    bs = sorted(set(int(b) for b in (slot.get("biomes") or []) if 0 <= int(b) < len(BIOMES)))
    if not bs: return "All"
    for name, pb in BIOME_PRESETS[1:]:
        if bs == sorted(pb): return name
    return ", ".join(BIOME_SHORT[b] for b in bs)

def slot_rate(slot, project_rate):
    r = int(slot.get("rate", 0) or 0)
    return r if r in RATES else int(project_rate)

# ------------------------------------------------------------------------------------------------ the scripts
# Short on purpose: a radio break is a breath between songs. Read them in character; ad-libbing is encouraged.
DEFAULT_SCRIPTS = [
    # ---- 88.7 LO-FI MANTIS: Moth, late-night, unhurried
    (1, 0, "Eighty-eight point seven. Lo-fi Mantis. Windows down, nowhere to be."),
    (1, 0, "You're on Lo-fi Mantis. Beats to drive slow to."),
    (1, 0, "Lo-fi Mantis, eighty-eight seven. I'm Moth. Stay a while."),
    (1, 1, "That one had some dust on it. The good kind. Keep rolling."),
    (1, 1, "If you're out there on the long road tonight... this is for you."),
    (1, 1, "Moth here. No requests, no rush. Just the next record."),
    (1, 1, "Somebody left a mixtape in the glovebox. Let's see what's on side B."),
    (1, 2, "Morning, early birds. Coffee in the cupholder? Good. Let's ease in."),
    (1, 3, "Middle of the day, middle of nowhere. Perfect."),
    (1, 4, "Sun's getting low. Headlights on soon. Here's something warm."),
    (1, 5, "Late now. Just you, me, and the road. Turn it down a notch."),
    (1, 6, "Rain on the roof. Wipers keeping time. Couldn't ask for a better drummer."),
    (1, 7, "Snow's falling. Take it slow. The music will wait."),
    (1, 8, "Can't see much in this fog. That's alright. Listen instead."),
    (1, 9, "Clear skies tonight. Go find a pull-off and look up."),
    # ---- 96.1 THE ODD METER: Tess, dry, counts in sevens
    (2, 0, "Ninety-six point one. The Odd Meter. Count along if you can."),
    (2, 0, "This is The Odd Meter. Nothing here is in four."),
    (2, 0, "Odd Meter, ninety-six one. I'm Tess. One, two, three, four, five, six, seven."),
    (2, 1, "That last one was in seven-eight. Your foot was wrong. It's fine. Everybody's foot is wrong."),
    (2, 1, "Tapping, twinkly guitars, a drummer showing off. You know where you are."),
    (2, 1, "Somebody asked for a song in four-four. We don't have any."),
    (2, 1, "Tess here. Hang on, the next one changes time signature twice."),
    (2, 2, "Good morning. It's too early for nine-eight. Here's nine-eight."),
    (2, 3, "Afternoon. Eyes on the road, fingers on the wheel. Not tapping. Okay, a little tapping."),
    (2, 4, "Evening. The light's doing that thing. So is the guitar."),
    (2, 5, "It's late. The math still adds up."),
    (2, 6, "Rain. Great for traction, terrible for counting."),
    (2, 7, "Snow. Slow down. Fifty-seven beats a minute, if you need a number."),
    (2, 8, "Fog's thick. Can't see the road, can't see the downbeat."),
    (2, 9, "Clear skies. Nothing in the way. Like a good riff."),
    # ---- 101.5 BACKROADS: Dusty, warm and easy
    (3, 0, "You're listening to Backroads, one-oh-one five. Dirt road music for dirt road people."),
    (3, 0, "Backroads, one-oh-one point five. Pull up a tailgate."),
    (3, 0, "This is Dusty on Backroads. Glad you found us out here."),
    (3, 1, "That was a good one. Somebody's grandma's favorite, I'd bet."),
    (3, 1, "If you see a deer, wave. If you see a moose, wave from farther away."),
    (3, 1, "Gas is cheap if you don't use any. Coast the hills, friends."),
    (3, 1, "Dusty here. Next one's got a fiddle in it. Most of 'em do."),
    (3, 2, "Mornin'. Sun's up, road's clear, and the coffee's strong."),
    (3, 3, "High noon on the backroads. Windows down, elbow out."),
    (3, 4, "Evenin'. That's a sunset worth pullin' over for."),
    (3, 5, "Night drive. Keep your brights handy and your eyes peeled."),
    (3, 6, "Rain's comin' down. Good for the crops. Bad for your windshield."),
    (3, 7, "Snow on the pines. Pretty as a postcard. Drive like your mama's watching."),
    (3, 8, "Fog's rolled in off the hollow. Low beams, slow wheels."),
    (3, 9, "Not a cloud up there. Big sky country, wherever you are."),
    # ---- Mantis Motors (any station)
    (0, 10, "Mantis Motors! Where your miles are money. Drive it, earn it, trade up. Follow the green arrows. Mantis Motors."),
    (0, 10, "Tired of pavement? The Mantis Motors Jeep. Doors off, top down, roads optional. Paid for with the miles you already drove."),
    (0, 10, "The lifted Stingray. It's a Corvette. On stilts. Don't ask why. Ask how many miles. Mantis Motors."),
    (0, 10, "Two wheels, no roof, all mud. The Mantis Motors dirt bike. You've earned it, you just don't know it yet."),
]

def default_project():
    return {"version": 2, "rate": 6000, "slots": [
        {"station": st, "kind": k, "script": txt, "file": "", "biomes": [], "rate": 0} for st, k, txt in DEFAULT_SCRIPTS]}

# ------------------------------------------------------------------------------------------------ reading audio
def _ffmpeg_to_wav(path):
    ff = shutil.which("ffmpeg")
    if not ff:
        raise RuntimeError("Can't read %s: it isn't a PCM WAV, and ffmpeg isn't installed to convert it." % os.path.basename(path))
    fd, tmp = tempfile.mkstemp(suffix=".wav"); os.close(fd)
    r = subprocess.run([ff, "-y", "-loglevel", "error", "-i", path, "-ac", "1", "-acodec", "pcm_s16le", tmp],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError("ffmpeg couldn't read %s: %s" % (os.path.basename(path), r.stderr.decode(errors="replace").strip()))
    return tmp

def read_audio(path):
    """-> (mono samples as floats -1..1, sample rate)"""
    tmp = None
    try:
        try:
            w = wave.open(path, "rb")
        except (wave.Error, EOFError):
            tmp = _ffmpeg_to_wav(path); w = wave.open(tmp, "rb")
        with w:
            ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
            raw = w.readframes(n)
        if width == 1:
            vals = [(b - 128) / 128.0 for b in raw]
        elif width == 2:
            a = array.array("h"); a.frombytes(raw[:len(raw) // 2 * 2])
            if sys.byteorder == "big": a.byteswap()
            vals = [v / 32768.0 for v in a]
        elif width == 3:
            vals = [int.from_bytes(raw[i:i + 3], "little", signed=True) / 8388608.0 for i in range(0, len(raw) - 2, 3)]
        elif width == 4:
            a = array.array("i"); a.frombytes(raw[:len(raw) // 4 * 4])
            if sys.byteorder == "big": a.byteswap()
            vals = [v / 2147483648.0 for v in a]
        else:
            raise RuntimeError("Unsupported sample width in %s" % path)
        if ch > 1:
            vals = [sum(vals[i:i + ch]) / ch for i in range(0, len(vals) - ch + 1, ch)]
        return vals, rate
    finally:
        if tmp and os.path.exists(tmp): os.remove(tmp)

def write_wav(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        a = array.array("h", [max(-32767, min(32767, int(round(s * 32767)))) for s in samples])
        if sys.byteorder == "big": a.byteswap()
        w.writeframes(a.tobytes())

# ------------------------------------------------------------------------------------------------ processing
def _lowpass_taps(cutoff, rate, n=63):
    """windowed-sinc low-pass, cutoff in Hz"""
    fc = cutoff / rate; m = (n - 1) / 2.0; taps = []
    for i in range(n):
        x = i - m
        h = 2 * fc if x == 0 else math.sin(2 * math.pi * fc * x) / (math.pi * x)
        h *= 0.42 - 0.5 * math.cos(2 * math.pi * i / (n - 1)) + 0.08 * math.cos(4 * math.pi * i / (n - 1))   # Blackman
        taps.append(h)
    s = sum(taps); return [t / s for t in taps]

def resample(x, src, dst):
    """band-limited resampling (windowed sinc), any ratio"""
    if src == dst: return list(x)
    cutoff = 0.45 * min(src, dst)
    if np is not None:
        xa = np.asarray(x, dtype=np.float64)
        n_out = int(len(xa) * dst / src)
        t = np.arange(n_out) * (src / dst)
        half = 16
        fc = cutoff / src
        out = np.zeros(n_out)
        base = np.floor(t).astype(np.int64)
        for k in range(-half + 1, half + 1):
            idx = base + k
            d = t - idx
            w = 0.5 + 0.5 * np.cos(np.pi * d / half)
            h = 2 * fc * np.sinc(2 * fc * d) * w
            valid = (idx >= 0) & (idx < len(xa))
            out[valid] += xa[idx[valid]] * h[valid]
        return out.tolist()
    # plain Python: low-pass at the source rate, then linear interpolation (fine for speech)
    taps = _lowpass_taps(cutoff, src, 47)
    m = len(taps) // 2
    n = len(x)
    y = [0.0] * n
    for i in range(n):
        acc = 0.0
        lo = max(0, i - m); hi = min(n, i + m + 1)
        for j in range(lo, hi):
            acc += x[j] * taps[j - i + m]
        y[i] = acc
    n_out = int(n * dst / src); out = []
    for i in range(n_out):
        p = i * src / dst; k = int(p); f = p - k
        a = y[k] if k < n else 0.0; b = y[k + 1] if k + 1 < n else a
        out.append(a + (b - a) * f)
    return out

class Biquad:
    def __init__(self, kind, f0, rate, q=0.707, gain_db=0.0):
        A = 10 ** (gain_db / 40.0); w = 2 * math.pi * f0 / rate; c = math.cos(w); s = math.sin(w); al = s / (2 * q)
        if kind == "hp":   b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]; a = [1 + al, -2 * c, 1 - al]
        elif kind == "lp": b = [(1 - c) / 2, 1 - c, (1 - c) / 2]; a = [1 + al, -2 * c, 1 - al]
        elif kind == "peak": b = [1 + al * A, -2 * c, 1 - al * A]; a = [1 + al / A, -2 * c, 1 - al / A]
        else: raise ValueError(kind)
        self.b = [v / a[0] for v in b]; self.a = [v / a[0] for v in a]
    def run(self, x):
        b0, b1, b2 = self.b; _, a1, a2 = self.a
        x1 = x2 = y1 = y2 = 0.0; out = []
        for v in x:
            y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, v, y1, y
            out.append(y)
        return out

def process_voice(x, rate, out_rate, grit=0.15):
    """trim, radio EQ, compression and level: everything a voice goes through before it's squeezed"""
    # 1) to an intermediate rate (light work), then remove rumble
    mid = 16000
    x = resample(x, rate, mid)
    x = Biquad("hp", 120, mid, 0.7).run(x)
    # 2) trim the silence at either end (keep 80 ms of air)
    env = 0.0; lev = []
    for v in x:
        env = max(abs(v), env * 0.999); lev.append(env)
    peak = max(lev) if lev else 0.0
    if peak < 1e-4: return []
    th = peak * 0.03
    first = next((i for i, l in enumerate(lev) if l > th), 0)
    last = len(lev) - next((i for i, l in enumerate(reversed(lev)) if l > th), 0)
    pad = int(0.08 * mid)
    lo, hi = max(0, first - pad), min(len(x), last + pad)
    x, lev = x[lo:hi], lev[lo:hi]
    # 2b) tighten the pauses inside it: a gap longer than PAUSE_MAX keeps PAUSE_MAX of air (half each side)
    keep = int(PAUSE_MAX * mid); out_x = []; i = 0; n = len(x)
    while i < n:
        if lev[i] > th: out_x.append(x[i]); i += 1; continue
        j = i
        while j < n and lev[j] <= th: j += 1
        if j - i > keep:
            out_x.extend(x[i:i + keep // 2]); out_x.extend(x[j - keep // 2:j])
        else:
            out_x.extend(x[i:j])
        i = j
    x = out_x
    # 3) the radio voice: presence, a little body, the top rolled off
    x = Biquad("peak", 2200, mid, 0.9, 5.0).run(x)
    x = Biquad("peak", 220, mid, 0.8, 2.0).run(x)
    x = Biquad("lp", out_rate * 0.44, mid, 0.7).run(x)
    # 4) compression: a fast RMS compressor (4:1 above -24 dB), then a gentle saturation for grit
    out = []; e = 0.0; g = 1.0
    att = math.exp(-1.0 / (0.004 * mid)); rel = math.exp(-1.0 / (0.12 * mid))
    th_lin = 10 ** (-24 / 20.0); ratio = 4.0
    peak = max(abs(v) for v in x) or 1.0
    for v in x:
        v /= peak
        p = v * v
        e = p + (e - p) * (att if p > e else rel)
        r = math.sqrt(e)
        tg = 1.0 if r <= th_lin else (th_lin * (r / th_lin) ** (1.0 / ratio)) / r
        g += (tg - g) * (0.3 if tg < g else 0.002)
        y = v * g
        if grit > 0: y = (1 - grit) * y + grit * math.tanh(y * 2.5) / 2.5 * 2.0
        out.append(y)
    # 5) to the radio's rate, level to -1 dBFS, fade the ends
    out = resample(out, mid, out_rate)
    peak = max(abs(v) for v in out) or 1.0
    k = 10 ** (-1 / 20.0) / peak
    out = [v * k for v in out]
    f = int(0.012 * out_rate)
    for i in range(min(f, len(out))):
        out[i] *= i / f; out[-1 - i] *= i / f
    return out

# ------------------------------------------------------------------------------------------------ IMA ADPCM
ADPCM_IDX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
ADPCM_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
              107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
              876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
              5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
              27086, 29794, 32767]

def adpcm_encode(samples):
    pred, idx = 0, 0; nibs = []
    for s in samples:
        target = max(-32768, min(32767, int(round(s * 32767))))
        step = ADPCM_STEP[idx]; diff = target - pred; nib = 0
        if diff < 0: nib = 8; diff = -diff
        delta = step >> 3
        if diff >= step: nib |= 4; diff -= step; delta += step
        if diff >= step >> 1: nib |= 2; diff -= step >> 1; delta += step >> 1
        if diff >= step >> 2: nib |= 1; delta += step >> 2
        pred = pred - delta if nib & 8 else pred + delta
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + ADPCM_IDX[nib]))
        nibs.append(nib)
    if len(nibs) & 1: nibs.append(0)
    return bytes(nibs[i] | (nibs[i + 1] << 4) for i in range(0, len(nibs), 2))

def adpcm_decode(data, count):
    """exactly what the game does (src/radio.cpp)"""
    pred, idx = 0, 0; out = []
    for i in range(count):
        b = data[i >> 1]; nib = (b >> 4) if i & 1 else (b & 15)
        step = ADPCM_STEP[idx]; diff = step >> 3
        if nib & 4: diff += step
        if nib & 2: diff += step >> 1
        if nib & 1: diff += step >> 2
        pred = pred - diff if nib & 8 else pred + diff
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + ADPCM_IDX[nib]))
        out.append(pred / 32768.0)
    return out

# ------------------------------------------------------------------------------------------------ the bank
def build_clip(slot, rate, base_dir=""):
    path = slot["file"]
    if not os.path.isabs(path) and base_dir: path = os.path.join(base_dir, path)
    x, src = read_audio(path)
    y = process_voice(x, src, rate)
    if not y: raise RuntimeError("%s is silent" % os.path.basename(path))
    return y, adpcm_encode(y)

def build_bank(project, base_dir="", progress=None):
    """-> (bytes of radio.bin, manifest text, warnings)"""
    prate = int(project.get("rate", 6000))
    entries, blobs, lines, warnings = [], [], [], []
    slots = [s for s in project["slots"] if s.get("file")]
    for i, s in enumerate(slots):
        if progress: progress(i, len(slots), s)
        rate = slot_rate(s, prate)
        try:
            y, data = build_clip(s, rate, base_dir)
        except Exception as e:
            warnings.append("%s / %s: %s" % (STATIONS[s["station"]], KINDS[s["kind"]], e)); continue
        entries.append((s["station"], s["kind"], rate, len(y), len(data), biome_mask(s)))
        blobs.append(data)
        lines.append("%-20s %-12s %-18s %5d Hz %5.1f s %7d bytes  %s" % (STATIONS[s["station"]], KINDS[s["kind"]], biome_label(s)[:18], rate, len(y) / rate, len(data), s["script"][:60]))
    head = b"MRAD" + struct.pack("<HH", VERSION, len(entries))
    table = b""; off = 0
    for (st, k, r, n, b, bm) in entries:
        table += struct.pack("<BBHIIIHH", st, k, r, n, off, b, bm, 0); off += b
    out = head + table + b"".join(blobs)
    total = len(out)
    manifest = ["Mantis Rally radio bank: %d clips, %.1f KB (%.0f%% of the %d KB budget), 4-bit ADPCM, %d Hz unless a line says"
                % (len(entries), total / 1024, 100.0 * total / BUDGET_BYTES, BUDGET_BYTES // 1024, prate), ""] + lines
    if total > BUDGET_BYTES:
        warnings.append("The bank is %.0f KB, over the %d KB the game reads: lines past that are cut off. Drop some, or use 6000 Hz." % (total / 1024, BUDGET_BYTES // 1024))
    return out, "\n".join(manifest) + "\n", warnings

def write_card(project, card_root, base_dir="", progress=None):
    data, manifest, warnings = build_bank(project, base_dir, progress)
    d = os.path.join(card_root, "radio"); os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "radio.bin"), "wb") as f: f.write(data)
    with open(os.path.join(d, "manifest.txt"), "w") as f: f.write(manifest)
    return len(data), manifest, warnings

# ------------------------------------------------------------------------------------------------ playing a preview
def play_wav(path):
    try:
        if sys.platform.startswith("win"):
            import winsound; winsound.PlaySound(path, winsound.SND_FILENAME | winsound.SND_ASYNC); return True
        if sys.platform == "darwin":
            subprocess.Popen(["afplay", path]); return True
        for p in (["paplay", path], ["aplay", "-q", path], ["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", path]):
            if shutil.which(p[0]): subprocess.Popen(p); return True
    except Exception:
        pass
    return False

# ------------------------------------------------------------------------------------------------ the window
def run_gui():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox

    root = tk.Tk(); root.title("Mantis Rally - Radio Studio"); root.geometry("1120x780")
    state = {"project": default_project(), "path": None}

    top = ttk.Frame(root, padding=6); top.pack(fill="x")
    ttk.Button(top, text="New", command=lambda: new_project()).pack(side="left")
    ttk.Button(top, text="Open...", command=lambda: open_project()).pack(side="left", padx=4)
    ttk.Button(top, text="Save", command=lambda: save_project()).pack(side="left")
    ttk.Label(top, text="   Rate:").pack(side="left")
    rate_var = tk.StringVar(value=str(state["project"]["rate"]))
    rate_box = ttk.Combobox(top, textvariable=rate_var, values=[str(r) for r in RATES], width=6, state="readonly"); rate_box.pack(side="left")
    ttk.Label(top, text="Hz (6000: smallest, 8000: clearer)").pack(side="left", padx=4)
    ttk.Button(top, text="Build SD card...", command=lambda: build()).pack(side="right")
    budget = ttk.Label(top, text=""); budget.pack(side="right", padx=10)

    mid = ttk.PanedWindow(root, orient="horizontal"); mid.pack(fill="both", expand=True, padx=6)
    left = ttk.Frame(mid); mid.add(left, weight=3)
    cols = ("station", "kind", "biomes", "file", "script")
    tree = ttk.Treeview(left, columns=cols, show="headings", selectmode="browse")
    for c, w in zip(cols, (140, 80, 120, 130, 380)): tree.heading(c, text=c.title()); tree.column(c, width=w, anchor="w")
    sb = ttk.Scrollbar(left, orient="vertical", command=tree.yview); tree.configure(yscrollcommand=sb.set)
    tree.pack(side="left", fill="both", expand=True); sb.pack(side="right", fill="y")

    right = ttk.Frame(mid, padding=8); mid.add(right, weight=2)
    st_var, kd_var = tk.StringVar(), tk.StringVar()
    ttk.Label(right, text="Station").pack(anchor="w")
    st_box = ttk.Combobox(right, textvariable=st_var, values=STATIONS, state="readonly"); st_box.pack(fill="x")
    ttk.Label(right, text="Kind").pack(anchor="w", pady=(6, 0))
    kd_box = ttk.Combobox(right, textvariable=kd_var, values=KINDS, state="readonly"); kd_box.pack(fill="x")
    kind_help = ttk.Label(right, text="", foreground="#555", wraplength=360); kind_help.pack(anchor="w", pady=2)
    # where it plays: none ticked = every biome
    bfr = ttk.LabelFrame(right, text="Plays in (none ticked: every biome)", padding=4); bfr.pack(fill="x", pady=(6, 0))
    bvars = [tk.BooleanVar(value=False) for _ in BIOMES]
    for b, name in enumerate(BIOMES):
        ttk.Checkbutton(bfr, text=name, variable=bvars[b], command=lambda: commit()).grid(row=b // 3, column=b % 3, sticky="w", padx=2)
    pfr = ttk.Frame(right); pfr.pack(fill="x", pady=(2, 0))
    ttk.Label(pfr, text="Presets:").pack(side="left")
    pre_var = tk.StringVar()
    pre_box = ttk.Combobox(pfr, textvariable=pre_var, values=[n for n, _ in BIOME_PRESETS], state="readonly", width=18); pre_box.pack(side="left", padx=4)
    rfr = ttk.Frame(right); rfr.pack(fill="x", pady=(6, 0))
    ttk.Label(rfr, text="Rate for this line:").pack(side="left")
    srate_var = tk.StringVar(value="Project")
    srate_box = ttk.Combobox(rfr, textvariable=srate_var, values=["Project"] + [str(r) for r in RATES], state="readonly", width=8); srate_box.pack(side="left", padx=4)
    ttk.Label(right, text="Script (what to say)").pack(anchor="w", pady=(6, 0))
    script = tk.Text(right, height=6, wrap="word"); script.pack(fill="x")
    file_lbl = ttk.Label(right, text="No recording", wraplength=360); file_lbl.pack(anchor="w", pady=6)
    row = ttk.Frame(right); row.pack(fill="x")
    ttk.Button(row, text="Load recording...", command=lambda: load_file()).pack(side="left")
    ttk.Button(row, text="Clear", command=lambda: clear_file()).pack(side="left", padx=4)
    row2 = ttk.Frame(right); row2.pack(fill="x", pady=4)
    ttk.Button(row2, text="Play original", command=lambda: play(False)).pack(side="left")
    ttk.Button(row2, text="Play as the radio", command=lambda: play(True)).pack(side="left", padx=4)
    row3 = ttk.Frame(right); row3.pack(fill="x", pady=(14, 0))
    ttk.Button(row3, text="Add slot", command=lambda: add_slot()).pack(side="left")
    ttk.Button(row3, text="Remove slot", command=lambda: remove_slot()).pack(side="left", padx=4)
    status = ttk.Label(root, text="", padding=6); status.pack(fill="x")

    def base_dir():
        return os.path.dirname(state["path"]) if state["path"] else ""
    def resolve(f):
        return f if os.path.isabs(f) or not base_dir() else os.path.join(base_dir(), f)
    def refresh():
        tree.delete(*tree.get_children())
        for i, s in enumerate(state["project"]["slots"]):
            tree.insert("", "end", iid=str(i), values=row_values(s))
        n = sum(1 for s in state["project"]["slots"] if s["file"])
        prate = int(rate_var.get())
        kb = 0.0
        for s in state["project"]["slots"]:
            if s["file"]:
                try:
                    with wave.open(resolve(s["file"])) as w:
                        secs = w.getnframes() / w.getframerate()
                except Exception:
                    secs = 6.0
                kb += secs * 0.8 * slot_rate(s, prate) / 2 / 1024        # (about a fifth goes in the trims)
        budget.config(text="%d of %d recorded, about %.0f KB of %d KB" % (n, len(state["project"]["slots"]), kb, BUDGET_BYTES // 1024))
    def row_values(s):
        return (STATIONS[s["station"]], KINDS[s["kind"]], biome_label(s), os.path.basename(s["file"]) if s["file"] else "-", s["script"].replace("\n", " "))
    def current():
        sel = tree.selection(); return int(sel[0]) if sel else None
    def on_select(_=None):
        i = current()
        if i is None: return
        s = state["project"]["slots"][i]
        st_var.set(STATIONS[s["station"]]); kd_var.set(KINDS[s["kind"]])
        kind_help.config(text=KIND_HELP[s["kind"]])
        bs = set(int(b) for b in (s.get("biomes") or []))
        for b, v in enumerate(bvars): v.set(b in bs)
        pre_var.set("")
        srate_var.set(str(s.get("rate")) if s.get("rate") in RATES else "Project")
        script.delete("1.0", "end"); script.insert("1.0", s["script"])
        file_lbl.config(text=s["file"] or "No recording")
    def commit(_=None):
        i = current()
        if i is None: return
        s = state["project"]["slots"][i]
        s["station"] = STATIONS.index(st_var.get()) if st_var.get() in STATIONS else s["station"]
        s["kind"] = KINDS.index(kd_var.get()) if kd_var.get() in KINDS else s["kind"]
        s["script"] = script.get("1.0", "end").strip()
        s["biomes"] = [b for b, v in enumerate(bvars) if v.get()]
        s["rate"] = int(srate_var.get()) if srate_var.get().isdigit() else 0
        kind_help.config(text=KIND_HELP[s["kind"]])
        tree.item(str(i), values=row_values(s))
    tree.bind("<<TreeviewSelect>>", on_select)
    st_box.bind("<<ComboboxSelected>>", commit); kd_box.bind("<<ComboboxSelected>>", commit)
    script.bind("<KeyRelease>", commit)
    def apply_preset(_=None):
        for name, bs in BIOME_PRESETS:
            if name == pre_var.get():
                for b, v in enumerate(bvars): v.set(b in bs)
        commit(); refresh_budget_only()
    def refresh_budget_only():
        i = current(); refresh()
        if i is not None: tree.selection_set(str(i))
    pre_box.bind("<<ComboboxSelected>>", apply_preset)
    srate_box.bind("<<ComboboxSelected>>", lambda e: (commit(), refresh_budget_only()))
    rate_box.bind("<<ComboboxSelected>>", lambda e: (state["project"].__setitem__("rate", int(rate_var.get())), refresh()))

    def load_file():
        i = current()
        if i is None: messagebox.showinfo("Radio Studio", "Pick a slot first."); return
        p = filedialog.askopenfilename(title="Recording for this slot",
                                       filetypes=[("Audio", "*.wav *.mp3 *.m4a *.ogg *.flac *.aiff *.aif"), ("All files", "*.*")])
        if not p: return
        try:
            x, r = read_audio(p)
            status.config(text="Loaded %s: %.1f s at %d Hz" % (os.path.basename(p), len(x) / r, r))
        except Exception as e:
            messagebox.showerror("Radio Studio", str(e)); return
        state["project"]["slots"][i]["file"] = p
        refresh(); tree.selection_set(str(i)); on_select()
    def clear_file():
        i = current()
        if i is None: return
        state["project"]["slots"][i]["file"] = ""; refresh(); tree.selection_set(str(i)); on_select()
    def play(processed):
        i = current()
        if i is None: return
        s = state["project"]["slots"][i]
        if not s["file"]: return
        try:
            if processed:
                rate = slot_rate(s, int(rate_var.get()))
                y, data = build_clip(s, rate, base_dir())
                y = adpcm_decode(data, len(y))
                fd, tmp = tempfile.mkstemp(suffix=".wav"); os.close(fd); write_wav(tmp, y, rate)
                status.config(text="As the radio: %.1f s, %d bytes" % (len(y) / rate, len(data)))
            else:
                x, r = read_audio(resolve(s["file"])); fd, tmp = tempfile.mkstemp(suffix=".wav"); os.close(fd); write_wav(tmp, x, r)
            if not play_wav(tmp): messagebox.showinfo("Radio Studio", "No audio player found. The file is at\n" + tmp)
        except Exception as e:
            messagebox.showerror("Radio Studio", str(e))
    def add_slot():
        state["project"]["slots"].append({"station": 1, "kind": 1, "script": "New line.", "file": "", "biomes": [], "rate": 0})
        refresh(); i = len(state["project"]["slots"]) - 1; tree.selection_set(str(i)); tree.see(str(i)); on_select()
    def remove_slot():
        i = current()
        if i is None: return
        del state["project"]["slots"][i]; refresh()
    def new_project():
        if messagebox.askyesno("Radio Studio", "Start over with the default scripts?"):
            state["project"] = default_project(); state["path"] = None; rate_var.set(str(state["project"]["rate"])); refresh()
    def open_project():
        p = filedialog.askopenfilename(filetypes=[("Radio project", "*.json")])
        if not p: return
        with open(p) as f: state["project"] = json.load(f)
        state["path"] = p; rate_var.set(str(state["project"].get("rate", 6000))); refresh()
    def save_project():
        p = state["path"] or filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("Radio project", "*.json")])
        if not p: return
        state["project"]["rate"] = int(rate_var.get())
        with open(p, "w") as f: json.dump(state["project"], f, indent=1)
        state["path"] = p; status.config(text="Saved " + p)
    def build():
        d = filedialog.askdirectory(title="The SD card (or a folder to copy onto it)")
        if not d: return
        state["project"]["rate"] = int(rate_var.get())
        def prog(i, n, s):
            status.config(text="Processing %d of %d: %s / %s" % (i + 1, n, STATIONS[s["station"]], KINDS[s["kind"]])); root.update()
        try:
            size, manifest, warnings = write_card(state["project"], d, base_dir(), prog)
        except Exception as e:
            messagebox.showerror("Radio Studio", str(e)); return
        status.config(text="Wrote %s (%.0f KB)" % (os.path.join(d, "radio", "radio.bin"), size / 1024))
        messagebox.showinfo("Radio Studio", manifest.splitlines()[0] + ("\n\n" + "\n".join(warnings) if warnings else ""))

    refresh()
    root.mainloop()

def main(argv):
    if len(argv) >= 2 and argv[1] == "--new":
        p = argv[2] if len(argv) > 2 else "radio_project.json"
        with open(p, "w") as f: json.dump(default_project(), f, indent=1)
        print("Wrote", p); return 0
    if len(argv) >= 2 and argv[1] == "--build":
        if len(argv) < 4: print("usage: radio_studio.py --build project.json /path/to/sdcard"); return 2
        with open(argv[2]) as f: project = json.load(f)
        def prog(i, n, s): print("  %d/%d %s / %s" % (i + 1, n, STATIONS[s["station"]], KINDS[s["kind"]]))
        size, manifest, warnings = write_card(project, argv[3], os.path.dirname(os.path.abspath(argv[2])), prog)
        print(manifest)
        for w in warnings: print("WARNING:", w)
        return 0
    run_gui(); return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
