#pragma GCC optimize("Os")
// Boot-time pixel art. Everything is drawn with a few primitives into small templates: shade 0..15 is baked
// lighting (sun from the upper right; the renderer mirrors it when the sun is on the left).
#include "sprites.h"
#include "vehicle.h"
#include "universe.h"
#include "mem.h"
#include "art.h"
#include <math.h>
#include <string.h>

namespace mantis {

static Tpl s_tpl[T_TPL_N];

static inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ---------------------------------------------------------------- template canvas helpers
static Tpl* T;
static void tNew(uint8_t id, int w, int h) {
    T = &s_tpl[id];
    T->w = (uint8_t)w; T->h = (uint8_t)h; T->ax = (int8_t)(w / 2);
    T->px = (uint8_t*)memSlow((size_t)w * h);
}
static inline void tp(int x, int y, int mat, int shade) {
    if ((unsigned)x >= T->w || (unsigned)y >= T->h) return;
    T->px[y * T->w + x] = (uint8_t)((mat << 4) | clampi(shade, 0, 15));
}
static inline bool tset(int x, int y) { return (unsigned)x < T->w && (unsigned)y < T->h && T->px[y * T->w + x]; }
static void tDisc(float cx, float cy, float rx, float ry, int mat, float lit, uint32_t seed, float rough) {
    for (int y = (int)(cy - ry - 1); y <= (int)(cy + ry + 1); y++)
        for (int x = (int)(cx - rx - 1); x <= (int)(cx + rx + 1); x++) {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            float d = dx * dx + dy * dy;
            float edge = 1.f + rough * (hashf(seed + x * 31 + y * 17) - 0.5f);
            if (d > edge) continue;
            // a sphere lit from the upper right
            float nz = sqrtf(fmaxf(0.f, 1.f - fminf(d, 1.f)));
            float l = 0.55f * dx - 0.6f * dy + 0.6f * nz;
            int sh = (int)(lit + l * 6.f + (hashf(seed ^ (x * 7 + y * 13)) - 0.5f) * 2.5f);
            tp(x, y, mat, sh);
        }
}
static void tLine(float x0, float y0, float x1, float y1, float w0, float w1, int mat, int shade, uint32_t seed) {
    int n = (int)(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) * 2.f) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t, w = w0 + (w1 - w0) * t;
        for (int k = -(int)w; k <= (int)w; k++) {
            int sh = shade + (k > 0 ? 2 : (k < 0 ? -1 : 0)) + (int)((hashf(seed + i * 3 + k) - 0.5f) * 2.f);
            tp((int)(x + k), (int)y, mat, sh);
        }
    }
}

// ---------------------------------------------------------------- vegetation
static void makePine(uint8_t id, uint32_t seed, int tiers, bool snow) {
    tNew(id, 26, 60);
    int cx = 13;
    tLine(cx, 59, cx, 8, 1.2f, 0.4f, TM_BARK, 5, seed);
    int top = 1, bot = 52;
    for (int t = 0; t < tiers; t++) {
        float p = (t + 1.f) / tiers;
        int y0 = top + (int)((bot - top) * (t / (float)tiers) * 0.95f);
        int y1 = top + (int)((bot - top) * p);
        float hw = 1.5f + p * 11.f * (0.85f + 0.3f * hashf(seed + t));
        for (int y = y0; y <= y1; y++) {
            float u = (float)(y - y0) / fmaxf(1.f, (float)(y1 - y0));
            float w = hw * (0.25f + 0.75f * u);
            for (int x = (int)(cx - w - 1); x <= (int)(cx + w + 1); x++) {
                float dx = (x + 0.5f - cx) / w;
                float jag = 1.f - 0.18f * hashf(seed * 7 + x * 13 + y * 5) - (u > 0.8f ? 0.25f * hashf(seed + x + y * 3) : 0.f);
                if (fabsf(dx) > jag) continue;
                int sh = 4 + (int)(dx * 3.5f) - (int)(u * 3.f) + (u < 0.25f ? 2 : 0) + (int)(hashf(seed ^ (x * 7 + y * 11)) * 2.4f);
                if (snow && u < 0.35f + 0.2f * hashf(seed + x)) tp(x, y, TM_SNOW, 9 + (int)(dx * 3.f));
                else tp(x, y, TM_LEAF, sh);
            }
        }
    }
    T->ax = cx;
}
static void makeBroad(uint8_t id, uint32_t seed) {
    tNew(id, 44, 46);
    int cx = 22;
    tLine(cx, 45, cx + 1, 22, 2.0f, 1.0f, TM_BARK, 5, seed);
    tLine(cx, 30, cx - 9, 18, 1.0f, 0.4f, TM_BARK, 4, seed + 1);
    tLine(cx + 1, 27, cx + 10, 16, 1.0f, 0.4f, TM_BARK, 6, seed + 2);
    for (int i = 0; i < 9; i++) {
        float a = hashf(seed + i * 11) * 6.283f, r = hashf(seed + i * 13) * 11.f;
        float x = cx + cosf(a) * r * 1.3f, y = 17 + sinf(a) * r * 0.75f;
        float rr = 6.f + hashf(seed + i * 17) * 5.f;
        tDisc(x, y, rr, rr * 0.85f, TM_LEAF, 5.5f, seed + i * 97, 0.5f);
    }
    T->ax = cx;
}
static void makeRedwood(uint8_t id, uint32_t seed) {
    tNew(id, 30, 100);
    int cx = 15;
    for (int y = 30; y < 100; y++) {
        float w = 2.2f + (y - 30) * 0.03f + (y > 92 ? (y - 92) * 0.4f : 0.f);
        for (int x = (int)(cx - w); x <= (int)(cx + w); x++) {
            float dx = (x + 0.5f - cx) / w;
            int fiss = (int)(hashf(x * 131 + seed) * 3.f) - ((x & 1) ? 1 : 0);
            tp(x, y, TM_BARK, 7 + (int)(dx * 4.f) + fiss);
        }
    }
    tLine(cx, 30, cx, 2, 1.6f, 0.5f, TM_BARK, 7, seed);
    for (int i = 0; i < 12; i++) {
        float y = 4 + i * 4.2f + hashf(seed + i) * 2.f;
        float hw = 4.f + i * 0.7f + hashf(seed + i * 3) * 3.f;
        float side = (i & 1) ? 1.f : -1.f;
        tDisc(cx + side * hw * 0.35f, y, hw, 2.3f, TM_LEAF, 5.f, seed + i * 41, 0.6f);
    }
    T->ax = cx;
}
static void makeBush(uint8_t id, uint32_t seed) {
    tNew(id, 24, 14);
    for (int i = 0; i < 5; i++)
        tDisc(5 + i * 3.5f, 8 + hashf(seed + i) * 2.f, 4.f + hashf(seed + i * 3) * 2.f, 4.f, TM_LEAF, 5.f, seed + i * 7, 0.6f);
    T->ax = 12;
}
static void makeCactus(uint8_t id) {
    tNew(id, 20, 40);
    int cx = 10;
    for (int y = 3; y < 40; y++) for (int x = cx - 2; x <= cx + 2; x++) tp(x, y, TM_LEAF, 5 + (x - cx) * 2 + ((x & 1) ? 1 : 0));
    for (int y = 1; y < 4; y++) for (int x = cx - 1; x <= cx + 1; x++) tp(x, y, TM_LEAF, 7 + (x - cx));
    // arms
    for (int x = cx - 7; x < cx - 2; x++) for (int y = 20; y < 23; y++) tp(x, y, TM_LEAF, 5);
    for (int y = 9; y < 23; y++) for (int x = cx - 8; x <= cx - 6; x++) tp(x, y, TM_LEAF, 5 + (x - (cx - 7)) * 2);
    for (int x = cx + 3; x < cx + 7; x++) for (int y = 25; y < 28; y++) tp(x, y, TM_LEAF, 7);
    for (int y = 14; y < 28; y++) for (int x = cx + 6; x <= cx + 8; x++) tp(x, y, TM_LEAF, 6 + (x - (cx + 7)) * 2);
    T->ax = cx;
}
static void makeSnag(uint8_t id, uint32_t seed) {
    tNew(id, 30, 50);
    int cx = 15;
    tLine(cx, 49, cx + 1, 4, 1.6f, 0.4f, TM_BARK, 3, seed);
    for (int i = 0; i < 6; i++) {
        float y = 10 + i * 6.f;
        float s = (i & 1) ? 1.f : -1.f;
        tLine(cx, y + 4, cx + s * (5 + hashf(seed + i) * 7.f), y - 3, 0.8f, 0.2f, TM_BARK, 3, seed + i);
    }
    for (int i = 0; i < 18; i++) tp(cx + (int)((hashf(seed + i * 5) - 0.5f) * 14.f), 8 + (int)(hashf(seed + i * 9) * 34.f), TM_LEAF, 3);   // moss
    T->ax = cx;
}
static void makeBoulder(uint8_t id, uint32_t seed) {
    tNew(id, 32, 20);
    tDisc(16, 13, 14.f, 9.f, TM_ROCK, 6.f, seed, 0.25f);
    tDisc(9, 15, 7.f, 6.f, TM_ROCK, 5.f, seed + 3, 0.3f);
    for (int y = 0; y < 20; y++) for (int x = 0; x < 32; x++)       // facets
        if (tset(x, y) && hashf(seed + (x / 4) * 7 + (y / 3) * 13) > 0.7f) tp(x, y, TM_ROCK, (T->px[y * 32 + x] & 15) + 2);
    T->ax = 16;
}
static void makeFlowers(uint8_t id, uint32_t seed) {
    tNew(id, 16, 6);
    for (int i = 0; i < 14; i++) {
        int x = (int)(hashf(seed + i) * 16.f), y = 1 + (int)(hashf(seed + i * 7) * 5.f);
        tp(x, y + 1, TM_LEAF, 4);
        tp(x, y, TM_FLOWER, (int)(hashf(seed + i * 3) * 8.f));
    }
    T->ax = 8;
}
static void makeStore() {
    tNew(T_STORE, 64, 40);
    // the building
    for (int y = 14; y < 40; y++) for (int x = 4; x < 60; x++) tp(x, y, TM_WALL, 8 + ((y & 3) == 0 ? -1 : 0) + (x > 40 ? -2 : 0));
    for (int y = 10; y < 15; y++) for (int x = 2 + (14 - y); x < 62 - (14 - y); x++) tp(x, y, TM_ROOF, 4 + (y - 10));
    // two garage doors and a lit window
    for (int d = 0; d < 2; d++)
        for (int y = 22; y < 40; y++) for (int x = 8 + d * 16; x < 22 + d * 16; x++) tp(x, y, TM_ROOF, 7 + ((y & 1) ? 0 : 2));
    for (int y = 22; y < 32; y++) for (int x = 44; x < 56; x++) tp(x, y, TM_GLASS, ((x + y) & 3) ? 6 : 10);
    // the big sign on poles
    for (int y = 0; y < 9; y++) for (int x = 6; x < 58; x++) tp(x, y, TM_SIGN, (y == 0 || y == 8 || x == 6 || x == 57) ? 15 : 8);
    for (int y = 9; y < 11; y++) { tp(14, y, TM_ROOF, 3); tp(50, y, TM_ROOF, 3); }
    // MANTIS MOTORS in 3x5 glyphs
    static const char* txt = "MANTIS MOTORS";
    extern int font3Glyph(char c);
    int x = 8;
    for (const char* p = txt; *p; p++) {
        int g = font3Glyph(*p);
        for (int r = 0; r < 5; r++) for (int c = 0; c < 3; c++) if (g & (1 << (14 - (r * 3 + c)))) tp(x + c, 2 + r, TM_SIGNTXT, 15);
        x += 4;
    }
    T->ax = 32;
}
static void makeDeer(uint8_t id, int pose, uint32_t seed) {
    tNew(id, 22, 18);
    // body
    tDisc(10, 8, 6.5f, 3.2f, TM_BARK, 7.f, seed, 0.1f);
    // neck + head (pose 3 grazes)
    if (pose == 3) { tLine(15, 7, 18, 13, 1.2f, 1.f, TM_BARK, 8, seed); tDisc(19, 14, 2.2f, 1.5f, TM_BARK, 8.f, seed, 0.f); }
    else {
        tLine(15, 7, 17, 2, 1.3f, 1.0f, TM_BARK, 8, seed);
        tDisc(18.5f, 2.f, 2.3f, 1.4f, TM_BARK, 8.f, seed, 0.f);
        tp(16, 0, TM_BARK, 6); tp(17, 0, TM_BARK, 7);           // ears
        if (seed & 1) { tp(17, -1 + 1, TM_BARK, 11); tp(16, 0, TM_BARK, 11); tp(15, 0, TM_BARK, 11); }   // little antlers
    }
    tp(4, 6, TM_WHITE, 15); tp(3, 6, TM_WHITE, 15); tp(4, 7, TM_WHITE, 13);  // tail
    // legs
    static const int8_t L[3][4] = {{0, 0, 0, 0}, {-2, 1, 2, -1}, {2, -1, -2, 1}};
    int f = pose == 3 ? 0 : (pose > 2 ? 0 : pose);
    int lx[4] = {6, 8, 13, 15};
    for (int i = 0; i < 4; i++) tLine((float)lx[i], 10, (float)(lx[i] + L[f][i]), 17, 0.45f, 0.3f, TM_BARK, i < 2 ? 5 : 6, seed + i);
    T->ax = 10;
}

const Tpl& tpl(uint8_t id) { return s_tpl[id % T_TPL_N]; }

void spritesInit() {
    if (s_tpl[0].px) return;
    art::artInit();                                   // the packed art, into PSRAM (once)
    makePine(T_PINE0, 11, 7, false);
    makePine(T_PINE1, 23, 9, false);
    makePine(T_PINE2, 37, 6, false);
    makePine(T_PINESNOW, 53, 8, true);
    makeBroad(T_BROAD0, 71);
    makeBroad(T_BROAD1, 89);
    makeRedwood(T_REDWOOD, 101);
    makeBush(T_BUSH, 113);
    makeCactus(T_CACTUS);
    makeSnag(T_SNAG, 131);
    makeBoulder(T_BOULDER, 149);
    makeFlowers(T_FLOWERS, 157);
    makeStore();
    for (int p = 0; p < 4; p++) makeDeer((uint8_t)(T_DEER0 + p), p, 1);
    // the MANTIS MOTORS arrow (from the sign sheet: tools/sprites/make_store_art.py)
    s_tpl[T_MMSIGN].w = art::MM_SIGN_W; s_tpl[T_MMSIGN].h = art::MM_SIGN_H; s_tpl[T_MMSIGN].ax = (int8_t)art::MM_SIGN_AX;
    s_tpl[T_MMSIGN].px = const_cast<uint8_t*>(art::MM_SIGN);
    s_tpl[T_MMSIGN_L].w = art::MM_SIGN_W; s_tpl[T_MMSIGN_L].h = art::MM_SIGN_H; s_tpl[T_MMSIGN_L].ax = (int8_t)art::MM_SIGN_AXL;
    s_tpl[T_MMSIGN_L].px = const_cast<uint8_t*>(art::MM_SIGN_L);
}

// ---------------------------------------------------------------- 3x5 font
// 15 bits per glyph, rows top to bottom, 3 columns each (msb first)
struct G3 { char c; uint16_t b; };
static const G3 F3[] = {
    {'0', 0x7B6F}, {'1', 0x2C97}, {'2', 0x62A7}, {'3', 0x628E}, {'4', 0x5BC9}, {'5', 0x798E}, {'6', 0x39EF}, {'7', 0x7292}, {'8', 0x7BEF}, {'9', 0x7BCE}, {'A', 0x2BED}, {'B', 0x6BAE}, {'C', 0x3923}, {'D', 0x6B6E}, {'E', 0x79A7}, {'F', 0x79A4}, {'G', 0x396B}, {'H', 0x5BED}, {'I', 0x7497}, {'J', 0x126A}, {'K', 0x5BAD}, {'L', 0x4927}, {'M', 0x5FED}, {'N', 0x6B6D}, {'O', 0x2B6A}, {'P', 0x6BA4}, {'Q', 0x2B73}, {'R', 0x6BAD}, {'S', 0x388E}, {'T', 0x7492}, {'U', 0x5B6F}, {'V', 0x5B6A}, {'W', 0x5BFD}, {'X', 0x5AAD}, {'Y', 0x5A92}, {'Z', 0x72A7}, {'.', 0x0002}, {'-', 0x01C0}, {':', 0x0410}, {'/', 0x12A4}, {'<', 0x1511}, {'>', 0x4454}, {'+', 0x05D0}, {'!', 0x2482}, {'$', 0x3C9E}, {'\'', 0x2400}, {'%', 0x52A5}, {'?', 0x6282}, {'^', 0x2A00}, {'#', 0x5F7D}, {'*', 0x0AA8},
};
int font3Glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (unsigned i = 0; i < sizeof(F3) / sizeof(F3[0]); i++) if (F3[i].c == c) return F3[i].b;
    return 0;
}
int font3Width(const char* s, int scale) { int n = 0; while (*s++) n++; return n ? (n * 4 - 1) * scale : 0; }
int font3(uint8_t* fb, int W, int H, int x, int y, const char* s, uint8_t c, uint8_t outline, int sc) {
    int x0 = x;
    for (const char* p = s; *p; p++, x += 4 * sc) {
        int g = font3Glyph(*p);
        if (!g) continue;
        for (int pass = outline ? 0 : 1; pass < 2; pass++)
            for (int r = 0; r < 5; r++) for (int q = 0; q < 3; q++) {
                if (!(g & (1 << (14 - (r * 3 + q))))) continue;
                for (int yy = 0; yy < sc; yy++) for (int xx = 0; xx < sc; xx++) {
                    int px = x + q * sc + xx, py = y + r * sc + yy;
                    if (pass == 0) {
                        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                            int ox = px + dx, oy = py + dy;
                            if ((unsigned)ox < (unsigned)W && (unsigned)oy < (unsigned)H) fb[oy * W + ox] = outline;
                        }
                    } else if ((unsigned)px < (unsigned)W && (unsigned)py < (unsigned)H) fb[py * W + px] = c;
                }
            }
    }
    return x - x0 - sc;
}

}
