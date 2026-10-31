// Mantis Rally renderer. 160x120 palette indices; the palette is rebuilt every frame from the time of day,
// weather, biome and headlights (MantisPlanet's model: artist ramps lit by ambient + key, fog as palette work).
// Order: terrain (front to back, per column) -> panorama -> sky / clouds / night -> props (z-buffered) ->
// reflections (water, wet road) -> weather -> vehicle or cockpit -> glass -> instruments.
#include "render.h"
#include "sprites.h"
#include "terrain.h"
#include "journey.h"
#include "scene.h"
#include "field.h"
#include "vehicle.h"
#include "universe.h"
#include "mem.h"
#include "vehicle_art.h"
#include "cockpit_art.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#if !defined(HOST) && !defined(MR_NO_IRAM)
#include <esp_attr.h>
#define MR_HOT IRAM_ATTR        // inner loops in instruction RAM (no flash-cache stalls), as MantisPlanet does
#define MR_FAST DRAM_ATTR
#else
#define MR_HOT
#define MR_FAST
#endif

namespace mantis {

ViewInfo g_view;

static constexpr int W = FB_W, H = FB_H;
static uint8_t fb[W * H];
static uint8_t* zb = nullptr;
static uint16_t pal565[256];
static uint8_t palRGB[256 * 3];
static uint8_t s_refl[256];                // a palette entry's reflection (darker, same ramp)
static uint8_t s_bright[256];              // one step brighter in its ramp (light shafts)
static float s_light[256];                 // normal byte -> sunlight 0..1
static float s_fog[520];                   // fog by distance (m)
static uint8_t s_veh = 0;
static uint8_t s_bottom[W];                // rows at and below this are the cockpit's (not raycast)
static uint8_t s_bottomCock[W];
static uint8_t* s_cock = nullptr;          // decoded cockpit, 0 = glass
static uint8_t* s_wheelMask = nullptr;     // cockpit pixels that are the steering wheel
static float s_bob = 0.f;

static MR_FAST uint8_t BAYER[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
static inline float dith(int x, int y) { return (BAYER[((y & 3) << 2) | (x & 3)] + 0.5f) * (1.f / 16.f); }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
static inline uint8_t zcode(float d) { float c = sqrtf(d > 0.f ? d : 0.f) * 11.f; return (uint8_t)((int)(c > 252.f ? 252.f : c) & ~1); }
static inline int band(float f, int x, int y) {              // Planet's pixel-art banding
    int i = (int)f; float fr = f - i;
    if (fr > 0.86f) return i + 1;
    if (fr > 0.62f && ((x ^ y) & 1)) return i + 1;
    return i;
}
static inline float fastAtan2(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y);
    float a = fminf(ax, ay) / (fmaxf(ax, ay) + 1e-9f);
    float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

// ================================================================================== palette
struct C3 { float r, g, b; };
static inline C3 mix(C3 a, C3 b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
static inline C3 scale(C3 a, float s) { return {a.r * s, a.g * s, a.b * s}; }
static inline C3 add(C3 a, C3 b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
static inline C3 mul(C3 a, C3 b) { return {a.r * b.r / 255.f, a.g * b.g / 255.f, a.b * b.b / 255.f}; }
static inline C3 c3(const uint8_t* p) { return {(float)p[0], (float)p[1], (float)p[2]}; }

struct Key { float t; C3 zen, hor, glow, cDark, cLit, sun, amb, key, fog; float elev, night, mist; };
#define NIGHT_KEY(T, E, M) {T, {6,5,20},{24,18,52},{40,24,70},{18,12,40},{95,55,150},{215,220,255},{70,75,120},{60,70,150},{26,22,55}, E, 1.0f, M}
static const Key KEYS[] = {             // MantisPlanet's day
    NIGHT_KEY(0.00f, -0.40f, 0.25f),
    {0.19f, {18,14,50},{75,42,105},{160,75,115},{40,28,75},{190,105,150},{255,200,160},{105,92,140},{190,105,125},{85,58,115}, -0.06f, 0.55f, 0.55f},
    {0.26f, {58,48,128},{238,118,92},{255,188,112},{105,58,118},{255,168,110},{255,240,190},{160,130,140},{255,150,80},{195,118,128}, 0.07f, 0.0f, 0.60f},
    {0.36f, {70,120,210},{188,208,235},{255,240,200},{138,150,190},{255,250,240},{255,255,230},{220,220,230},{255,230,180},{168,192,222}, 0.42f, 0.0f, 0.25f},
    {0.50f, {40,100,215},{168,208,245},{240,248,255},{150,165,200},{255,255,255},{255,255,240},{255,255,255},{255,245,220},{160,195,235}, 0.80f, 0.0f, 0.10f},
    {0.63f, {62,90,190},{218,190,172},{255,220,150},{150,128,170},{255,230,190},{255,245,200},{230,215,205},{255,210,140},{190,170,182}, 0.36f, 0.0f, 0.15f},
    {0.71f, {82,50,142},{255,118,70},{255,200,90},{120,50,110},{255,148,90},{255,240,170},{170,115,128},{255,128,50},{190,90,120}, 0.06f, 0.0f, 0.35f},
    {0.79f, {35,22,75},{178,50,100},{255,110,70},{60,30,80},{220,90,125},{255,180,120},{105,78,115},{200,70,80},{108,45,95}, -0.08f, 0.35f, 0.45f},
    NIGHT_KEY(0.89f, -0.35f, 0.30f),
    NIGHT_KEY(1.00f, -0.40f, 0.25f),
};
static constexpr int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);
static Key S;
static Key sampleKey(float tod) {
    int k = 0;
    while (k < NKEYS - 2 && tod >= KEYS[k + 1].t) k++;
    const Key &A = KEYS[k], &B = KEYS[k + 1];
    float t = clampf((tod - A.t) / (B.t - A.t), 0.f, 1.f); t = t * t * (3.f - 2.f * t);
    Key o; o.t = tod;
    o.zen = mix(A.zen, B.zen, t); o.hor = mix(A.hor, B.hor, t); o.glow = mix(A.glow, B.glow, t);
    o.cDark = mix(A.cDark, B.cDark, t); o.cLit = mix(A.cLit, B.cLit, t); o.sun = mix(A.sun, B.sun, t);
    o.amb = mix(A.amb, B.amb, t); o.key = mix(A.key, B.key, t); o.fog = mix(A.fog, B.fog, t);
    o.elev = A.elev + (B.elev - A.elev) * t; o.night = A.night + (B.night - A.night) * t; o.mist = A.mist + (B.mist - A.mist) * t;
    return o;
}
struct Ramp4 { C3 k[4]; };
static C3 rampAt(const Ramp4& r, float s) {
    s = clampf(s, 0.f, 1.f) * 3.f; int i = (int)s;
    return i >= 3 ? r.k[3] : mix(r.k[i], r.k[i + 1], s - i);
}
// an artist ramp from a biome's dark / lit pair: shadows lean cool, highlights warm (Planet's rule)
static Ramp4 artist(const uint8_t* d, const uint8_t* l) {
    C3 D = c3(d), Lc = c3(l);
    Ramp4 r;
    r.k[0] = mix(scale(D, 0.55f), C3{20, 30, 60}, 0.35f);
    r.k[1] = D;
    r.k[2] = mix(D, Lc, 0.6f);
    r.k[3] = mix(Lc, C3{255, 236, 170}, 0.18f);
    return r;
}
static Ramp4 blendR(const Ramp4& a, const Ramp4& b, float t) { Ramp4 r; for (int i = 0; i < 4; i++) r.k[i] = mix(a.k[i], b.k[i], t); return r; }

static float s_night = 0.f, s_lights = 0.f, s_sunK = 1.f;
static float s_sunDx = 0.f, s_sunDy = 1.f, s_sunDz = 0.f;

static void buildPalette() {
    const Scene& sc = g_scene;
    const Biome& A = biome(sc.biomeA); const Biome& B = biome(sc.biomeB);
    float bl = sc.blend;
    S = sampleKey(sc.tod);
    // weather washes colour out (Planet's weatherKey)
    float grey = clampf(sc.cloud * 0.6f - 0.15f + sc.rain * 0.45f + sc.storm * 0.25f + sc.snow * 0.2f, 0.f, 0.9f);
    float dark = 1.f - 0.28f * sc.rain - 0.25f * sc.storm;
    auto desat = [](C3 c, float k, float d) -> C3 { float l = c.r * 0.3f + c.g * 0.55f + c.b * 0.15f; return scale(mix(c, C3{l * 0.92f, l * 0.96f, l * 1.05f}, k), d); };
    S.zen = desat(S.zen, grey, dark); S.hor = desat(S.hor, grey * 0.8f, dark); S.glow = desat(S.glow, grey * 0.9f, dark);
    S.cDark = desat(S.cDark, grey, dark * 0.85f); S.cLit = desat(S.cLit, grey * 0.8f, dark); S.fog = desat(S.fog, grey * 0.7f, dark);
    S.amb = scale(S.amb, 1.f - 0.25f * grey); 
    s_sunK = (1.f - 0.75f * grey) * clampf((S.elev + 0.05f) * 4.f, 0.f, 1.f);
    s_night = S.night;
    s_lights = sc.lights;
    // the key light: the sun by day; the headlights by night
    C3 key = scale(S.key, 1.f - 0.75f * grey);
    C3 HEAD = {255, 238, 205};
    key = mix(key, HEAD, clampf(s_lights * (0.25f + S.night * 0.9f), 0.f, 1.f));
    C3 amb = S.amb;
    if (S.night > 0.3f) amb = add(amb, scale(C3{14, 18, 34}, sc.moon * S.night));
    C3 P[256];
    memset(P, 0, sizeof(P));
    auto litR = [&](const Ramp4& r, float s) -> C3 {
        C3 b = rampAt(r, s);
        return add(scale(mul(b, amb), 0.55f + 0.45f * s), scale(mul(b, key), s * s * 0.55f));
    };
    Ramp4 grass = blendR(artist(A.grassD, A.grassL), artist(B.grassD, B.grassL), bl);
    Ramp4 soil = blendR(artist(A.soilD, A.soilL), artist(B.soilD, B.soilL), bl);
    Ramp4 rock = blendR(artist(A.rockD, A.rockL), artist(B.rockD, B.rockL), bl);
    Ramp4 leaf = blendR(artist(A.leafD, A.leafL), artist(B.leafD, B.leafL), bl);
    C3 acc = mix(c3(A.accent), c3(B.accent), bl);
    C3 wat = mix(c3(A.waterC), c3(B.waterC), bl);
    C3 mtn = mix(c3(A.mtn), c3(B.mtn), bl);
    uint8_t surf = bl < 0.5f ? A.surface : B.surface;
    if (sc.wet > 0.2f) { for (int i = 0; i < 4; i++) { soil.k[i] = scale(soil.k[i], 1.f - 0.25f * sc.wet); grass.k[i] = mix(grass.k[i], C3{16, 50, 52}, 0.2f * sc.wet); } }
    Ramp4 asph;
    if (surf == SURF_SNOW) asph = {{{120, 130, 160}, {170, 180, 205}, {215, 222, 238}, {245, 248, 255}}};
    else asph = {{{22, 22, 34}, {44, 44, 56}, {78, 76, 84}, {128, 124, 122}}};
    if (sc.wet > 0.2f) for (int i = 0; i < 4; i++) asph.k[i] = scale(asph.k[i], 1.f - 0.3f * sc.wet);
    static const Ramp4 SNOWR = {{{120, 140, 190}, {180, 196, 228}, {226, 234, 248}, {255, 255, 255}}};
    static const Ramp4 BARKR = {{{40, 24, 32}, {88, 52, 36}, {150, 92, 52}, {210, 150, 90}}};
    Ramp4 leaf2 = {{scale(acc, 0.3f), scale(acc, 0.6f), acc, mix(acc, C3{255, 250, 220}, 0.3f)}};
    Ramp4 haze = {{scale(S.fog, 0.55f), scale(S.fog, 0.8f), S.fog, mix(S.fog, S.hor, 0.6f)}};
    Ramp4 mt = {{mix(scale(mtn, 0.5f), C3{30, 30, 60}, 0.3f), mtn, mix(mtn, C3{200, 190, 180}, 0.4f), C3{235, 238, 250}}};
    for (int i = 0; i < 16; i++) {
        float t = i / 15.f;
        P[P_SKY + i] = i < 10 ? mix(S.zen, S.hor, powf(i / 9.f, 1.15f)) : mix(S.hor, S.glow, (i - 9) / 6.f);
        C3 c = mix(S.cDark, S.cLit, t);
        P[P_CLOUD + i] = mix(c, S.hor, 0.22f * sinf(t * 3.14159f));
        // mountains: 0..7 far (hazed), 8..15 near
        float s = (i & 7) / 7.f;
        C3 m = rampAt(mt, s * 0.92f + (s > 0.85f ? 0.08f : 0.f));
        C3 ml = add(scale(mul(m, amb), 0.55f + 0.5f * s), scale(mul(m, key), s * s * 0.45f));
        P[P_MTN + i] = mix(ml, S.fog, i < 8 ? 0.5f : 0.25f);
        P[P_HAZE + i] = rampAt(haze, t);
        P[P_GRASS + i] = litR(grass, t);
        P[P_SOIL + i] = litR(soil, t);
        P[P_ROCK + i] = litR(rock, t);
        P[P_ROAD + i] = litR(asph, t);
        P[P_LEAF + i] = litR(leaf, t);
    }
    for (int i = 0; i < 8; i++) {
        float t = i / 7.f;
        P[P_SUN + i] = i == 7 ? mix(S.sun, C3{255, 255, 255}, 0.4f) : mix(S.glow, S.sun, t);
        P[P_LEAF2 + i] = litR(leaf2, t);
        P[P_SNOW + i] = litR(SNOWR, t);
        P[P_BARK + i] = litR(BARKR, t);
        P[P_WATER + i] = add(scale(mul(wat, amb), 0.5f + 0.5f * t), scale(mul(wat, key), t * 0.3f));
    }
    // special: rainbow by day, aurora by night
    static const C3 BOW[8] = {{150, 90, 230}, {80, 120, 255}, {70, 200, 235}, {90, 225, 110}, {250, 235, 90}, {255, 160, 60}, {240, 70, 60}, {255, 255, 255}};
    static const C3 AUR[8] = {{8, 30, 44}, {20, 90, 80}, {40, 170, 110}, {60, 225, 140}, {120, 245, 170}, {120, 140, 230}, {170, 100, 240}, {220, 160, 255}};
    for (int i = 0; i < 8; i++) P[P_SPECIAL + i] = S.night > 0.5f ? AUR[i] : mix(BOW[i], P[P_SKY + 9], 0.25f);
    // flowers: lit, saturated
    static const C3 FL[8] = {{250, 250, 240}, {255, 220, 60}, {200, 120, 230}, {255, 130, 170}, {230, 60, 60}, {110, 150, 255}, {255, 160, 60}, {245, 245, 255}};
    for (int i = 0; i < 8; i++) P[P_FLOWER + i] = mix(add(scale(mul(FL[i], amb), 0.8f), scale(mul(FL[i], key), 0.45f)), mul(FL[i], amb), 0.3f);
    if (A.accent[0] > 0) P[P_FLOWER + 2] = mix(P[P_FLOWER + 2], litR(leaf2, 0.8f), 0.6f);
    // the vehicle and its cockpit, lit by the scene
    const art::VehArt& va = art::VEH_ART[s_veh];
    const art::CockArt& ca = art::COCK_ART[s_veh];
    for (int i = 0; i < 15; i++) {
        C3 v = {(float)va.pal[i * 3], (float)va.pal[i * 3 + 1], (float)va.pal[i * 3 + 2]};
        C3 lit = add(scale(mul(v, amb), 0.75f), scale(mul(v, key), 0.42f));
        P[P_VEH + 1 + i] = lit;
        C3 k = {(float)ca.pal[i * 3], (float)ca.pal[i * 3 + 1], (float)ca.pal[i * 3 + 2]};
        float satk = fmaxf(k.r, fmaxf(k.g, k.b)) - fminf(k.r, fminf(k.g, k.b));
        C3 kl = add(scale(mul(k, amb), 0.62f), scale(mul(k, S.key), 0.3f * (1.f - S.night)));
        if (satk > 60.f && k.g > k.r) kl = mix(kl, k, 0.6f + 0.4f * S.night);      // instrument glow
        P[P_COCK + 1 + i] = kl;
    }
    // emissive
    P[P_HUD_W] = {242, 246, 250}; P[P_HUD_A] = {255, 190, 50}; P[P_HUD_R] = {255, 70, 60}; P[P_HUD_G] = {120, 255, 120};
    P[P_HUD_K] = {12, 14, 20}; P[P_FIREFLY] = {225, 255, 130}; P[P_WINDOW] = mix(C3{60, 70, 80}, C3{255, 200, 110}, S.night);
    P[P_BEAM] = mix(S.fog, C3{255, 240, 200}, 0.35f);
    // paint is retro-reflective: it shines in the headlights
    for (int i = 0; i < 4; i++) {
        float t = i / 3.f;
        P[P_LINE_W + i] = add(scale(mul(C3{235, 235, 228}, amb), 0.5f + 0.4f * t), scale(mul(C3{235, 235, 228}, key), t * 0.6f));
        P[P_LINE_Y + i] = add(scale(mul(C3{240, 196, 40}, amb), 0.5f + 0.4f * t), scale(mul(C3{240, 196, 40}, key), t * 0.6f));
    }
    P[P_SIGN_G] = add(scale(mul(C3{20, 110, 60}, amb), 0.8f), scale(mul(C3{20, 110, 60}, key), 0.5f));
    P[P_SIGN_W] = add(scale(mul(C3{240, 240, 240}, amb), 0.7f), scale(mul(C3{240, 240, 240}, key), 0.6f + s_lights * S.night * 0.4f));
    P[P_TAIL_D] = {120, 16, 24}; P[P_TAIL] = {255, 40, 40}; P[P_AMBER] = {255, 170, 40}; P[P_BLACK] = {10, 10, 14};
    P[P_BOLT] = {235, 235, 255}; P[P_MUD] = mul(C3{70, 48, 30}, add(scale(amb, 0.7f), scale(key, 0.3f)));
    // lightning lifts everything toward a cold white
    if (sc.flash > 0.01f) for (int i = 0; i < 232; i++) P[i] = mix(P[i], C3{225, 230, 255}, sc.flash * (i < P_GRASS ? 0.6f : 0.3f));
    for (int i = 0; i < 256; i++) {
        int r = clampi((int)P[i].r, 0, 255), g = clampi((int)P[i].g, 0, 255), b = clampi((int)P[i].b, 0, 255);
        palRGB[i * 3] = r; palRGB[i * 3 + 1] = g; palRGB[i * 3 + 2] = b;
        uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        pal565[i] = (uint16_t)((v >> 8) | (v << 8));
    }
}

// ramp tables for reflections and light shafts (static: built once)
static void buildLUTs() {
    for (int i = 0; i < 256; i++) { s_refl[i] = (uint8_t)i; s_bright[i] = (uint8_t)i; }
    auto r16 = [](int base) { for (int k = 0; k < 16; k++) { s_refl[base + k] = (uint8_t)(base + (k > 2 ? k - 2 : 0)); s_bright[base + k] = (uint8_t)(base + (k < 14 ? k + 2 : 15)); } };
    auto r8 = [](int base) { for (int k = 0; k < 8; k++) { s_refl[base + k] = (uint8_t)(base + (k > 1 ? k - 1 : 0)); s_bright[base + k] = (uint8_t)(base + (k < 7 ? k + 1 : 7)); } };
    r16(P_SKY); r16(P_CLOUD); r16(P_MTN); r16(P_HAZE); r16(P_GRASS); r16(P_SOIL); r16(P_ROCK); r16(P_ROAD); r16(P_LEAF);
    r8(P_SUN); r8(P_LEAF2); r8(P_SNOW); r8(P_BARK); r8(P_WATER);
    for (int k = 0; k < 8; k++) s_refl[P_SUN + k] = (uint8_t)(P_SUN + k);     // the sun glitters, undimmed
}

// ================================================================================== camera
static float cx, cy, cz, cyaw, F, fx, fz, rx, rz, hor0, rollT;
static float horX[W], kxC[W], dxC[W], dzC[W], azC[W], beamC[W], sideC[W];
static uint8_t ymin[W];
static float s_camYaw = 0.f, s_camY = 0.f;
static bool s_camInit = false;
static float s_t = 0.f;

bool renderProject(float wx, float wy, float wz, float& sx, float& sy, float& depth) {
    float dx = wx - cx, dz = wz - cz;
    depth = dx * fx + dz * fz;
    if (depth < 0.3f) return false;
    float lat = dx * rx + dz * rz;
    sx = W * 0.5f + lat / depth * F;
    sy = hor0 + (sx - W * 0.5f) * rollT - (wy - cy) * F / depth;
    return true;
}

static void setupCamera() {
    const ViewInfo& v = g_view;
    const VehicleSpec& V = vehicleSpec(v.vehicle);
    float pitch, roll;
    if (v.view == VIEW_CHASE) {
        float back = v.vehicle == V_BIKE ? 3.9f : 6.6f;
        if (!s_camInit) { s_camYaw = v.yaw; s_camY = v.groundY; s_camInit = true; }
        s_camYaw += wrapPi(v.yaw - s_camYaw) * clampf(g_scene.dt * 3.2f, 0.f, 1.f);
        s_camY += (v.groundY - s_camY) * clampf(g_scene.dt * 5.f, 0.f, 1.f);
        cyaw = s_camYaw;
        cx = v.x - cosf(cyaw) * back; cz = v.z - sinf(cyaw) * back;
        float gh = terrainHeight(cx, cz);
        cy = fmaxf(s_camY + 1.65f, gh + 1.0f);
        F = 120.f; pitch = -0.0873f; roll = 0.f;
        hor0 = H * 0.5f + tanf(pitch) * F;
    } else {
        cyaw = v.yaw;
        float c = cosf(v.yaw), s = sinf(v.yaw);
        cx = v.x + c * V.eyeBack; cz = v.z + s * V.eyeBack;
        cy = v.groundY + V.eyeHeight + v.bounce * 0.6f;
        F = v.view == VIEW_COCKPIT ? 92.f : 96.f;
        pitch = v.pitch * 0.75f - 0.05f;
        roll = V.bike ? v.lean * 0.45f + v.roll * 0.3f : v.roll * 0.6f;
        hor0 = H * 0.47f + tanf(pitch) * F;
    }
    fx = cosf(cyaw); fz = sinf(cyaw); rx = -fz; rz = fx;
    rollT = -tanf(roll);                 // camera rolled right (right side down): the horizon's right end rises
    for (int x = 0; x < W; x++) {
        float kx = (x + 0.5f - W * 0.5f) / F;
        kxC[x] = kx; dxC[x] = fx + rx * kx; dzC[x] = fz + rz * kx;
        horX[x] = hor0 + (x + 0.5f - W * 0.5f) * rollT;
        azC[x] = cyaw + atanf(kx);
        float b = 1.f - (kx * kx) / 0.17f;
        beamC[x] = b > 0.f ? b : 0.f;
        sideC[x] = sinf(wrapPi(g_scene.sunAz - azC[x]));
    }
}

// ================================================================================== terrain
struct Smp { float h, a, al, lt; uint8_t mat, surf, half, extra; };
static uint8_t* s_tex = nullptr;            // 64x64 value noise, 0..255
static inline int tex(int u, int v) { return s_tex[((v & 63) << 6) | (u & 63)]; }
// distant samples: height bilinear (silhouettes stay smooth), everything else from the nearest cell
static inline bool sampleLite(const Grid& g, float wx, float wz, Smp& s) {
    float gx = wx * g.inv, gz = wz * g.inv;
    int ix = (int)(gx + 4096.f) - 4096, iz = (int)(gz + 4096.f) - 4096;
    if (ix < g.ox + 1 || iz < g.oz + 1 || ix + 2 >= g.ox + g.N || iz + 2 >= g.oz + g.N) return false;
    float tx = gx - ix, tz = gz - iz;
    int N = g.N;
    int x0 = ix - g.ox; x0 = (x0 + (g.ox % N + N)) % N;
    int z0 = iz - g.oz; z0 = (z0 + (g.oz % N + N)) % N;
    int x1 = x0 + 1 == N ? 0 : x0 + 1, z1 = z0 + 1 == N ? 0 : z0 + 1;
    const Cell &a = g.c[z0 * N + x0], &b = g.c[z0 * N + x1], &c = g.c[z1 * N + x0], &d = g.c[z1 * N + x1];
    float h0 = a.h + (b.h - a.h) * tx, h1 = c.h + (d.h - c.h) * tx;
    s.h = (h0 + (h1 - h0) * tz) * 0.1f;
    const Cell& n = tx < 0.5f ? (tz < 0.5f ? a : c) : (tz < 0.5f ? b : d);
    s.a = n.across * 0.02f; s.lt = s_light[n.nrm]; s.al = n.along * 0.05f;
    s.mat = n.mat & 15; s.surf = n.mat >> 4; s.extra = n.extra;
    s.half = (s.mat == MAT_ROAD || s.mat == MAT_SHOULDER) ? n.extra : 68;
    return true;
}
static inline bool sampleAt(const Grid& g, float wx, float wz, Smp& s) {
    float gx = wx * g.inv, gz = wz * g.inv;
    int ix = (int)floorf(gx), iz = (int)floorf(gz);
    if (ix < g.ox + 1 || iz < g.oz + 1 || ix + 2 >= g.ox + g.N || iz + 2 >= g.oz + g.N) return false;
    float tx = gx - ix, tz = gz - iz;
    int N = g.N;
    int x0 = ix % N; if (x0 < 0) x0 += N;
    int z0 = iz % N; if (z0 < 0) z0 += N;
    int x1 = x0 + 1 == N ? 0 : x0 + 1, z1 = z0 + 1 == N ? 0 : z0 + 1;
    const Cell &a = g.c[z0 * N + x0], &b = g.c[z0 * N + x1], &c = g.c[z1 * N + x0], &d = g.c[z1 * N + x1];
    float h0 = a.h + (b.h - a.h) * tx, h1 = c.h + (d.h - c.h) * tx;
    s.h = (h0 + (h1 - h0) * tz) * 0.1f;
    float a0 = a.across + (b.across - a.across) * tx, a1 = c.across + (d.across - c.across) * tx;
    s.a = (a0 + (a1 - a0) * tz) * 0.02f;
    const Cell& n = tx < 0.5f ? (tz < 0.5f ? a : c) : (tz < 0.5f ? b : d);
    float l0 = s_light[a.nrm] + (s_light[b.nrm] - s_light[a.nrm]) * tx, l1 = s_light[c.nrm] + (s_light[d.nrm] - s_light[c.nrm]) * tx;
    s.lt = l0 + (l1 - l0) * tz;
    s.al = n.along * 0.05f;
    s.mat = n.mat & 15; s.surf = n.mat >> 4; s.extra = n.extra;
    s.half = (s.mat == MAT_ROAD || s.mat == MAT_SHOULDER) ? n.extra : 68;
    return true;
}

static float s_flowers = 0.f, s_wet = 0.f, s_paved = 1.f;

// one ground pixel. Returns the palette index; sets *refl for water / wet surfaces.
static inline uint8_t groundPix(int x, int y, float wx, float wz, float a, float al, float half, float lt, float z, float hgt,
                                uint8_t mat, uint8_t surf, bool farSmp, uint8_t extra, bool& refl) {
    float bay = dith(x, y);
    int scl = z < 12.f ? 5 : (z < 35.f ? 2 : 1);
    int n = tex((int)(wx * scl), (int)(wz * scl));
    float nf = n * (1.f / 255.f);
    float s = lt * 12.f + (nf - 0.5f) * 3.f;
    if (s_lights > 0.02f && z < 75.f) s += s_lights * beamC[x] * 10.f * fminf(1.f, 11.f / (z + 3.f)) * (0.35f + 0.65f * s_night);
    float aa = fabsf(a);
    float lw = fmaxf(0.10f, z * 0.55f / F);           // paint never thinner than ~half a pixel
    uint8_t c;
    if (aa < half && mat != MAT_WATER) {
        if (surf == SURF_TARMAC) {
            int lv = clampi(band(s * 0.9f + 1.f, x, y), 0, 15);
            c = (uint8_t)(P_ROAD + lv);
            if (tex((int)(wx * 9.f), (int)(wz * 9.f)) > 228 && z < 25.f) c = (uint8_t)(P_ROAD + clampi(lv + 2, 0, 15));
            float e = fabsf(aa - (half - 0.32f));
            float m = al;
            float sl = s * 0.25f + 0.2f;
            if (e < lw) c = (uint8_t)(P_LINE_W + clampi((int)sl, 0, 3));
            else if (aa < lw + 0.05f && m < 4.6f) c = (uint8_t)(P_LINE_Y + clampi((int)sl, 0, 3));
            if (s_wet > 0.25f) refl = true;
        } else if (surf == SURF_SNOW) {
            float rut = fabsf(aa - half * 0.48f);
            int lv = (int)(s * 0.5f + 1.5f - (rut < 0.45f ? 2.f : 0.f) + bay);
            c = (uint8_t)(P_SNOW + clampi(lv, 0, 7));
        } else {
            float rut = fabsf(aa - half * 0.48f);
            float sh = s + 1.0f;
            if (rut < 0.5f) sh -= surf == SURF_MUD ? 3.f : 1.8f;
            if (aa < 0.45f) sh += 0.8f;
            if (surf == SURF_GRAVEL && z < 22.f && tex((int)(wx * 11.f), (int)(wz * 11.f)) > 205) sh += 3.f;
            if (surf == SURF_SAND) sh += 1.5f + sinf(wx * 1.3f + wz * 0.7f) * 0.8f;
            if (surf == SURF_DIRT) sh -= 0.6f;
            c = (uint8_t)(P_SOIL + clampi(band(sh, x, y), 0, 15));
            if (surf == SURF_MUD) {
                c = (uint8_t)(P_SOIL + clampi(band(sh * 0.7f - 1.f, x, y), 0, 15));
                if (rut < 0.55f && tex((int)(wx * 1.5f), (int)(wz * 1.5f)) > 120) refl = true;
            } else if (s_wet > 0.5f && rut < 0.4f && tex((int)(wx * 1.2f), (int)(wz * 1.2f)) > 170) refl = true;
        }
    } else if (aa < half + 1.3f && mat != MAT_WATER) {
        int lv = band(s * 0.9f + (s_paved > 0.5f ? 1.f : 0.f), x, y);
        c = (uint8_t)(P_SOIL + clampi(lv, 0, 15));
        if (nf > 0.62f && aa > half + 0.7f) c = (uint8_t)(P_GRASS + clampi(lv, 0, 15));
    } else {
        switch (mat) {
            case MAT_WATER: {
                int r = (int)((nf - 0.5f) * 4.f + 3.f + (z < 20.f ? 1.f : 0.f));
                refl = true;
                return (uint8_t)(P_WATER + clampi(r, 0, 7));
            }
            case MAT_ROCK: {
                float strata = sinf(hgt * 2.4f + nf * 2.f) * 1.3f;
                c = (uint8_t)(P_ROCK + clampi(band(s + strata + 0.5f, x, y), 0, 15));
                break;
            }
            case MAT_SNOW: c = (uint8_t)(P_SNOW + clampi((int)(s * 0.55f + 1.f + bay), 0, 7)); break;
            case MAT_SAND: c = (uint8_t)(P_SOIL + clampi(band(s + 2.f + sinf(wx * 0.9f + wz * 0.5f) * 0.9f, x, y), 0, 15)); break;
            case MAT_SOIL: c = (uint8_t)(P_SOIL + clampi(band(s, x, y), 0, 15)); break;
            case MAT_MUD:
                c = (uint8_t)(P_SOIL + clampi(band(s * 0.6f - 1.f, x, y), 0, 15));
                if (nf > 0.6f) { refl = true; }
                break;
            case MAT_LOT: c = (uint8_t)(P_SOIL + clampi(band(s + 1.f, x, y), 0, 15)); break;
            default: {
                if (farSmp && extra > 0 && mat < MAT_WATER) {                      // distant forest canopy
                    int lv = (int)(s * 0.8f - 1.f + nf * 3.f);
                    return (uint8_t)(P_LEAF + clampi(lv, 0, 15));
                }
                float blades = z < 14.f ? (tex((int)(wx * 9.f), (int)(wz * 3.f)) - 128) * (1.f / 60.f) : 0.f;
                c = (uint8_t)(P_GRASS + clampi(band(s + blades, x, y), 0, 15));
                if (s_flowers > 0.05f && z < 40.f) {
                    int fl = tex((int)(wx * 3.f) * 7 + 13, (int)(wz * 3.f) * 5 + 29);
                    if (fl > 250 - (int)(s_flowers * 30.f)) c = (uint8_t)(P_FLOWER + ((int)(wx * 3.f + wz * 7.f) & 7));
                }
            }
        }
    }
    return c;
}

MR_HOT static void drawTerrain(float zFar) {
    const Grid& gN = terrainNear();
    const Grid& gF = terrainFar();
    float maxH = fmaxf(gN.maxH, gF.maxH);
    for (int x = 0; x < W; x++) {
        int y = s_bottom[x];
        const float ddx = dxC[x], ddz = dzC[x], hx = horX[x];
        float z = 0.55f;
        // previous sample (for per-pixel interpolation)
        float pY = (float)H + 40.f, pz = z, pwx = cx + ddx * z, pwz = cz + ddz * z, pa = 600.f, pal = 0.f, plt = 0.5f, ph = cy - 2.f;
        uint8_t pmat = 0, psurf = 0, phalf = 68, pextra = 0; bool pfar = false;
        bool first = true, inFar = false;
        while (z < zFar && y > 0) {
            float wx = cx + ddx * z, wz = cz + ddz * z;
            Smp s; bool far = inFar;
            if (!inFar) {
                bool ok = z < 45.f ? sampleAt(gN, wx, wz, s) : sampleLite(gN, wx, wz, s);
                if (!ok) { inFar = far = true; }
            }
            if (far) {
                if (!sampleLite(gF, wx, wz, s)) break;
                if (s.extra && s.mat < MAT_WATER) s.h += s.extra * (0.35f + 0.65f * tex((int)(wx * 0.6f), (int)(wz * 0.6f)) * (1.f / 255.f));
            }
            float ys = hx - (s.h - cy) * F / z;
            if (first) { pY = fmaxf(ys, (float)H + 1.f); first = false; pa = s.a; pal = s.al; plt = s.lt; ph = s.h; pmat = s.mat; psurf = s.surf; phalf = s.half; pextra = s.extra; pfar = far; }
            if (ys < y) {
                int yTop = (int)ceilf(ys);
                if (yTop < 0) yTop = 0;
                float span = pY - ys;
                float inv = span > 0.001f ? 1.f / span : 0.f;
                float fogNear = s_fog[clampi((int)pz, 0, 519)], fogFar = s_fog[clampi((int)z, 0, 519)];
                for (int yy = y - 1; yy >= yTop; yy--) {
                    float t = clampf((pY - yy) * inv, 0.f, 1.f);
                    float zz = pz + (z - pz) * t;
                    float wxx = pwx + (wx - pwx) * t, wzz = pwz + (wz - pwz) * t;
                    float aa = pa + (s.a - pa) * t;
                    float dal = s.al - pal; if (dal > 6.4f) dal -= 12.8f; else if (dal < -6.4f) dal += 12.8f;
                    float al = pal + dal * t; if (al < 0.f) al += 12.8f;
                    float lt = plt + (s.lt - plt) * t;
                    float hg = ph + (s.h - ph) * t;
                    bool useCur = t > dith(x, yy);
                    uint8_t mat = useCur ? s.mat : pmat, surf = useCur ? s.surf : psurf;
                    float half = (useCur ? s.half : phalf) * 0.05f;
                    bool refl = false;
                    uint8_t c = groundPix(x, yy, wxx, wzz, aa, al, half, lt, zz, hg, mat, surf, useCur ? far : pfar, useCur ? s.extra : pextra, refl);
                    float fg = fogNear + (fogFar - fogNear) * t;
                    if (fg > 0.8f || (fg > 0.6f && ((x ^ yy) & 1))) {
                        int hz = clampi((int)(9.f - (yy - hx) * 0.15f + ((c & 15) - 8) * 0.25f), 0, 15);
                        c = (uint8_t)(P_HAZE + hz); refl = false;
                    }
                    int i = yy * W + x;
                    fb[i] = c;
                    zb[i] = (uint8_t)(zcode(zz) | (refl ? 1 : 0));
                }
                y = yTop;
            }
            pY = ys; pz = z; pwx = wx; pwz = wz; pa = s.a; pal = s.al; plt = s.lt; ph = s.h; pmat = s.mat; psurf = s.surf; phalf = s.half; pextra = s.extra; pfar = far;
            // nothing beyond can rise above what is drawn: done
            if (y <= hx - (maxH - cy) * F / z) break;
            z += far ? 0.6f + z * 0.045f : 0.12f + z * 0.038f;
        }
        ymin[x] = (uint8_t)clampi(y, 0, H);
    }
}

// ================================================================================== panorama and sky
static float hFar[W + 2], hNear[W + 2];
static float s_skyPow[80], s_skyTT[80];
static float s_sunX, s_sunY, s_sunVis, s_lowSun;

// The ranges live in a ring around the whole horizon (1024 steps), refreshed a slice a frame and looked up by
// heading: turning costs nothing, and the land only morphs slowly with distance.
static constexpr int PAN_N = 1024;
static float* s_panFar = nullptr;
static float* s_panNear = nullptr;
static int s_panCur = 0;
static bool s_panFull = true;
static float s_panPeaks = -1.f; static bool s_panMesa = false; static float s_panMorph = 0.f;
static void panoramaRefresh(int count) {
    const Scene& sc = g_scene;
    const Biome& A = biome(sc.biomeA); const Biome& B = biome(sc.biomeB);
    float peaks = A.peaks + (B.peaks - A.peaks) * sc.blend;
    bool mesa = (sc.blend < 0.5f ? sc.biomeA : sc.biomeB) == B_CANYON;
    float morph = sc.distance * 0.00015f;
    for (int n = 0; n < count; n++) {
        int k = s_panCur; s_panCur = (s_panCur + 1) & (PAN_N - 1);
        float u = k * (12.f / PAN_N);
        float mass = fbmP(u, morph, 12, 3, worldSeed() + 0x51);                   // broad massifs
        float r = 1.f - fabsf(2.f * fbmP(u * 3.f, morph + 3.f, 36, 2, worldSeed() + 0x52) - 1.f);   // ridged detail
        float hf = (mass - 0.38f) * 1.6f + r * r * 0.35f * mass;
        hf = fmaxf(0.f, hf) * peaks * 62.f;
        if (mesa) hf = floorf(hf / 9.f) * 9.f + 4.f * clampf(fmodf(hf, 9.f) - 6.f, 0.f, 1.f);
        float f2 = fbmP(u * 2.f, morph * 1.6f, 24, 3, worldSeed() + 0x53);
        s_panFar[k] = hf;
        s_panNear[k] = (f2 - 0.42f) * (10.f + peaks * 26.f);
    }
}
static void computePanorama() {
    if (!s_panFar) { s_panFar = (float*)memSlow(PAN_N * sizeof(float)); s_panNear = (float*)memSlow(PAN_N * sizeof(float)); s_panFull = true; }   // read ~160x a frame: PSRAM is fine
    {   const Scene& sc = g_scene;
        const Biome& A = biome(sc.biomeA); const Biome& B = biome(sc.biomeB);
        float peaks = A.peaks + (B.peaks - A.peaks) * sc.blend;
        bool mesa = (sc.blend < 0.5f ? sc.biomeA : sc.biomeB) == B_CANYON;
        float morph = sc.distance * 0.00015f;
        if (fabsf(peaks - s_panPeaks) > 0.01f || mesa != s_panMesa || fabsf(morph - s_panMorph) > 0.02f) s_panFull = true;
        s_panPeaks = peaks; s_panMesa = mesa; s_panMorph = morph;
    }
    panoramaRefresh(s_panFull ? PAN_N : PAN_N / 8);
    s_panFull = false;
    float lift = (cy - journeyBase(cx, cz)) * F / 1800.f;
    const float toIdx = PAN_N / 6.2831853f;
    for (int x = -1; x <= W; x++) {
        int xi = clampi(x, 0, W - 1);
        float a = azC[xi] + (x - xi) / F;
        float f = a * toIdx;
        f -= PAN_N * floorf(f / PAN_N);
        int i0 = (int)f; float t = f - i0;
        int i1 = (i0 + 1) & (PAN_N - 1); i0 &= PAN_N - 1;
        float hf = s_panFar[i0] + (s_panFar[i1] - s_panFar[i0]) * t;
        float hn = s_panNear[i0] + (s_panNear[i1] - s_panNear[i0]) * t;
        hFar[x + 1] = fmaxf(0.f, hf * F / 96.f - lift);
        hNear[x + 1] = fmaxf(0.f, hn * F / 96.f - lift * 2.f);
    }
}

MR_HOT static void drawRidge(const float* hh, int far, float snowLine) {
    for (int x = 0; x < W; x++) {
        float h = hh[x + 1];
        int baseY = (int)horX[x] + (far ? 1 : 3);
        if (h <= 0.f) continue;
        float top = horX[x] - h;
        float slope = (hh[x + 2] - hh[x]) * 0.5f;
        float face = -slope * sideC[x];
        float lit = 0.5f + 0.3f * face / (1.f + fabsf(face));
        float u = angU(azC[x], 240);
        int y0 = clampi((int)ceilf(top), 0, H);
        int y1 = baseY < ymin[x] ? baseY : ymin[x];
        float snowDepth = h > snowLine ? (h - snowLine) * 0.55f : -1.f;
        for (int y = y0; y < y1; y++) {
            float d = y - top, depth = d / h;
            float g = tex((int)(u * 0.5f + d * 0.25f * (sideC[x] >= 0.f ? 1.f : -1.f)) + (far ? 17 : 0), (int)(d * 0.4f)) * (1.f / 255.f);
            float s = lit + (g - 0.5f) * 0.38f * (0.4f + 0.6f * (1.f - depth)) - depth * 0.3f;
            if (snowDepth > 0.f && d < snowDepth + g * 3.f) s = 0.95f;
            int lv = clampi((int)(s * 7.99f), 0, 7);
            if (d < 1.f && face > 0.15f) lv = clampi(lv + 2, 0, 7);
            fb[y * W + x] = (uint8_t)(P_MTN + (far ? 0 : 8) + lv);
            zb[y * W + x] = 254;
        }
    }
}

MR_HOT static void drawSkyAndPanorama() {
    const Scene& sc = g_scene;
    computePanorama();
    // fill between the far terrain and the horizon with distant hills
    for (int x = 0; x < W; x++) for (int y = (int)horX[x] + 1; y < ymin[x]; y++) { if (y < 0) continue; fb[y * W + x] = (uint8_t)(P_MTN + 3); zb[y * W + x] = 254; }
    float snowF = 26.f * F / 96.f;
    drawRidge(hNear, 0, 999.f);
    // the far range sits behind the near one: only where the sky still shows
    static uint8_t top[W];
    for (int x = 0; x < W; x++) {
        float hn = hNear[x + 1];
        int t = hn > 0.f ? (int)ceilf(horX[x] - hn) : (int)horX[x] + 3;
        if (t > ymin[x]) t = ymin[x];
        top[x] = (uint8_t)clampi(t, 0, H);
    }
    {
        for (int x = 0; x < W; x++) {
            float h = hFar[x + 1];
            if (h <= 0.f) continue;
            float tp = horX[x] - h;
            float slope = (hFar[x + 2] - hFar[x]) * 0.5f;
            float face = -slope * sideC[x];
            float lit = 0.5f + 0.3f * face / (1.f + fabsf(face));
            for (int y = clampi((int)ceilf(tp), 0, H); y < top[x]; y++) {
                float d = y - tp;
                float s = lit - d / h * 0.25f;
                if (h > snowF && d < (h - snowF) * 0.5f + 1.f) s = 0.92f;
                fb[y * W + x] = (uint8_t)(P_MTN + clampi((int)(s * 7.99f), 0, 7));
                zb[y * W + x] = 254;
            }
            int t2 = (int)ceilf(tp);
            if (t2 < top[x]) top[x] = (uint8_t)clampi(t2, 0, H);
        }
    }
    // sky (Planet's gradient + horizon glow + sun halo)
    float sunRel = wrapPi(sc.sunAz - cyaw);
    s_sunVis = (sc.sunElev > -0.12f ? 1.f : 0.f) * clampf(1.f - sc.cloud * 0.85f, 0.f, 1.f);
    s_lowSun = clampf(1.f - sc.sunElev / 0.5f, 0.f, 1.f) * (sc.sunElev > -0.15f ? 1.f : 0.f);
    s_sunX = W * 0.5f + tanf(clampf(sunRel, -1.4f, 1.4f)) * F;
    s_sunY = hor0 - tanf(sc.sunElev) * F + (s_sunX - W * 0.5f) * rollT;
    if (fabsf(sunRel) > 1.4f) s_sunX = sunRel > 0 ? 9999.f : -9999.f;
    for (int x = 0; x < W; x++) {
        float rel = wrapPi(sc.sunAz - azC[x]);
        float hglow = s_sunVis * s_lowSun / (1.f + rel * rel * 3.f);
        float skyH = horX[x] + 2.f;
        for (int y = 0; y < top[x]; y++) {
            int k = clampi((int)(y + 0.5f - (skyH - 78.f)), 0, 78);
            float base = s_skyPow[k], tt = s_skyTT[k];
            float dx = x - s_sunX, dy = y - s_sunY;
            float halo = s_sunVis * 5.5f / (1.f + (dx * dx + dy * dy) * (1.f / 260.f));
            fb[y * W + x] = (uint8_t)(P_SKY + clampi(band(base + hglow * tt * 6.5f + halo, x, y), 0, 15));
            zb[y * W + x] = 255;
        }
    }
    // rainbow: a 42 degree circle round the point opposite the sun
    if (sc.rainbow > 0.05f && sc.night < 0.5f) {
        float antiAz = sc.sunAz + 3.14159f, antiEl = -sc.sunElev;
        for (int x = 0; x < W; x++) {
            float da = wrapPi(azC[x] - antiAz);
            for (int y = 0; y < ymin[x] && y < H; y++) {
                if (zb[y * W + x] < 254) continue;
                float el = atanf((horX[x] - y) / F);
                float r = sqrtf(da * da + (el - antiEl) * (el - antiEl));
                if (r < 0.69f || r > 0.75f) continue;
                if (sc.rainbow * 0.75f < dith(x, y)) continue;
                fb[y * W + x] = (uint8_t)(P_SPECIAL + clampi((int)((r - 0.69f) / 0.06f * 7.f), 0, 6));
            }
        }
    }
}

static constexpr int CL_N = 512, CL_R = 26;
static uint8_t* s_cl = nullptr;                // cloud density around the horizon [angle][row]
static int s_clCur = 0;
static bool s_clFull = true;
static void cloudRefresh(int count) {
    const Scene& sc = g_scene;
    for (int n = 0; n < count; n++) {
        int k = s_clCur; s_clCur = (s_clCur + 1) % CL_N;
        float u = k * (36.f / CL_N);
        for (int r = 0; r < CL_R; r++) {
            float v = r * 0.15f + sc.distance * 0.001f + s_t * 0.004f;
            float nn = (tex((int)(u * 3.556f), (int)(v * 8.f)) * 0.62f + tex((int)(u * 7.11f) + 21, (int)(v * 16.f) + 9) * 0.38f) * (1.f / 255.f);
            nn = (nn - 0.5f) * 1.5f + 0.5f;
            s_cl[k * CL_R + r] = (uint8_t)clampi((int)(nn * 255.f), 0, 255);
        }
    }
}
MR_HOT static void drawClouds() {                                 // Planet's banded pixel clouds
    const Scene& sc = g_scene;
    if (sc.cloud < 0.04f) return;
    const int CR = 26;
    float thresh = 0.92f - 0.62f * sc.cloud + 0.12f * s_night * (1.f - sc.cloud);
    float drift = s_t * 0.006f * (1.f + 2.f * sc.wind);
    float centre = CR * (0.42f + 0.2f * sc.storm);
    if (!s_cl) { s_cl = (uint8_t*)memSlow(CL_N * CL_R); s_clFull = true; }   // read ~2,000x a frame: PSRAM is fine
    if (!s_cl) return;
    cloudRefresh(s_clFull ? CL_N : CL_N / 8);
    s_clFull = false;
    const float toIdx = CL_N / 6.2831853f;
    for (int x = 0; x < W; x += 2) {
        float a = azC[x] + drift;
        float fi = a * toIdx; fi -= CL_N * floorf(fi / CL_N);
        const uint8_t* col = s_cl + ((int)fi % CL_N) * CL_R;
        float rel = wrapPi(sc.sunAz - a);
        float prox = s_sunVis / (1.f + rel * rel * 6.f);
        float prevD = -1.f;
        float n = 0.f;
        for (int y = 0; y < CR * 2; y++) {
            int yy = y + (int)(horX[x] - 70.f);
            if (yy < 0 || yy >= H) continue;
            float cyf = y * 0.5f;
            n = col[y >> 1] * (1.f / 255.f);
            float prof = clampf(1.f - fabsf(cyf - centre) / (CR * 0.6f), 0.f, 1.f);
            float d = n * 0.8f + prof * 0.35f - thresh;
            float dn = d;
            if (d <= 0.f) { prevD = d; continue; }
            float alpha = clampf(d * 11.f, 0.f, 1.f);
            float top = clampf((d - prevD) * 6.f, 0.f, 1.f);
            prevD = dn;
            for (int k = 0; k < 2; k++) {
                int xx = x + k;
                if (xx >= W) break;
                int i = yy * W + xx;
                if (zb[i] < 255) continue;
                if (alpha < 0.45f || (alpha < 0.7f && ((xx ^ yy) & 1))) continue;
                float light = 0.3f + top * 0.45f * (1.f - s_lowSun) + prox * 0.35f + (1.f - alpha) * 0.2f + s_lowSun * 0.25f * (1.f - cyf / CR);
                light *= 1.f - 0.4f * sc.rain - 0.3f * sc.storm;
                int lvl = clampi(band(light * 4.f, xx, yy), 0, 4) * 3 + 2;
                fb[i] = (uint8_t)(P_CLOUD + lvl);
            }
        }
    }
}

static constexpr int STAR_N = 1000;
static void drawNight() {
    const Scene& sc = g_scene;
    float n = s_night;
    if (n < 0.08f) return;
    uint32_t tw = (uint32_t)(s_t * 5.f);
    static float starAz[STAR_N], starEl[STAR_N]; static uint8_t starB[STAR_N]; static bool made = false;
    if (!made) {
        for (int k = 0; k < STAR_N; k++) {
            uint32_t h = hash32((uint32_t)k * 2654435761u ^ 0x57A2u);
            starAz[k] = (h & 0xFFFF) * (6.2831853f / 65536.f);
            float e = ((h >> 16) & 0x3FF) / 1023.f;
            starEl[k] = atanf(0.02f + e * 0.8f);              // evenly over the rows of sky, like the old per-pixel scan
            starB[k] = (uint8_t)(3 + (hash32(h) % 5));
        }
        made = true;
    }
    float dens = n * (1.f - sc.cloud * 0.8f) * (sc.biomeA == B_TUNDRA ? 1.f : 0.4f);
    int count = (int)(STAR_N * clampf(dens, 0.f, 1.f));
    for (int k = 0; k < count; k++) {
        float rel = wrapPi(starAz[k] - cyaw);
        if (fabsf(rel) > 0.8f) continue;
        float sx = W * 0.5f + tanf(rel) * F;
        int x = (int)sx;
        if ((unsigned)x >= (unsigned)W) continue;
        int y = (int)(horX[x] - tanf(starEl[k]) * F);
        if ((unsigned)y >= (unsigned)H) continue;
        int i = y * W + x;
        if (zb[i] != 255 || fb[i] >= P_CLOUD) continue;
        int b = starB[k];
        if ((hash32((uint32_t)k ^ tw) & 7u) == 0) b -= 3;
        fb[i] = (uint8_t)(P_SUN + clampi(b, 0, 7));
    }
    // moon
    float mrel = wrapPi(sc.sunAz + 3.14159f - cyaw);
    if (n > 0.3f && fabsf(mrel) < 1.2f) {
        float mx = W * 0.5f + tanf(mrel) * F, my = horX[0] - 40.f;
        for (int dy = -5; dy <= 5; dy++) for (int dx = -5; dx <= 5; dx++) {
            if (dx * dx + dy * dy > 22) continue;
            int x = (int)mx + dx, y = (int)my + dy;
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || zb[y * W + x] != 255) continue;
            int b = (dx * 0.8f + dy * 0.4f > 2.6f) ? 5 : 7;
            if ((dx + 1) * (dx + 1) + (dy - 1) * (dy - 1) < 3) b = 5;
            fb[y * W + x] = (uint8_t)(P_SUN + b);
        }
    }
    // aurora: curtains folding over the northern sky
    if (sc.aurora > 0.05f) {
        for (int x = 0; x < W; x++) {
            float u = angU(azC[x], 96);
            float curtain = fbmP(u * 0.5f, s_t * 0.03f, 48, 3, 0xA0A0u);
            curtain = clampf((curtain - 0.42f) * 3.f, 0.f, 1.f) * sc.aurora;
            if (curtain <= 0.f) continue;
            float base = horX[x] - 18.f - 10.f * vnoiseP(u * 0.3f, s_t * 0.02f, 29, 0xA1u);
            float streak = vnoiseP(u * 4.f, s_t * 0.2f, 384, 0xA2u);
            for (int y = (int)(base - 40.f); y < (int)base; y++) {
                if (y < 0 || y >= H) continue;
                int i = y * W + x;
                if (zb[i] != 255) continue;
                float v = (y - (base - 40.f)) / 40.f;
                float k = curtain * v * v * (0.5f + streak);
                if (k < dith(x, y) * 0.9f) continue;
                fb[i] = (uint8_t)(P_SPECIAL + clampi((int)(k * 7.f + (v < 0.5f ? 4.f : 0.f)), 1, 7));
            }
        }
    }
}

static void drawSun() {
    if (s_sunVis <= 0.f || s_sunX < -12 || s_sunX > W + 12 || s_sunY < -12) return;
    int sx = (int)lroundf(s_sunX), sy = (int)lroundf(s_sunY);
    for (int dy = -8; dy <= 8; dy++) for (int dx = -8; dx <= 8; dx++) {
        float d = sqrtf((float)(dx * dx + dy * dy));
        int x = sx + dx, y = sy + dy;
        if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
        int i = y * W + x;
        if (zb[i] != 255) continue;
        if (d < 4.2f) fb[i] = P_SUN + 7;
        else if (d < 7.5f && (1.f - (d - 4.2f) / 3.3f) > dith(x, y)) fb[i] = P_SUN + 5;
    }
}

// ================================================================================== props
static inline void putZ(int x, int y, uint8_t c, uint8_t zc) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    int i = y * W + x;
    if (zb[i] <= zc) return;
    fb[i] = c; zb[i] = zc;
}

// a lit template, scaled. base: screen row of its foot at column sx.
MR_HOT static void blitTpl(uint8_t id, float sx, float baseY, float pxH, float depth, bool mirror, bool alt, float headK) {
    const Tpl& t = tpl(id);
    if (!t.px || pxH < 1.5f) return;
    float sc = pxH / t.h;
    float pxW = t.w * sc;
    int x0 = (int)floorf(sx - t.ax * sc), x1 = (int)ceilf(x0 + pxW);
    if (x1 < 0 || x0 >= W) return;
    uint8_t zc = zcode(depth);
    float fog = s_fog[clampi((int)depth, 0, 519)];
    float inv = 1.f / sc;
    int lightAdd = (int)(headK * 6.f) - (s_night > 0.5f ? 3 : 0);
    for (int x = x0 > 0 ? x0 : 0; x < x1 && x < W; x++) {
        int tx = (int)((x - x0) * inv);
        if (tx >= t.w) tx = t.w - 1;
        if (mirror) tx = t.w - 1 - tx;
        float by = baseY + (x - sx) * rollT;
        int y0 = (int)floorf(by - pxH);
        for (int y = y0 > 0 ? y0 : 0; y < (int)by && y < H; y++) {
            int ty = (int)((y - y0) * inv);
            if (ty >= t.h) ty = t.h - 1;
            uint8_t v = t.px[ty * t.w + tx];
            if (!v) continue;
            int i = y * W + x;
            if (zb[i] <= zc) continue;
            int m = v >> 4, sh = (v & 15) + lightAdd;
            uint8_t c;
            switch (m) {
                case TM_LEAF:  c = alt ? (uint8_t)(P_LEAF2 + clampi(sh / 2, 0, 7)) : (uint8_t)(P_LEAF + clampi(sh, 0, 15)); break;
                case TM_LEAF2: c = (uint8_t)(P_LEAF2 + clampi(sh / 2, 0, 7)); break;
                case TM_BARK:  c = (uint8_t)(P_BARK + clampi(sh / 2, 0, 7)); break;
                case TM_ROCK:  c = (uint8_t)(P_ROCK + clampi(sh, 0, 15)); break;
                case TM_SNOW:  c = (uint8_t)(P_SNOW + clampi(sh / 2, 0, 7)); break;
                case TM_FLOWER: c = (uint8_t)(P_FLOWER + (v & 7)); break;
                case TM_WALL:  c = (uint8_t)(P_ROCK + clampi(sh + 2, 0, 15)); break;
                case TM_ROOF:  c = (uint8_t)(P_ROCK + clampi(sh - 4, 0, 15)); break;
                case TM_GLASS: c = P_WINDOW; break;
                case TM_SIGN:  c = (v & 15) > 12 ? P_SIGN_W : P_SIGN_G; break;
                case TM_SIGNTXT: c = P_SIGN_W; break;
                default:       c = (uint8_t)(P_SNOW + 7); break;
            }
            if (fog > 0.72f || (fog > 0.42f && ((x ^ y) & 1))) c = (uint8_t)(P_HAZE + clampi(8 + (sh - 8) / 3, 0, 15));
            fb[i] = c; zb[i] = zc;
        }
    }
}

static float headAt(float lat, float depth) {               // headlight on a prop
    if (s_lights < 0.02f || depth > 70.f) return 0.f;
    float k = lat / depth;
    float c = 1.f - k * k / 0.17f;
    return c > 0.f ? s_lights * c * fminf(1.f, 14.f / (depth + 4.f)) * s_night : 0.f;
}

static void drawVegetation() {
    const Grid& g = terrainNear();
    if (!g.c) return;
    bool snowy = g_scene.biomeA == B_TUNDRA || g_scene.biomeA == B_ALPINE || g_scene.snow > 0.3f;
    bool autumn = (g_scene.blend < 0.5f ? g_scene.biomeA : g_scene.biomeB) == B_AUTUMN;
    float sunRel = g_scene.sunAz;
    float R = 125.f;
    float lx = cx + (fx - rx) * R, lz = cz + (fz - rz) * R, qx = cx + (fx + rx) * R, qz = cz + (fz + rz) * R;
    int i0 = (int)floorf(fminf(cx, fminf(lx, qx)) * g.inv) - 1, i1 = (int)ceilf(fmaxf(cx, fmaxf(lx, qx)) * g.inv) + 1;
    int j0 = (int)floorf(fminf(cz, fminf(lz, qz)) * g.inv) - 1, j1 = (int)ceilf(fmaxf(cz, fmaxf(lz, qz)) * g.inv) + 1;
    if (i0 < g.ox + 1) i0 = g.ox + 1;
    if (j0 < g.oz + 1) j0 = g.oz + 1;
    if (i1 > g.ox + g.N - 1) i1 = g.ox + g.N - 1;
    if (j1 > g.oz + g.N - 1) j1 = g.oz + g.N - 1;
    for (int j = j0; j < j1; j++) {
        for (int i = i0; i < i1; i++) {
            const Cell& c = g.c[gridWrap(j, g.N) * g.N + gridWrap(i, g.N)];
            uint8_t t = c.extra & 15;
            uint8_t m = c.mat & 15;
            if (!t || t >= VEG_N || m == MAT_ROAD || m == MAT_SHOULDER || m == MAT_WATER || m == MAT_LOT) continue;
            float wx0 = (i + 0.5f) * g.cell - cx, wz0 = (j + 0.5f) * g.cell - cz;
            float d0 = wx0 * fx + wz0 * fz;
            if (d0 < 0.5f || d0 > 125.f) continue;
            float l0 = wx0 * rx + wz0 * rz;
            if (fabsf(l0) > d0 * 0.95f + 6.f) continue;
            Veg v;
            if (!vegAt(i, j, v)) continue;
            float sx, sy, dep;
            if (!renderProject(v.x, v.y, v.z, sx, sy, dep)) continue;
            float pxH = v.size * F / dep;
            if (pxH < 2.5f) continue;
            {   int bx = (int)sx, by = (int)sy - 1, ty = (int)(sy - pxH * 0.7f);   // hidden behind nearer land?
                uint8_t zc = zcode(dep);
                if ((unsigned)bx < (unsigned)W && (unsigned)ty < (unsigned)H && (unsigned)by < (unsigned)H && zb[by * W + bx] < zc && zb[ty * W + bx] < zc) continue; }
            uint8_t id;
            switch (v.type) {
                case VEG_PINE: id = snowy ? T_PINESNOW : (uint8_t)(T_PINE0 + v.var % 3); break;
                case VEG_BROAD: id = (uint8_t)(T_BROAD0 + (v.var & 1)); break;
                case VEG_REDWOOD: id = T_REDWOOD; break;
                case VEG_BUSH: id = T_BUSH; break;
                case VEG_CACTUS: id = T_CACTUS; break;
                case VEG_SNAG: id = T_SNAG; break;
                case VEG_BOULDER: id = T_BOULDER; pxH *= 0.65f; break;
                default: id = T_FLOWERS; if (dep > 30.f) continue; break;
            }
            float az = cyaw + atanf((sx - W * 0.5f) / F);
            bool mirror = sinf(wrapPi(sunRel - az)) < 0.f;
            blitTpl(id, sx, sy + 1.f, pxH, dep, mirror, autumn && (v.var & 2), headAt(l0, d0));
        }
    }
}

static void drawSignText(int x0, int y0, int w, int h, uint8_t zc) {
    const Junction* j = roadJunction();
    if (!j || !j->active || w < 30) return;
    char l[16], r[16];
    bool armLeft = j->armSide < 0;
    snprintf(l, sizeof l, "<%s", biome(armLeft ? j->destArm : j->destMain).sign);
    snprintf(r, sizeof r, "%s>", biome(armLeft ? j->destMain : j->destArm).sign);
    int sc = w > 70 ? 2 : 1;
    font3(fb, W, H, x0 + 2, y0 + 2, l, P_SIGN_W, 0, sc);
    if (h > 14 * sc) font3(fb, W, H, x0 + w - font3Width(r, sc) - 2, y0 + 2 + 7 * sc, r, P_SIGN_W, 0, sc);
    (void)zc;
}

static void drawSignAndStore() {
    const RoadSign* s = roadSign();
    if (s && s->active) {
        float h = terrainHeight(s->x, s->z);
        float sx, sy, d;
        if (renderProject(s->x, h, s->z, sx, sy, d) && d < 160.f) {
            float pw = 4.4f * F / d, ph = 2.0f * F / d, post = 2.2f * F / d;
            uint8_t zc = zcode(d);
            int x0 = (int)(sx - pw * 0.5f), y0 = (int)(sy - post - ph);
            float hk = headAt(0.f, d);
            uint8_t g = hk > 0.3f ? P_SIGN_W : P_SIGN_G;
            for (int y = y0; y < (int)(sy - post); y++) for (int x = x0; x < (int)(x0 + pw); x++) {
                bool edge = y == y0 || y == (int)(sy - post) - 1 || x == x0 || x == (int)(x0 + pw) - 1;
                putZ(x, y, edge ? P_SIGN_W : (hk > 0.6f ? P_SIGN_G : g == P_SIGN_W ? P_SIGN_G : P_SIGN_G), zc);
            }
            for (int y = (int)(sy - post); y < (int)sy; y++) { putZ((int)(x0 + pw * 0.2f), y, P_BLACK, zc); putZ((int)(x0 + pw * 0.8f), y, P_BLACK, zc); }
            drawSignText(x0, y0, (int)pw, (int)ph, zc);
        }
    }
    const Store* st = roadStore();
    if (st && st->active) {
        float h = terrainHeight(st->x, st->z);
        float sx, sy, d;
        if (renderProject(st->x, h, st->z, sx, sy, d) && d < 300.f) blitTpl(T_STORE, sx, sy + 1.f, 10.f * F / d, d, false, false, 0.f);
    }
}

static void drawCritters() {
    const Critter* c = fieldCritters();
    for (int k = 0; k < CRIT_N; k++) {
        if (!c[k].on) continue;
        float sx, sy, d;
        if (!renderProject(c[k].x, c[k].y, c[k].z, sx, sy, d) || d > 160.f) continue;
        float dirScreen = sinf(c[k].yaw - cyaw);
        blitTpl((uint8_t)(T_DEER0 + (c[k].pose & 3)), sx, sy + 1.f, 1.5f * F / d, d, dirScreen < 0.f, false, headAt((c[k].x - cx) * rx + (c[k].z - cz) * rz, d));
    }
}

static void drawParticles() {
    const Particle* p = fieldParticles();
    for (int k = 0; k < PART_N; k++) {
        if (p[k].life <= 0.f) continue;
        float sx, sy, d;
        if (!renderProject(p[k].x, p[k].y, p[k].z, sx, sy, d) || d < 0.8f) continue;
        float r = p[k].size * F / d;
        float fade = 1.f - p[k].age / p[k].life;
        uint8_t zc = zcode(d);
        uint8_t c;
        switch (p[k].kind) {
            case PK_MUD: c = P_MUD; break;
            case PK_SPRAY: c = P_HAZE + 12; break;
            case PK_SNOW: c = P_SNOW + 6; break;
            case PK_SMOKE: c = P_HAZE + 13; break;
            default: c = (uint8_t)(P_SOIL + 10); break;
        }
        bool puff = p[k].kind == PK_DUST || p[k].kind == PK_SMOKE;
        // puffs thin out as they grow and age, and near the lens they are mostly air
        float dens = puff ? fade * 0.45f * clampf(d / 6.f, 0.15f, 1.f) : 1.f;
        int R = (int)(r + 0.5f);
        int cap = puff ? 7 : 2;
        if (R > cap) R = cap;
        if (!puff && d < 2.5f) continue;
        for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
            if (dx * dx + dy * dy > R * R + 1) continue;
            int x = (int)sx + dx, y = (int)sy + dy;
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
            if (dens < 1.f && dith(x + k, y) > dens * (1.f - (dx * dx + dy * dy) / (float)(R * R + 1) * 0.6f)) continue;
            putZ(x, y, c, zc);
        }
    }
}

// ================================================================================== reflections, weather
MR_HOT static void reflectPass() {
    const float t = s_t;
    for (int x = 0; x < W; x++) {
        for (int y = (int)horX[x] + 1; y < H; y++) {
            if (y < 0) continue;
            int i = y * W + x;
            if (!(zb[i] & 1)) continue;
            uint8_t cur = fb[i];
            bool water = cur >= P_WATER && cur < P_WATER + 8;
            int ym = (int)(2.f * horX[x] - y + 1.f);
            int xs = x + (((y + (int)(t * 6.f)) & 7) < 2 ? 1 : 0);
            if (xs >= W) xs = W - 1;
            uint8_t src = ym >= 0 ? fb[ym * W + xs] : (uint8_t)P_SKY;
            uint8_t r = s_refl[src];
            float fres = clampf(1.f - (y - horX[x]) / 55.f, 0.25f, 1.f);
            if (water) fb[i] = (fres > dith(x, y)) ? r : cur;
            else if (s_wet * 0.55f * fres > dith(x, y)) fb[i] = r;
        }
    }
}

static void drawRain() {
    const Scene& sc = g_scene;
    float amt = sc.rain;
    if (amt < 0.05f) return;
    int n = (int)(amt * 70.f);
    float slant = clampf(-sc.speed * 0.012f + sinf(sc.windAz - cyaw) * sc.wind * 0.6f, -1.2f, 1.2f);
    uint32_t seed = (uint32_t)(s_t * 30.f) * 2654435761u;
    for (int k = 0; k < n; k++) {
        uint32_t h = hash32(seed + k * 7919u);
        int x = h % W, y = (h >> 9) % H;
        int len = 3 + ((h >> 17) & 3);
        for (int j = 0; j < len; j++) {
            int xx = x + (int)(j * slant * 0.5f), yy = y + j;
            if ((unsigned)xx < (unsigned)W && (unsigned)yy < (unsigned)H) fb[yy * W + xx] = s_bright[fb[yy * W + xx]] == fb[yy * W + xx] ? (uint8_t)(P_HAZE + 13) : s_bright[s_bright[fb[yy * W + xx]]];
        }
    }
}

struct Flake { float x, y, z; };
static Flake s_flk[60];
static void drawSnow() {
    const Scene& sc = g_scene;
    if (sc.snow < 0.05f) return;
    int n = (int)(sc.snow * 60.f);
    float dt = sc.dt;
    for (int k = 0; k < n; k++) {
        Flake& f = s_flk[k];
        f.z -= sc.speed * dt;
        f.y -= 1.2f * dt;
        f.x += sinf(s_t * 0.7f + k) * 0.4f * dt + sc.wind * dt;
        if (f.z < 0.6f || f.y < -2.f) {
            uint32_t h = hash32((uint32_t)k * 31u + (uint32_t)(s_t * 10.f));
            f.z = 2.f + (h & 255) / 255.f * 18.f; f.x = ((h >> 8 & 255) / 255.f - 0.5f) * f.z * 1.6f; f.y = ((h >> 16 & 255) / 255.f) * 6.f - 1.f;
        }
        int x = (int)(W * 0.5f + f.x / f.z * F), y = (int)(hor0 - f.y / f.z * F);
        if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) {
            fb[y * W + x] = P_SNOW + 7;
            if (f.z < 4.f && x + 1 < W) fb[y * W + x + 1] = P_SNOW + 6;
        }
    }
}

static void drawFireflies() {
    const Scene& sc = g_scene;
    if (sc.fireflies < 0.05f) return;
    int n = (int)(sc.fireflies * 26.f);
    for (int k = 0; k < n; k++) {
        uint32_t h = hash32((uint32_t)k * 7u + worldSeed());
        float ph = s_t * (0.2f + (h & 15) * 0.02f) + k;
        float wx = cx + ((h & 255) / 255.f - 0.5f) * 50.f + sinf(ph) * 2.f;
        float wz = cz + ((h >> 8 & 255) / 255.f - 0.5f) * 50.f + cosf(ph * 0.7f) * 2.f;
        float blink = sinf(s_t * (1.f + (h >> 16 & 7) * 0.3f) + k * 2.f);
        if (blink < 0.3f) continue;
        float sx, sy, d;
        float gy = terrainHeight(wx, wz) + 0.6f + sinf(ph * 1.3f) * 0.4f;
        if (!renderProject(wx, gy, wz, sx, sy, d) || d > 40.f) continue;
        putZ((int)sx, (int)sy, P_FIREFLY, zcode(d));
        if (d < 10.f) { putZ((int)sx + 1, (int)sy, P_FIREFLY, zcode(d)); }
    }
}

static void drawLeaves() {
    const Scene& sc = g_scene;
    if (sc.leaves < 0.05f) return;
    int n = (int)(sc.leaves * 14.f);
    for (int k = 0; k < n; k++) {
        uint32_t h = hash32((uint32_t)k * 13u + 99u);
        float sp = 18.f + (h & 31);
        float x = fmodf((h >> 5 & 255) * 0.7f + s_t * (sc.wind * 30.f + 6.f) + sinf(s_t * 2.f + k) * 6.f, (float)W + 20.f) - 10.f;
        float y = fmodf((h >> 13 & 255) * 0.5f + s_t * sp, (float)H + 10.f) - 5.f;
        uint8_t c = (h & 1) ? (uint8_t)(P_LEAF2 + 5) : (uint8_t)(P_LEAF + 12);
        int xi = (int)x, yi = (int)y;
        if ((unsigned)xi < (unsigned)W && (unsigned)yi < (unsigned)H) { fb[yi * W + xi] = c; if (((int)(s_t * 6.f + k) & 1) && xi + 1 < W) fb[yi * W + xi + 1] = c; }
    }
}

static void drawBolt() {
    const Scene& sc = g_scene;
    if (sc.boltAge > 0.3f) return;
    float rel = wrapPi(sc.boltAz - cyaw);
    if (fabsf(rel) > 0.9f) return;
    float x = W * 0.5f + tanf(rel) * F;
    uint32_t h = sc.boltSeed;
    int y0 = 0, y1 = (int)horX[clampi((int)x, 0, W - 1)];
    for (int y = y0; y < y1; y++) {
        h = hash32(h + (uint32_t)y);
        if ((y & 3) == 0) x += ((h & 255) / 255.f - 0.5f) * 5.f;
        int xi = (int)x;
        if ((unsigned)xi >= (unsigned)W) continue;
        int i = y * W + xi;
        if (zb[i] < 254) continue;
        fb[i] = P_BOLT;
        if (xi + 1 < W && zb[i + 1] >= 254) fb[i + 1] = P_SUN + 6;
    }
}

static void drawMist() {
    float mist = S.mist * 0.4f + g_scene.mist + g_scene.fog * 0.35f;
    if (mist < 0.04f) return;
    for (int x = 0; x < W; x++) {
        int hy = (int)horX[x];
        for (int y = hy - 10; y < hy + 18; y++) {
            if (y < 0 || y >= H) continue;
            float k = mist * (1.f - fabsf((float)(y - hy - 3)) / 15.f) * (0.7f + 0.3f * tex((int)((x + cyaw * F) * 0.1f), (int)(y * 0.4f + s_t * 0.3f)) * (1.f / 255.f));
            if (k > 0.55f || (k > 0.35f && ((x ^ y) & 1))) { int i = y * W + x; if (zb[i] > 60) fb[i] = (uint8_t)(P_HAZE + clampi(10 + (int)(k * 4.f), 0, 15)); }
        }
    }
}

static uint8_t* s_shaft = nullptr;            // 80x60 brighten chance, refreshed every other frame
static float s_shaftSX = 0.f, s_shaftSY = 0.f;   // where the sun was when it was made
static int s_frameN = 0;
static void drawShafts() {
    const Scene& sc = g_scene;
    if (sc.beams < 0.12f || s_night > 0.6f || s_sunX < -400.f || s_sunX > W + 400.f) return;
    if (!s_shaft) { s_shaft = (uint8_t*)memFast(80 * 60); if (!s_shaft) return; memset(s_shaft, 0, 80 * 60); }
    if (!(s_frameN & 1)) {
        s_shaftSX = s_sunX; s_shaftSY = s_sunY;
        float str = sc.beams * 0.45f;
        for (int y = 0; y < H; y += 2) for (int x = 0; x < W; x += 2) {
            float dx = x - s_sunX, dy = y - s_sunY;
            uint8_t k8 = 0;
            if (dy >= -6.f) {
                float st = tex((int)(fastAtan2(dy, dx) * 20.f + 64.f), (int)(s_t * 0.3f)) * (1.f / 255.f);
                if (st >= 0.56f) {
                    float dist = sqrtf(dx * dx + dy * dy);
                    float k = (st - 0.56f) * 2.3f * str * clampf(1.f - dist / 260.f, 0.f, 1.f);
                    k8 = (uint8_t)clampi((int)(k * 255.f), 0, 255);
                }
            }
            s_shaft[(y >> 1) * 80 + (x >> 1)] = k8;
        }
    }
    int ox = (int)lroundf(s_sunX - s_shaftSX), oy = (int)lroundf(s_sunY - s_shaftSY);   // the shafts move with the sun
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int mx = (x - ox) >> 1, my = (y - oy) >> 1;
        if ((unsigned)mx >= 80u || (unsigned)my >= 60u) continue;
        uint8_t k8 = s_shaft[my * 80 + mx];
        if (k8 && k8 * (1.f / 255.f) > dith(x, y)) { int i = y * W + x; fb[i] = s_bright[fb[i]]; }
    }
}

static void drawBirds() {
    const Bird* b = fieldBirds();
    if (!b->on) return;
    for (int k = 0; k < b->n; k++) {
        float az = b->az + (k - b->n / 2) * 0.018f - abs(k - b->n / 2) * 0.0f;
        float el = b->el - abs(k - b->n / 2) * 0.012f;
        float rel = wrapPi(az - cyaw);
        if (fabsf(rel) > 1.f) continue;
        int x = (int)(W * 0.5f + tanf(rel) * F), y = (int)(hor0 - tanf(el) * F);
        bool flap = ((int)(s_t * 6.f + k) & 1);
        uint8_t c = P_CLOUD + 1;
        if ((unsigned)x < (unsigned)(W - 2) && (unsigned)y < (unsigned)(H - 1) && y > 0) {
            fb[y * W + x] = c;
            fb[(flap ? y - 1 : y) * W + x - 1 + (x > 0 ? 0 : 1)] = c; fb[(flap ? y - 1 : y) * W + x + 1] = c;
        }
    }
}

// ================================================================================== the vehicle, the cockpit
static void drawChaseVehicle() {
    const ViewInfo& v = g_view;
    const art::VehArt& a = art::VEH_ART[v.vehicle];
    float sx, sy, d;
    if (!renderProject(v.x, v.groundY, v.z, sx, sy, d)) return;
    float rel = wrapPi(v.yaw - cyaw);
    bool mirror = rel < 0.f;
    float ar = fabsf(rel) * 57.3f;
    int frame;
    if (v.vehicle == V_BIKE) {
        float lean = fabsf(v.lean) * 57.3f;
        frame = clampi((int)(lean / 10.f + 0.5f), 0, 4);
        mirror = v.lean < 0.f;
    } else {
        static const float YAWS[6] = {0, 7, 15, 25, 37, 52};
        int yi = 0; for (int k = 1; k < 6; k++) if (fabsf(ar - YAWS[k]) < fabsf(ar - YAWS[yi])) yi = k;
        int pi = v.pitch > 0.07f ? 1 : (v.pitch < -0.07f ? 2 : 0);
        frame = pi * 6 + yi;
    }
    const uint8_t* px = a.px + (size_t)frame * ((a.w * a.h + 1) / 2);
    int ox = (int)lroundf(sx) - a.ax, oy = (int)lroundf(sy + v.bounce * F / d) - a.ay;
    // its shadow
    float sw = (v.vehicle == V_BIKE ? 0.5f : 1.0f) * F / d * 1.1f;
    for (int y = -2; y <= 1; y++) for (int x = -(int)sw; x <= (int)sw; x++) {
        int xx = (int)sx + x, yy = (int)sy + y;
        if ((unsigned)xx < (unsigned)W && (unsigned)yy < (unsigned)H && ((xx + yy) & 1)) fb[yy * W + xx] = P_BLACK;
    }
    for (int y = 0; y < a.h; y++) for (int x = 0; x < a.w; x++) {
        int k = y * a.w + (mirror ? a.w - 1 - x : x);
        uint8_t p = (k & 1) ? (px[k >> 1] & 15) : (px[k >> 1] >> 4);
        if (!p) continue;
        int X = ox + x, Y = oy + y;
        if ((unsigned)X < (unsigned)W && (unsigned)Y < (unsigned)H) { fb[Y * W + X] = (uint8_t)(P_VEH + p); zb[Y * W + X] = 0; }
    }
    // tail lights burn at night and under braking
    if ((s_night > 0.4f || v.brake) && v.vehicle != V_BIKE) {
        int ty = oy + a.ay - (int)(0.85f * F / d);
        int tx = (int)(0.62f * F / d);
        uint8_t c = v.brake ? P_TAIL : P_TAIL_D;
        for (int s = -1; s <= 1; s += 2) for (int q = 0; q < 3; q++) {
            int X = (int)sx + s * tx + (q - 1) * s, Y = ty;
            if ((unsigned)X < (unsigned)W && (unsigned)Y < (unsigned)H) fb[Y * W + X] = c;
        }
    }
}

static float s_wheelAng = 0.f;
static uint16_t* s_ckRun = nullptr;            // (start, length) pairs of opaque, non-wheel cockpit pixels
static uint8_t* s_ckPix = nullptr;             // their palette indices, ready to copy
static int s_ckRuns = 0;
MR_HOT static void drawCockpit() {
    if (!s_cock) return;
    const art::CockArt& c = art::COCK_ART[g_view.vehicle];
    const uint8_t* src = s_ckPix;
    for (int r = 0; r < s_ckRuns; r++) {
        int st = s_ckRun[r * 2], len = s_ckRun[r * 2 + 1];
        memcpy(fb + st, src, len); src += len;
    }
    // the wheel turns with the steering
    if (c.wr > 0) {
        s_wheelAng += (g_view.steer * 1.6f - s_wheelAng) * clampf(g_scene.dt * 12.f, 0.f, 1.f);
        float ca = cosf(s_wheelAng), sa = sinf(s_wheelAng);
        int R = c.wr + 2;
        for (int y = c.wy - R; y <= c.wy + R; y++) for (int x = c.wx - R; x <= c.wx + R; x++) {
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
            float dx = x - c.wx, dy = y - c.wy;
            int sx = (int)lroundf(c.wx + dx * ca + dy * sa), sy = (int)lroundf(c.wy - dx * sa + dy * ca);
            if ((unsigned)sx >= (unsigned)W || (unsigned)sy >= (unsigned)H) continue;
            int si = sy * W + sx;
            if (s_wheelMask[si]) fb[y * W + x] = (uint8_t)(P_COCK + s_cock[si]);
        }
    }
}

// ---- the glass: mud and drops
struct Splat { float x, y, r, life; uint8_t kind; };
static Splat s_spl[14];
void renderSplat(float strength) {
    for (Splat& s : s_spl) if (s.life <= 0.f) {
        uint32_t h = hash32((uint32_t)(s_t * 1000.f) + (uint32_t)(strength * 99.f));
        bool left = h & 1;
        s.x = left ? (h >> 4) % 40 : W - 1 - (h >> 4) % 40; s.y = 10 + (h >> 12) % 90; s.r = 2.f + strength * 4.f + (h >> 20) % 3;
        s.life = 2.5f + strength * 3.f; s.kind = 0; return;
    }
}
void renderWater(float strength) {
    for (Splat& s : s_spl) if (s.life <= 0.f) {
        uint32_t h = hash32((uint32_t)(s_t * 997.f) + 7u);
        s.x = (float)(h % W); s.y = (float)((h >> 8) % 90); s.r = 1.f + strength * 1.5f; s.life = 1.5f; s.kind = 1; return;
    }
}
static void drawGlass() {
    float dt = g_scene.dt;
    if (g_view.view == VIEW_COCKPIT && g_scene.rain > 0.1f && hashf((uint32_t)(s_t * 60.f)) < g_scene.rain * 0.6f) renderWater(0.3f);
    for (Splat& s : s_spl) {
        if (s.life <= 0.f) continue;
        s.life -= dt;
        if (s.kind == 1) s.y += dt * (6.f + g_view.speedMph * 0.3f);
        int R = (int)s.r;
        for (int dy = -R; dy <= R; dy++) for (int dx = -R; dx <= R; dx++) {
            if (dx * dx + dy * dy > R * R) continue;
            int x = (int)s.x + dx, y = (int)s.y + dy;
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
            if (s.kind == 0) { if (s.life > 1.f || dith(x, y) < s.life) fb[y * W + x] = (dx * dx + dy * dy < R * R / 3) ? P_MUD : (((x ^ y) & 1) ? P_MUD : fb[y * W + x]); }
            else fb[y * W + x] = s_bright[fb[y * W + x]];
        }
    }
}

// ---- instruments: as little as possible, out of the way
static void hudText(int x, int y, const char* s, uint8_t c, int sc = 1) { font3(fb, W, H, x, y, s, c, P_HUD_K, sc); }
static void drawHud() {
    const ViewInfo& v = g_view;
    char t[24];
    // speed: on the dash in the cockpit, a corner readout otherwise
    snprintf(t, sizeof t, "%d", (int)(v.speedMph + 0.5f));
    if (v.view == VIEW_COCKPIT) {
        static const int8_t SP[4][2] = {{82, 99}, {112, 101}, {113, 100}, {80, 111}};
        int w = font3Width(t);
        hudText(SP[v.vehicle][0] - w / 2, SP[v.vehicle][1], t, P_HUD_G);
    } else {
        int w = font3Width(t, 2);
        hudText(W - 4 - w, H - 13, t, P_HUD_W, 2);
        hudText(W - 4 - 11, H - 20, "MPH", P_HUD_W);
    }
    if (v.reverse) hudText(W - 8, 3, "R", P_HUD_A);
    // the pace note (the STI's co-driver)
    if (v.paceA > 0.1f) {
        char p[8];
        if (v.paceDir == 0) snprintf(p, sizeof p, "CREST");
        else snprintf(p, sizeof p, "%s%s%d", v.paceDir < 0 ? "<" : "", v.paceSev == 1 ? "H" : "", v.paceSev);
        if (v.paceDir > 0) { size_t n = strlen(p); p[n] = '>'; p[n + 1] = 0; }
        hudText(4, 4, p, v.paceSev <= 2 ? P_HUD_R : P_HUD_A, 2);
        if (v.paceCrest && v.paceDir != 0) hudText(4, 16, "CREST", P_HUD_A);
    }
    // a fork ahead: where each side goes
    if (v.juncA > 0.05f && v.juncL && v.juncR) {
        hudText(4, H - 22 - (v.view == VIEW_COCKPIT ? 26 : 0) + 30 - 30 + 0, "", P_HUD_W);
        int y = v.view == VIEW_COCKPIT ? 4 : 4;
        int wl = font3Width(v.juncL), wr = font3Width(v.juncR);
        hudText(W / 2 - wl - 6, y + (v.paceA > 0.1f ? 0 : 0), v.juncL, P_HUD_W);
        hudText(W / 2 + 6, y, v.juncR, P_HUD_W);
        (void)wr;
    }
    if (v.storeA > 0.05f) hudText(W / 2 - font3Width("$ MANTIS MOTORS") / 2, 12, "$ MANTIS MOTORS", P_HUD_A);
    if (v.offroadLeft >= 0.f) {
        snprintf(t, sizeof t, "%d", (int)ceilf(v.offroadLeft));
        hudText(W / 2 - font3Width(t, 3) / 2, 30, t, P_HUD_R, 3);
        hudText(W / 2 - font3Width("BACK TO THE ROAD") / 2, 48, "BACK TO THE ROAD", P_HUD_R);
    }
    if (v.msg && v.msgA > 0.05f) {
        hudText(W / 2 - font3Width(v.msg) / 2, 22, v.msg, P_HUD_W);
        if (v.sub) hudText(W / 2 - font3Width(v.sub) / 2, 30, v.sub, P_HUD_A);
    }
    if (v.showFps) { snprintf(t, sizeof t, "%d", v.fps); hudText(2, H - 8, t, P_HUD_G); if (g_memNote) hudText(2, H - 15, g_memNote, g_memNote[4] == 'O' ? P_HUD_G : P_HUD_R); }
}

// ================================================================================== frame
void renderSetVehicle(uint8_t veh) {
    s_veh = veh % V_COUNT;
    const art::CockArt& c = art::COCK_ART[s_veh];
    if (!s_cock) { s_cock = (uint8_t*)memSlow(W * H); s_wheelMask = (uint8_t*)memSlow(W * H); }
    if (!s_cock || !s_wheelMask) return;
    for (int i = 0; i < W * H; i++) s_cock[i] = (i & 1) ? (c.px[i >> 1] & 15) : (c.px[i >> 1] >> 4);
    memset(s_wheelMask, 0, W * H);
    if (c.wr > 0) {
        // the wheel: the dark pixels inside its circle (gauges seen through it stay put)
        int bg = 0, cnt[16] = {0};
        for (int y = c.wy - c.wr; y <= c.wy + c.wr; y++) for (int x = c.wx - c.wr; x <= c.wx + c.wr; x++) {
            if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) continue;
            int dx = x - c.wx, dy = y - c.wy;
            if (dx * dx + dy * dy > c.wr * c.wr) continue;
            uint8_t p = s_cock[y * W + x];
            if (!p) continue;
            const uint8_t* rgb = c.pal + (p - 1) * 3;
            float lum = rgb[0] * 0.3f + rgb[1] * 0.59f + rgb[2] * 0.11f;
            float sat = (float)(fmaxf(rgb[0], fmaxf(rgb[1], rgb[2])) - fminf(rgb[0], fminf(rgb[1], rgb[2])));
            int d2 = dx * dx + dy * dy;
            bool rim = d2 > (c.wr - 4) * (c.wr - 4);
            if ((lum < 70.f && sat < 60.f) || (rim && lum < 110.f)) s_wheelMask[y * W + x] = 1;
            else cnt[p]++;
        }
        for (int k = 1; k < 16; k++) if (cnt[k] > cnt[bg]) bg = k;
        // behind the wheel: the dash's own colour
        for (int i = 0; i < W * H; i++) if (s_wheelMask[i]) { /* drawn rotated; the base shows dash */ }
        (void)bg;
    }
    // the overlay as runs in internal RAM (drawn every cockpit frame: no PSRAM reads)
    if (!s_ckRun) { s_ckRun = (uint16_t*)memSlow(2400 * 2 * sizeof(uint16_t)); s_ckPix = (uint8_t*)memSlow(W * H / 2); }   // read straight through: PSRAM streams it
    s_ckRuns = 0;
    if (s_ckRun && s_ckPix) {
        int np = 0;
        for (int y = 0; y < H; y++) {
            int x = 0;
            while (x < W) {
                int i = y * W + x;
                if (!s_cock[i] || s_wheelMask[i]) { x++; continue; }
                int st = i, len = 0;
                while (x < W && s_cock[y * W + x] && !s_wheelMask[y * W + x] && np < W * H / 2) { s_ckPix[np++] = (uint8_t)(P_COCK + s_cock[y * W + x]); x++; len++; }
                if (s_ckRuns < 2400) { s_ckRun[s_ckRuns * 2] = (uint16_t)st; s_ckRun[s_ckRuns * 2 + 1] = (uint16_t)len; s_ckRuns++; }
                if (np >= W * H / 2) break;
            }
        }
    }
    // rows the dash covers solidly need no raycast
    for (int x = 0; x < W; x++) {
        int y = H;
        while (y > 0 && s_cock[(y - 1) * W + x]) y--;
        s_bottomCock[x] = (uint8_t)y;
    }
}

void renderInit() {
    if (!zb) zb = (uint8_t*)memFast(W * H);
    if (!s_tex) {
        s_tex = (uint8_t*)memFast(64 * 64);
        for (int v = 0; v < 64; v++) for (int u = 0; u < 64; u++) {
            float n = fbmP((float)u / 8.f, (float)v / 8.f, 8, 3, 0x7E57u);
            s_tex[v * 64 + u] = (uint8_t)clampi((int)(n * 255.f), 0, 255);
        }
    }
    spritesInit();
    buildLUTs();
    for (int k = 0; k < 80; k++) { float ty = clampf(k / 78.f, 0.f, 1.f); s_skyPow[k] = 9.f * powf(ty, 1.2f); s_skyTT[k] = ty * ty; }
    renderSetVehicle(0);
    memset(fb, 0, sizeof(fb));
}

void renderFrame() {
    const Scene& sc = g_scene;
    s_t = sc.t;
    if (g_view.vehicle != s_veh) renderSetVehicle(g_view.vehicle);
    buildPalette();
    setupCamera();
    // sunlight per baked normal, and fog by distance, once a frame
    float se = fmaxf(sc.sunElev, 0.f);
    s_sunDx = cosf(sc.sunAz) * cosf(se); s_sunDz = sinf(sc.sunAz) * cosf(se); s_sunDy = sinf(se) + 0.15f;
    for (int b = 0; b < 256; b++) {
        float nx = ((b >> 4) - 8) / 8.f, nz = ((b & 15) - 8) / 8.f;
        float ny = sqrtf(fmaxf(0.05f, 1.f - nx * nx - nz * nz));
        float d = fmaxf(0.f, nx * s_sunDx + ny * s_sunDy + nz * s_sunDz);
        s_light[b] = 0.22f + 0.1f * ny + 0.62f * d * s_sunK;
    }
    float vis = 330.f - sc.fog * 250.f - sc.rain * 90.f - sc.snow * 120.f - sc.dust * 80.f;
    vis = fmaxf(vis, 40.f);
    for (int z = 0; z < 520; z++) s_fog[z] = clampf((z - vis * 0.42f) / (vis * 1.0f), 0.f, 1.f);
    const Land& L = journeyLand();
    s_flowers = L.flowers; s_wet = sc.wet; s_paved = L.paved;
    if (g_view.vehicle != s_veh) renderSetVehicle(g_view.vehicle);
    if (g_view.view != VIEW_COCKPIT) memset(s_bottom, H, sizeof(s_bottom));
    else memcpy(s_bottom, s_bottomCock, sizeof(s_bottom));
    memset(zb, 255, W * H);
    drawTerrain(560.f);
    drawSkyAndPanorama();
    drawNight();
    drawSun();
    drawClouds();
    drawBolt();
    drawBirds();
    drawVegetation();
    drawSignAndStore();
    drawCritters();
    drawParticles();
    reflectPass();
    drawFireflies();
    drawMist();
    drawShafts();
    drawRain();
    drawSnow();
    drawLeaves();
    if (g_view.view == VIEW_CHASE) drawChaseVehicle();
    if (g_view.view == VIEW_COCKPIT) drawCockpit();
    drawGlass();
    drawHud();
    s_bob = 0.f;
    s_frameN++;
}

const uint8_t* frameBuffer() { return fb; }
bool renderDepthFast() { return memIsFast(zb); }
const char* g_memNote = nullptr;
const uint16_t* framePalette565() { return pal565; }
const uint8_t* framePaletteRGB() { return palRGB; }
int frameBob() { return 0; }

}
