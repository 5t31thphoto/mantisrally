#pragma GCC optimize("Os")
// Rally renderer — lighting/weather from MantisPlanet:
//   palette rebuilt every frame from TOD keyframes; fog = palette twins;
//   sky, clouds, mist, god rays, lightning flash, distance haze.
// Layers: sky -> sun -> clouds -> far ridges -> road -> props/signs -> mist -> rays -> HUD
#include "render.h"
#include "render_int.h"
#include "scene.h"
#include "terrain.h"
#include "field.h"
#include "journey.h"
#include "universe.h"
#include "mem.h"
#include <math.h>
#include <string.h>

namespace mantis {

HudInfo g_hud;

namespace rr {
uint8_t fb[W * H];
uint8_t* zb = nullptr;
float g_cx, g_cy, g_cz, g_cY, g_sY, g_t, g_vis, g_pitch, g_yaw;
int g_hor, g_pan;
float g_alpha = 1.f;
uint8_t g_zc = 255;
}
using namespace rr;
// R_* palette bases and clampf/clampi/put/zcode live in rr::
// Planet-style projection: columns are world angles, ground rows are depth.
static constexpr int   HOR = 58;                 // horizon row
static constexpr float PXR = W / FOV;            // pixels per radian
static constexpr float ZK  = 1.35f * PXR;        // ground: depth = ZK / (row - HOR)
static float colAng[W];
static float g_dist = 0.f;   // distance travelled (panorama morph seed)


// ---- palette ----
static uint16_t s_pal565[256];
static uint8_t  s_palRGB[256 * 3];

struct C3 { float r, g, b; };
static inline C3 mix3(C3 a, C3 b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Planet-style TOD keyframes (zenith, horizon, glow, cloud dark/lit, sun, ambient, key, fog)
struct Key {
    float t;
    C3 zen, hor, glow, cDark, cLit, sun, amb, key, fog;
    float elev, night, mist;
};
#define NK(T, E, M) {T, {6,5,20},{24,18,52},{40,24,70},{18,12,40},{95,55,150},{215,220,255},{70,75,120},{60,70,150},{26,22,55}, E, 1.f, M}
static const Key KEYS[] = {
    NK(0.00f, -0.40f, 0.25f),
    {0.19f, {18,14,50},{75,42,105},{160,75,115},{40,28,75},{190,105,150},{255,200,160},{105,92,140},{190,105,125},{85,58,115}, -0.06f, 0.55f, 0.55f},
    {0.26f, {58,48,128},{238,118,92},{255,188,112},{105,58,118},{255,168,110},{255,240,190},{160,130,140},{255,150,80},{195,118,128}, 0.07f, 0.0f, 0.60f},
    {0.36f, {70,120,210},{188,208,235},{255,240,200},{138,150,190},{255,250,240},{255,255,230},{220,220,230},{255,230,180},{168,192,222}, 0.42f, 0.0f, 0.25f},
    {0.50f, {40,100,215},{168,208,245},{240,248,255},{150,165,200},{255,255,255},{255,255,240},{255,255,255},{255,245,220},{160,195,235}, 0.80f, 0.0f, 0.10f},
    {0.63f, {62,90,190},{218,190,172},{255,220,150},{150,128,170},{255,230,190},{255,245,200},{230,215,205},{255,210,140},{190,170,182}, 0.36f, 0.0f, 0.15f},
    {0.71f, {82,50,142},{255,118,70},{255,200,90},{120,50,110},{255,148,90},{255,240,170},{170,115,128},{255,128,50},{190,90,120}, 0.06f, 0.0f, 0.35f},
    {0.79f, {35,22,75},{178,50,100},{255,110,70},{60,30,80},{220,90,125},{255,180,120},{105,78,115},{200,70,80},{108,45,95}, -0.08f, 0.35f, 0.45f},
    NK(0.89f, -0.35f, 0.30f),
    NK(1.00f, -0.40f, 0.25f),
};
static constexpr int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);

static Key sampleKey(float tod) {
    int k = 0;
    while (k < NKEYS - 2 && tod >= KEYS[k + 1].t) k++;
    const Key &A = KEYS[k], &B = KEYS[k + 1];
    float t = clampf((tod - A.t) / (B.t - A.t), 0.f, 1.f);
    t = t * t * (3.f - 2.f * t);
    Key o;
    o.zen = mix3(A.zen, B.zen, t); o.hor = mix3(A.hor, B.hor, t); o.glow = mix3(A.glow, B.glow, t);
    o.cDark = mix3(A.cDark, B.cDark, t); o.cLit = mix3(A.cLit, B.cLit, t); o.sun = mix3(A.sun, B.sun, t);
    o.amb = mix3(A.amb, B.amb, t); o.key = mix3(A.key, B.key, t); o.fog = mix3(A.fog, B.fog, t);
    o.elev = A.elev + (B.elev - A.elev) * t;
    o.night = A.night + (B.night - A.night) * t;
    o.mist = A.mist + (B.mist - A.mist) * t;
    return o;
}

static Key S;
static float s_sunX = 80.f, s_sunY = 20.f;
static float s_grey = 0.f;   // weather desat


static void ramp(C3* P, int base, C3 dark, C3 lit) {
    for (int i = 0; i < 16; i++) {
        float t = i / 15.f;
        P[base + i] = mix3(dark, lit, t);
    }
}

static void weatherKey(const Scene& sc) {
    s_grey = clampf(sc.cloud * 0.55f - 0.1f + sc.rain * 0.4f + sc.storm * 0.3f, 0.f, 0.85f);
}

static void buildPalette() {
    const Scene& sc = g_scene;
    C3 P[256];
    memset(P, 0, sizeof(P));

    // Sky: zenith -> horizon, with glow band near sun height
    for (int i = 0; i < 16; i++) {
        float t = i / 15.f;
        C3 c = mix3(S.zen, S.hor, t);
        if (t > 0.55f && t < 0.95f) {
            float g = 1.f - fabsf(t - 0.75f) * 5.f;
            if (g > 0.f) c = mix3(c, S.glow, g * (1.f - S.night) * 0.55f);
        }
        // weather grey
        C3 grey = {c.r * 0.3f + c.g * 0.5f + c.b * 0.2f, c.r * 0.3f + c.g * 0.5f + c.b * 0.2f, c.r * 0.3f + c.g * 0.5f + c.b * 0.2f};
        grey.b += 8.f; grey.r -= 4.f;
        c = mix3(c, grey, s_grey);
        P[R_SKY + i] = c;
    }

    ramp(P, R_CLOUD, S.cDark, S.cLit);
    for (int i = 0; i < 16; i++) {
        C3& c = P[R_CLOUD + i];
        C3 g = {(c.r+c.g+c.b)/3.f, (c.r+c.g+c.b)/3.f, (c.r+c.g+c.b)/3.f};
        c = mix3(c, g, s_grey * 0.7f);
    }
    ramp(P, R_SUN, mix3(S.sun, C3{255,180,80}, 0.3f), S.sun);

    // Far ridges — ambient rock tinted by key light
    C3 ridgeD = mix3(S.amb, C3{40, 36, 42}, 0.5f);
    C3 ridgeL = mix3(S.key, C3{120, 110, 100}, 0.4f);
    ramp(P, R_RIDGE, ridgeD, ridgeL);

    // Road: tarmac / gravel / dirt / snow / mud from surface, lit by amb/key
    uint8_t surf = 1;
    {
        // approximate from journey
        const BiomeInfo& bi = journeyBiome(sc.stage);
        surf = bi.surface;
    }
    C3 roadD, roadL;
    if (surf == 0) { roadD = {28, 28, 30}; roadL = {70, 70, 74}; }           // tarmac
    else if (surf == 1) { roadD = {48, 40, 32}; roadL = {110, 95, 70}; }      // gravel
    else if (surf == 2) { roadD = {55, 38, 22}; roadL = {130, 95, 55}; }      // dirt
    else if (surf == 3) { roadD = {160, 170, 185}; roadL = {230, 235, 245}; } // snow
    else { roadD = {40, 32, 22}; roadL = {85, 65, 40}; }                      // mud
    roadD = mix3(roadD, S.amb, 0.25f);
    roadL = mix3(roadL, S.key, 0.2f);
    if (sc.wet > 0.3f) { roadD = mix3(roadD, C3{20, 25, 30}, 0.25f); roadL = mix3(roadL, C3{50, 55, 60}, 0.2f); }
    ramp(P, R_ROAD, roadD, roadL);

    // Verge / grass / desert
    C3 vgD = mix3(C3{18, 40, 16}, S.amb, 0.3f);
    C3 vgL = mix3(C3{55, 110, 40}, S.key, 0.25f);
    if (surf == 4 || sc.stage == 4 || sc.stage == 15) { // desert / salt
        vgD = mix3(C3{70, 55, 30}, S.amb, 0.3f);
        vgL = mix3(C3{180, 150, 90}, S.key, 0.2f);
    }
    if (surf == 3) { vgD = mix3(C3{140, 155, 165}, S.amb, 0.2f); vgL = mix3(C3{220, 230, 240}, S.key, 0.15f); }
    ramp(P, R_VERGE, vgD, vgL);

    ramp(P, R_ROCK, mix3(C3{45, 42, 40}, S.amb, 0.3f), mix3(C3{130, 120, 110}, S.key, 0.25f));
    ramp(P, R_SNOW, C3{170, 180, 195}, C3{240, 245, 255});
    // HUD / accents
    P[R_HUD + 0] = {0, 0, 0};
    P[R_HUD + 1] = {255, 255, 255};
    P[R_HUD + 2] = {255, 200, 40};
    P[R_HUD + 3] = {255, 60, 40};
    P[R_HUD + 4] = {80, 200, 80};
    P[R_HUD + 5] = {40, 40, 40};
    P[R_HUD + 6] = {180, 180, 180};
    for (int i = 7; i < 16; i++) P[R_HUD + i] = mix3(C3{20, 20, 20}, C3{200, 200, 200}, i / 15.f);
    ramp(P, R_CAR, C3{120, 20, 20}, C3{220, 60, 40});
    ramp(P, R_DUST, mix3(S.fog, C3{90, 80, 70}, 0.4f), mix3(S.fog, C3{180, 170, 150}, 0.3f));
    ramp(P, R_ACCENT, C3{40, 40, 50}, C3{200, 180, 100});

    // Lightning flash — lift whole palette toward cold white
    if (sc.flash > 0.01f) {
        C3 flash = {225, 230, 255};
        for (int i = 0; i < 256; i++)
            P[i] = mix3(P[i], flash, sc.flash * (i < R_ROAD ? 0.55f : 0.25f));
    }

    // Night dim
    if (S.night > 0.05f) {
        for (int i = 0; i < 256; i++) {
            if (i >= R_HUD && i < R_CAR) continue;
            P[i].r *= (1.f - S.night * 0.35f);
            P[i].g *= (1.f - S.night * 0.35f);
            P[i].b *= (1.f - S.night * 0.2f);
        }
    }

    for (int i = 0; i < 256; i++) {
        int r = clampi((int)P[i].r, 0, 255), g = clampi((int)P[i].g, 0, 255), b = clampi((int)P[i].b, 0, 255);
        s_palRGB[i * 3] = (uint8_t)r; s_palRGB[i * 3 + 1] = (uint8_t)g; s_palRGB[i * 3 + 2] = (uint8_t)b;
        uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        s_pal565[i] = (uint16_t)((v >> 8) | (v << 8));
    }
}

// Index-space fog twin (Planet technique)
static inline uint8_t fogStep(uint8_t p) {
    if (p < R_CLOUD) return (p & 15) < 14 ? p + 1 : p;                 // sky softens
    if (p >= R_CLOUD && p < R_SUN) return (p & 15) < 12 ? p + 2 : p;
    if (p >= R_RIDGE && p < R_ROAD) return (p & 15) < 12 ? p + 2 : p;
    if (p >= R_ROAD && p < R_VERGE) { int s = p - R_ROAD; return R_ROAD + (s < 12 ? s + 2 : s); }
    if (p >= R_VERGE && p < R_ROCK) { int s = p - R_VERGE; return R_VERGE + (s < 10 ? s + 3 : s); }
    if (p >= R_ROCK && p < R_SNOW) return (p & 15) < 12 ? p + 2 : p;
    if (p >= R_DUST && p < R_ACCENT) return (p & 15) < 14 ? p + 1 : p;
    return p;
}

void renderInit() {
    if (!zb) zb = (uint8_t*)memFast(W * H);
    memset(fb, 0, sizeof(fb));
    memset(zb, 255, W * H);
    S = sampleKey(0.4f);
    buildPalette();
}

bool renderProject(float wx, float wy, float wz, int& px, int& py, float& dist) {
    float dx = wx - g_cx, dy = wy - g_cy, dz = wz - g_cz;
    float rx = dx * g_cY + dz * g_sY;
    float rz = -dx * g_sY + dz * g_cY;
    float ry = dy;
    float cp = cosf(g_pitch), sp = sinf(g_pitch);
    float ry2 = ry * cp - rz * sp;
    float rz2 = ry * sp + rz * cp;
    if (rz2 < 0.4f) return false;
    dist = rz2;
    float sx = (rx / rz2) * F + W * 0.5f;
    float sy = (-ry2 / rz2) * F + H * 0.5f;
    px = (int)(sx * 2.f);
    py = (int)(sy * 2.f);
    return px > -40 && px < 360 && py > -40 && py < 280;
}

float renderFov() { return FOV; }
const uint8_t*  frameBuffer() { return fb; }
const uint16_t* framePalette565() { return s_pal565; }
const uint8_t*  framePaletteRGB() { return s_palRGB; }
int frameBob() { return 0; }

static void drawSky() {
    // Sky only above horizon; ground owns the lower half
    memset(zb, 255, W * H);
    for (int y = 0; y < HOR; y++) {
        float t = (float)y / (float)fmaxf(1, HOR - 1);
        int idx = clampi((int)(t * 15.f), 0, 15);
        uint8_t col = (uint8_t)(R_SKY + idx);
        uint8_t* row = fb + y * W;
        for (int x = 0; x < W; x++) row[x] = col;
    }
    // seed below horizon with far haze so any gaps aren't black
    for (int y = HOR; y < H; y++) {
        uint8_t* row = fb + y * W;
        for (int x = 0; x < W; x++) row[x] = R_RIDGE + 12;
    }
    // Sun disc
    if (S.elev > -0.15f && g_scene.cloud < 0.92f) {
        int sx = (int)s_sunX, sy = (int)s_sunY;
        int rad = 3 + (int)(S.elev * 4.f);
        for (int dy = -rad - 2; dy <= rad + 2; dy++) {
            for (int dx = -rad - 2; dx <= rad + 2; dx++) {
                float d = sqrtf((float)(dx * dx + dy * dy));
                int x = sx + dx, y = sy + dy;
                if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
                if (d < rad) put(x, y, R_SUN + 12);
                else if (d < rad + 2.5f) put(x, y, R_SUN + 6);
            }
        }
    }
}

static void drawClouds() {
    const Scene& sc = g_scene;
    if (sc.cloud < 0.08f && sc.storm < 0.05f) return;
    float cov = sc.cloud;
    float thresh = 0.88f - 0.55f * cov;
    float drift = g_t * 0.015f * (1.f + sc.wind * 2.f);
    int y1 = H / 2 + 4;
    for (int y = 0; y < y1; y++) {
        float rowT = (float)y / (float)y1;
        for (int x = 0; x < W; x++) {
            float u = (float)x / W + drift + g_yaw * 0.08f;
            float n = fbm2(u * 3.5f, rowT * 2.2f + g_t * 0.01f, 3, worldSeed() + 0xC1);
            if (n < thresh) continue;
            float dens = (n - thresh) / fmaxf(0.05f, 1.f - thresh);
            int shade = clampi((int)(dens * 10.f + (1.f - rowT) * 3.f), 0, 15);
            // darker belly
            if (rowT > 0.55f) shade = clampi(shade - 3, 0, 15);
            put(x, y, (uint8_t)(R_CLOUD + shade));
        }
    }
}

static void computeColumns() {
    for (int i = 0; i < W; i++) {
        colAng[i] = g_yaw + ((i + 0.5f) - W * 0.5f) / PXR;
    }
}

static void drawRidges() {
    // Far silhouette from real height samples (cheap, few depths)
    for (int x = 0; x < W; x++) {
        float a = colAng[x];
        float bestY = (float)HOR;
        for (float zr = 55.f; zr < 110.f; zr += 12.f) {
            float wx = g_cx + cosf(a) * zr;
            float wz = g_cz + sinf(a) * zr;
            float h = terrainHeight(wx, wz) - (terrainHeight(g_cx, g_cz) + 1.2f);
            float y = (float)HOR - (h * F) / zr;
            if (y < bestY) bestY = y;
        }
        int top = clampi((int)bestY, 2, HOR);
        for (int y = top; y < HOR; y++) {
            float d = (float)(y - top) / fmaxf(1.f, (float)(HOR - top));
            int sh = clampi((int)(3.f + d * 9.f), 0, 15);
            put(x, y, (uint8_t)(R_RIDGE + sh));
        }
    }
}

// Heightmap raycast per column (Comanche-style): march depth, project height, paint down the column.
static void drawRoad() {
    const Scene& sc = g_scene;
    const Junction* fork = terrainJunction();
    const float eyeH = 1.2f; // camera above local ground

    for (int bx = 0; bx < W; bx++) {
        float a = colAng[bx];
        int y = H - 1;
        // local ground under camera for relative height
        float h0 = terrainHeight(g_cx, g_cz);

        for (float zr = 1.5f; zr < 70.f && y >= 0; ) {
            float step = 0.7f + zr * 0.05f;
            float wx = g_cx + cosf(a) * zr;
            float wz = g_cz + sinf(a) * zr;
            TrackSample ts = terrainAt(wx, wz);
            float vertical = ts.height - (h0 + eyeH); // negative = below eye
            // screen row where this ground point projects
            int yHit = HOR - (int)((vertical * F) / zr);
            if (yHit > y) { zr += step; continue; } // behind current draw line

            // surface classification
            // Classify the ray against the same curved centerline used by
            // physics and terrain generation. A camera-relative straight strip
            // made the rendered road disagree with steering and off-road tests.
            float absLat = fabsf(ts.across);
            float half = ts.roadHalf;
            if (fork && fork->active) {
                const float fx = wx - fork->x, fz = wz - fork->z;
                const float leftYaw = fork->yaw + fork->armL;
                const float rightYaw = fork->yaw + fork->armR;
                const float branchLimit = 82.f;
                const float leftAlong = fx * cosf(leftYaw) + fz * sinf(leftYaw);
                const float leftAcross = -fx * sinf(leftYaw) + fz * cosf(leftYaw);
                const float rightAlong = fx * cosf(rightYaw) + fz * sinf(rightYaw);
                const float rightAcross = -fx * sinf(rightYaw) + fz * cosf(rightYaw);
                if (leftAlong >= -3.f && leftAlong <= branchLimit && fabsf(leftAcross) < half)
                    absLat = fabsf(leftAcross);
                if (rightAlong >= -3.f && rightAlong <= branchLimit && fabsf(rightAcross) < half)
                    absLat = fabsf(rightAcross);
            }

            uint32_t hh = hash32((uint32_t)(wx * 6.f) ^ (uint32_t)(wz * 4.f) ^ (uint32_t)zr);
            float grit = (hh & 255) / 255.f;
            float n = fbm2(wx * 0.1f, wz * 0.1f, 2, worldSeed() + 9);

            uint8_t base;
            int shade;
            if (absLat <= half) {
                float edge = absLat / half;
                shade = clampi((int)(9.f * (1.f - zr / 65.f) + grit * 2.2f + edge * 2.f
                                     - ts.slope * 4.f), 0, 15);
                uint8_t surf = journeyBiome(sc.stage).surface;
                base = (surf == 3) ? R_SNOW : R_ROAD;
                if (absLat < 0.28f) {
                    float dash = fmodf(zr * 1.35f + g_dist * 0.5f, 4.f);
                    if (dash < 1.5f) { base = R_HUD; shade = 1; }
                }
                if (absLat > half - 0.35f) shade = clampi(shade - 3, 0, 12);
            } else if (absLat < half + 4.f) {
                shade = clampi((int)(6.f + n * 4.f + grit * 2.f + fabsf(ts.camber) * 5.f), 0, 15);
                base = R_VERGE;
            } else {
                shade = clampi((int)(n * 8.f + grit * 3.f + (1.f - zr / 50.f) * 2.f), 0, 15);
                base = R_VERGE;
                if (n > 0.68f && grit > 0.4f) { base = R_ROCK; shade = clampi((int)(grit * 9.f), 0, 15); }
                if (journeyBiome(sc.stage).surface == 3 && n > 0.35f) {
                    base = R_SNOW; shade = clampi(shade + 2, 0, 15);
                }
                if (zr < 22.f && (hh & 0x3f) < 6) shade = clampi(shade + 3, 0, 15);
            }

            float sunSide = sinf(wrapPi(sc.sunAz - a));
            shade = clampi(shade + (int)(sunSide * 1.2f - ts.slope * 2.5f), 0, 15);

            uint8_t col = (uint8_t)(base + shade);
            float fogAmt = (1.f - expf(-zr / g_vis)) * (0.4f + sc.fog * 0.9f);
            float r = ((bx * 13 + (int)zr * 7) & 15) / 15.f;
            if (r < fogAmt) col = fogStep(col);
            if (r < fogAmt - 0.5f) col = fogStep(col);

            uint8_t zc = zcode(zr);
            // paint down the column until the projected ground row
            int yStop = clampi(yHit, 0, H - 1);
            while (y >= yStop && y >= 0) {
                int i = y * W + bx;
                if (zb[i] >= zc) { fb[i] = col; zb[i] = zc; }
                y--;
            }
            zr += step;
        }
        // leftover above last hit toward horizon: leave sky / ridges
    }
}

static void splat(int cx, int cy, int r, uint8_t c) {
    if (r <= 0) { put(cx, cy, c); return; }
    if (r > 12) r = 12;
    for (int dy = -r; dy <= r; dy++) {
        int hw = (int)sqrtf((float)(r * r - dy * dy));
        for (int dx = -hw; dx <= hw; dx++) put(cx + dx, cy + dy, c);
    }
}

static void drawTrunk(int cx, int yTop, int yBot, int tw, uint8_t bark) {
    for (int y = yTop; y <= yBot; y++) {
        float t = (float)(y - yTop) / fmaxf(1.f, (float)(yBot - yTop));
        int w = tw + (int)(t * 1.5f);
        for (int dx = -w; dx <= w; dx++) {
            uint8_t c = bark + (dx > 0 ? 1 : 0);
            put(cx + dx, y, c);
        }
    }
}

static void drawPine(int sx, int baseY, float scale, uint32_t seed) {
    int hp = clampi((int)(22.f * scale), 8, 48);
    int trunkH = hp / 6;
    int top = baseY - hp;
    int tw = clampi((int)(1.2f * scale), 1, 3);
    drawTrunk(sx, baseY - trunkH, baseY, tw, R_ROCK + 2);
    // tiers of canopy, dark under / lit top
    int tiers = 3 + (seed & 1);
    for (int t = 0; t < tiers; t++) {
        float p = (t + 0.5f) / tiers;
        float ty = top + p * (hp - trunkH) * 0.85f;
        float w = (0.35f + p * 0.9f) * hp * 0.28f;
        int th = clampi((int)(w * 0.45f), 2, 10);
        int ww = clampi((int)w, 2, 18);
        for (int dy = -th; dy <= th; dy++) {
            float u = (float)dy / th;
            int rowW = (int)(ww * (1.f - u * u * 0.5f));
            uint8_t col = (dy < -th / 3) ? (uint8_t)(R_VERGE + 10) : (uint8_t)(R_VERGE + 4);
            if (dy > th / 4) col = R_VERGE + 2; // dark belly
            for (int dx = -rowW; dx <= rowW; dx++) put(sx + dx, (int)ty + dy, col);
        }
    }
}

static void drawBroadleaf(int sx, int baseY, float scale, uint32_t seed) {
    int hp = clampi((int)(18.f * scale), 7, 40);
    int trunkH = (int)(hp * 0.4f);
    int tw = clampi((int)(1.4f * scale), 1, 3);
    drawTrunk(sx, baseY - trunkH, baseY, tw, R_ROCK + 3);
    int cy = baseY - trunkH - (int)(hp * 0.25f);
    int R = clampi((int)(hp * 0.38f), 3, 16);
    // canopy blob with shade
    for (int dy = -R; dy <= R; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy)) / R;
            if (d > 1.f) continue;
            int sh = clampi((int)(3.f + (1.f - d) * 6.f + (dx > 0 ? 2 : 0)), 0, 15);
            put(sx + dx, cy + dy, (uint8_t)(R_VERGE + sh));
        }
    }
    // a few lit splats on sun side
    uint32_t h = seed;
    for (int i = 0; i < 5; i++) {
        h = hash32(h + 17);
        int ox = (int)(((h & 15) - 7) * R / 10);
        int oy = (int)((((h >> 4) & 15) - 7) * R / 10);
        splat(sx + ox, cy + oy, 1 + (int)(R / 8), R_VERGE + 12);
    }
}

static void drawRock(int sx, int baseY, float scale, uint32_t seed) {
    int R = clampi((int)(6.f * scale), 2, 14);
    int Ry = clampi((int)(R * 0.7f), 2, 10);
    for (int dy = -Ry; dy <= Ry / 3; dy++) {
        for (int dx = -R; dx <= R; dx++) {
            float u = (float)dx / R, v = (float)dy / Ry;
            if (u * u + v * v > 1.f) continue;
            // facet via hash
            uint32_t h = hash32(seed + (dx * 13) + (dy * 31));
            int sh = clampi(3 + (int)((1.f - u * u) * 5.f) + (int)(h & 3), 0, 15);
            put(sx + dx, baseY + dy, (uint8_t)(R_ROCK + sh));
        }
    }
}

static void drawProps() {
    // Sort-ish: draw far to near by scanning; simple enough without full sort
    const FieldObj* o = fieldObjects();
    // two passes: far then near
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < FIELD_N; i++) {
            if (!o[i].on) continue;
            int px, py; float d;
            float h = o[i].type == OBJ_TREE ? o[i].b : o[i].a;
            if (!renderProject(o[i].x, o[i].y, o[i].z, px, py, d) || d > 75.f) continue;
            bool near = d < 28.f;
            if (pass == 0 && near) continue;
            if (pass == 1 && !near) continue;
            int sx = px / 2, sy = py / 2;
            float scale = fmaxf(0.35f, 14.f / d);
            g_zc = zcode(d);
            uint32_t seed = hash32((uint32_t)(o[i].x * 10.f) ^ (uint32_t)(o[i].z * 7.f) ^ o[i].var);

            if (o[i].type == OBJ_TREE) {
                if ((o[i].var & 1) == 0) drawPine(sx, sy, scale * (0.7f + o[i].b * 0.08f), seed);
                else drawBroadleaf(sx, sy, scale * (0.7f + o[i].b * 0.07f), seed);
            } else if (o[i].type == OBJ_ROCK) {
                drawRock(sx, sy, scale * o[i].a, seed);
            } else if (o[i].type == OBJ_BARRIER) {
                int w = clampi((int)(8.f * scale), 2, 12);
                int hh = clampi((int)(5.f * scale), 2, 8);
                for (int dy = -hh; dy <= 0; dy++)
                    for (int dx = -w; dx <= w; dx++)
                        put(sx + dx, sy + dy, R_ROCK + 8 + (dy & 1));
            } else if (o[i].type == OBJ_HAYBALE) {
                int w = clampi((int)(5.f * scale), 2, 9);
                int hh = clampi((int)(4.f * scale), 2, 7);
                for (int dy = -hh; dy <= 0; dy++)
                    for (int dx = -w; dx <= w; dx++)
                        if (dx * dx + dy * dy * 2 < w * w)
                            put(sx + dx, sy + dy, R_ROAD + 9);
            } else if (o[i].type == OBJ_SPECTATOR) {
                // body + head
                int hh = clampi((int)(7.f * scale), 3, 12);
                for (int dy = -hh; dy <= 0; dy++) put(sx, sy + dy, R_CAR + 6);
                splat(sx, sy - hh, clampi((int)(2.f * scale), 1, 3), R_CAR + 10);
            } else if (o[i].type == OBJ_SIGN) {
                int hh = clampi((int)(8.f * scale), 3, 14);
                for (int dy = 0; dy < hh; dy++) put(sx, sy - dy, R_ROCK + 4);
                int rw = clampi((int)(5.f * scale), 2, 8);
                for (int dy = -3; dy <= 1; dy++)
                    for (int dx = -rw; dx <= rw; dx++)
                        put(sx + dx, sy - hh + dy, R_HUD + 2);
            }

            if (d > 30.f) {
                // fog twin on a few centre pixels
                for (int dy = -2; dy <= 2; dy++)
                    for (int dx = -2; dx <= 2; dx++) {
                        int xx = sx + dx, yy = sy + dy;
                        if ((unsigned)xx < (unsigned)W && (unsigned)yy < (unsigned)H)
                            fb[yy * W + xx] = fogStep(fb[yy * W + xx]);
                    }
            }
        }
    }
    // Junction destination signs (always)
    int sn = 0;
    const RoadSign* sg = terrainSigns(sn);
    for (int i = 0; i < sn; i++) {
        if (!sg[i].active) continue;
        float h = terrainHeight(sg[i].x, sg[i].z);
        int px, py; float d;
        if (!renderProject(sg[i].x, h + 1.6f, sg[i].z, px, py, d) || d > 65.f) continue;
        int sx = px / 2, sy = py / 2;
        int rw = (int)fmaxf(4.f, 100.f / d);
        int rh = (int)fmaxf(2.f, 55.f / d);
        g_zc = zcode(d);
        for (int dy = -rh; dy <= rh; dy++)
            for (int dx = -rw; dx <= rw; dx++)
                put(sx + dx, sy + dy, R_HUD + 2);
        // chevron suggesting direction
        int dir = sg[i].isLeft ? -1 : 1;
        for (int k = 0; k < rh; k++)
            put(sx + dir * (rw - 1 - k / 2), sy - k / 2, R_HUD + 1);
        for (int dy = 0; dy < rh * 2 + 2; dy++)
            put(sx, sy + rh + dy, R_ROCK + 4);
    }
}

static void drawMist() {
    float mist = S.mist + g_scene.fog * 0.75f + g_scene.dust * 0.35f;
    if (mist < 0.03f) return;
    int hor = H / 2;
    float spread = 6.f + g_scene.fog * 10.f + g_scene.dust * 6.f;
    for (int y = 0; y < H; y++) {
        float fy = mist * 1.5f * expf(-fabsf((float)(y - hor)) / spread);
        if (fy < 0.02f) continue;
        for (int x = 0; x < W; x++) {
            float r = ((x * 13 + y * 29 + g_pan) & 15) / 15.f;
            uint8_t& p = fb[y * W + x];
            if (r < fy) p = fogStep(p);
            if (r < fy - 0.5f) p = fogStep(p);
        }
    }
}

static void drawGodRays() {
    const Scene& sc = g_scene;
    if (sc.beams < 0.08f || S.elev < -0.05f) return;
    float str = sc.beams * (0.4f + 0.6f * clampf(1.f - sc.cloud, 0.f, 1.f));
    if (str < 0.05f) return;
    int sx = (int)s_sunX, sy = (int)s_sunY;
    // March a few rays from sun through the upper half
    for (int ray = 0; ray < 7; ray++) {
        float ang = -0.55f + ray * 0.18f + sinf(g_t * 0.3f + ray) * 0.04f;
        float dx = sinf(ang), dy = cosf(ang) * 0.7f + 0.3f;
        float px = (float)sx, py = (float)sy;
        for (int step = 0; step < 40; step++) {
            px += dx * 2.2f; py += dy * 2.2f;
            int x = (int)px, y = (int)py;
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) break;
            if (y > H / 2 + 8) break;
            uint8_t p = fb[y * W + x];
            // brighten sky/cloud pixels within ramp
            if (p < R_RIDGE) {
                int base = (p < R_CLOUD) ? R_SKY : (p < R_SUN ? R_CLOUD : R_SUN);
                int sh = (p - base) + (int)(str * 4.f);
                fb[y * W + x] = (uint8_t)(base + clampi(sh, 0, 15));
            }
        }
    }
}

static void drawRainSnow() {
    const Scene& sc = g_scene;
    if (sc.rain < 0.08f && sc.snow < 0.08f) return;
    uint32_t seed = worldSeed() + (uint32_t)(g_t * 40.f);
    int n = (int)((sc.rain + sc.snow) * 80.f);
    for (int i = 0; i < n; i++) {
        uint32_t h = hash32(seed + i * 17);
        int x = (int)(h % W);
        int y = (int)((h >> 8) % H);
        if (sc.snow > sc.rain) put(x, y, R_SNOW + 12);
        else {
            // streak
            put(x, y, R_HUD + 6);
            if (y + 1 < H) put(x, y + 1, R_HUD + 6);
        }
    }
}

static void drawHud() {
    int th = (int)(g_hud.throttle * (H - 20));
    for (int y = H - 4 - th; y < H - 4; y++)
        for (int x = W - 6; x < W - 2; x++) put(x, y, R_HUD + 2);
    if (g_hud.banner && g_hud.bannerA > 0.1f)
        for (int x = 16; x < W - 16; x++) put(x, 3, R_HUD + 1);
}

void renderFrame(float x, float y, float z, float yaw, float pitch, float timeSec) {
    g_cx = x; g_cy = y + 1.15f; g_cz = z;
    g_yaw = yaw; g_pitch = pitch;
    g_cY = cosf(yaw); g_sY = sinf(yaw);
    g_t = timeSec;
    g_vis = 60.f - g_scene.fog * 28.f - g_scene.dust * 12.f - g_scene.rain * 10.f;
    g_hor = 0; g_pan = 0; g_alpha = 1.f;

    S = sampleKey(g_scene.tod);
    weatherKey(g_scene);
    g_dist = g_scene.distance;

    // Sun screen position from azimuth relative to view
    float sunRel = wrapPi(g_scene.sunAz - yaw);
    s_sunX = W * 0.5f + sunRel * F;
    s_sunY = (float)HOR - S.elev * (HOR * 0.85f) - 2.f;

    buildPalette();
    computeColumns();

    drawSky();
    drawClouds();
    drawRidges();
    drawRoad();
    drawProps();
    drawMist();
    drawGodRays();
    drawRainSnow();
    drawHud();
}

}
