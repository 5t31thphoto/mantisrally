#pragma GCC optimize("Os")
#include "store.h"
#include "progress.h"
#include "vehicle.h"
#include "journey.h"
#include "display.h"
#include "art.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>

namespace mantis {

static bool s_open = false, s_dirty = true, s_tune = false;   // s_tune: the tuning shop page
static int s_msgT = 0; static char s_msg[40];

void storeOpen() { s_open = true; s_dirty = true; s_tune = false; s_msg[0] = 0; }
bool storeActive() { return s_open; }

static void rect(uint16_t* b, int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++) { if (yy < 0 || yy >= 240) continue; for (int xx = x; xx < x + w; xx++) if (xx >= 0 && xx < 320) b[yy * 320 + xx] = c; }
}
static void frame(uint16_t* b, int x, int y, int w, int h, uint16_t c) { rect(b, x, y, w, 2, c); rect(b, x, y + h - 2, w, 2, c); rect(b, x, y, 2, h, c); rect(b, x + w - 2, y, 2, h, c); }
static void text(uint16_t* b, int x, int y, const char* s, uint16_t c, int sc = 1) { uiText(b, 320, 240, x, y, s, c, sc, true); }
static void textC(uint16_t* b, int cx, int y, const char* s, uint16_t c, int sc = 1) { text(b, cx - uiTextWidth(s, sc) / 2, y, s, c, sc); }
// word-wrapped to maxW pixels; returns the next free y
static int textWrap(uint16_t* b, int x, int y, const char* s, uint16_t c, int maxW, int lineH) {
    char line[48]; int n = 0; const char* p = s;
    while (*p) {
        const char* w = p; while (*w && *w != ' ') w++;
        int wl = (int)(w - p);
        char test[48]; snprintf(test, sizeof test, "%.*s%s%.*s", n, line, n ? " " : "", wl, p);
        if (n && uiTextWidth(test, 1) > maxW) { line[n] = 0; text(b, x, y, line, c, 1); y += lineH; n = 0; snprintf(line, sizeof line, "%.*s", wl, p); n = wl; }
        else { snprintf(line, sizeof line, "%s", test); n = (int)strlen(line); }
        p = *w ? w + 1 : w;
    }
    if (n) { line[n] = 0; text(b, x, y, line, c, 1); y += lineH; }
    return y;
}

// the vehicle for sale in profile: the sprite sheet's side view, full colour
static void drawSide(uint16_t* b, int v, int cx, int baseY) {
    const art::SideArt& a = art::SIDE_ART[v];
    int x0 = cx - a.w / 2, y0 = baseY - a.h;
    for (int y = 0; y < a.h; y++) for (int x = 0; x < a.w; x++) {
        uint16_t p = a.px[y * a.w + x];
        if (!p) continue;
        int X = x0 + x, Y = y0 + y;
        if ((unsigned)X < 320u && (unsigned)Y < 240u) b[Y * 320 + X] = p;
    }
}

// layout (overlay pixels)
struct Btn { int x, y, w, h; };
static const Btn BUY = {182, 150, 60, 30}, TUNE = {246, 150, 60, 30}, DRIVE = {182, 196, 124, 30}, OFFR = {14, 196, 156, 30},
                 BACK = {14, 196, 156, 30};
static Btn upgBtn(int k) { Btn u = {230, 46 + k * 40, 76, 30}; return u; }
static Btn garageBtn(int i) { Btn g = {14 + i * 40, 154, 36, 30}; return g; }
static bool hit(const Btn& b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static void drawTune(uint16_t* b) {
    const uint16_t panel = rgb565(18, 26, 38), lime = rgb565(150, 255, 70), white = rgb565(240, 244, 250),
                   amber = rgb565(255, 190, 50), grey = rgb565(90, 96, 110), teal = rgb565(0, 200, 180);
    char t[48];
    snprintf(t, sizeof t, "TUNING THE %s", vehicleSpec(g_prog.current).shortName);
    text(b, 14, 32, t, white, 1);
    snprintf(t, sizeof t, "STYLE %u", (unsigned)g_prog.style);
    text(b, 306 - uiTextWidth(t, 1), 32, t, lime, 1);
    static const char* NAME[UPG_N] = {"HANDLING", "GRIP", "POWER"};
    static const char* WHAT[UPG_N] = {"SHARPER STEERING", "MORE TYRE BITE", "MORE TORQUE"};
    for (int k = 0; k < UPG_N; k++) {
        Btn u = upgBtn(k);
        rect(b, 12, u.y - 2, 296, 34, panel);
        text(b, 18, u.y + 2, NAME[k], white, 1);
        text(b, 18, u.y + 13, WHAT[k], grey, 1);
        int lv = progressUpgradeLevel(k);
        for (int p = 0; p < 3; p++) rect(b, 176 + p * 12, u.y + 3, 9, 6, p < lv ? teal : rgb565(40, 46, 60));
        int c = progressUpgradeCost(k);
        if (c > 0) {
            snprintf(t, sizeof t, "%d", c);
            text(b, 176, u.y + 13, t, amber, 1);
            bool can = g_prog.style >= (uint32_t)c;
            rect(b, u.x, u.y, u.w, u.h, can ? rgb565(30, 120, 60) : rgb565(50, 54, 62));
            textC(b, u.x + u.w / 2, u.y + 11, "UPGRADE", can ? white : grey, 1);
        } else {
            text(b, 176, u.y + 13, "MAXED", lime, 1);
        }
    }
    textC(b, 160, 168, "EARN STYLE WITH AIRTIME AND DRIFTS", amber, 1);
    rect(b, BACK.x, BACK.y, BACK.w, BACK.h, rgb565(36, 42, 56));
    textC(b, BACK.x + BACK.w / 2, BACK.y + 8, "BACK", white, 2);
}

static void draw() {
    uint16_t* b = overlayBuffer();
    if (!b) return;
    const uint16_t bg = rgb565(10, 14, 22), panel = rgb565(18, 26, 38), teal = rgb565(0, 200, 180), lime = rgb565(150, 255, 70),
                   white = rgb565(240, 244, 250), amber = rgb565(255, 190, 50), grey = rgb565(90, 96, 110), mag = rgb565(200, 60, 170);
    rect(b, 0, 0, 320, 240, bg);
    frame(b, 4, 4, 312, 232, teal);
    textC(b, 160, 10, "$ MANTIS MOTORS $", lime, 2);
    if (s_tune) {
        drawTune(b);
        rect(b, DRIVE.x, DRIVE.y, DRIVE.w, DRIVE.h, rgb565(0, 100, 130));
        textC(b, DRIVE.x + DRIVE.w / 2, DRIVE.y + 8, "DRIVE ON", white, 2);
        if (s_msg[0]) textC(b, 160, 180, s_msg, lime, 1);
        overlayEnable(true);
        return;
    }
    int nxt = progressNextLocked();
    if (nxt >= 0) {
        const VehicleSpec& S = vehicleSpec((uint8_t)nxt);
        rect(b, 12, 32, 296, 112, panel);
        drawSide(b, nxt, 82, 132);
        text(b, 156, 36, S.name, white, 1);
        int ty = textWrap(b, 156, 47, S.blurb1, grey, 150, 9);
        textWrap(b, 156, ty, S.blurb2, mag, 150, 9);
        const char* lab[3] = {"SPEED", "GRIP", "OFF-ROAD"};
        float val[3] = {S.statSpeed, S.statGrip, S.statOffroad};
        for (int i = 0; i < 3; i++) {
            text(b, 156, 86 + i * 11, lab[i], grey, 1);
            rect(b, 214, 87 + i * 11, 86, 6, rgb565(40, 46, 60));
            rect(b, 214, 87 + i * 11, (int)(86 * val[i]), 6, i == 2 ? amber : teal);
        }
        char t[48];
        snprintf(t, sizeof t, "COST %d MI IN THE %s", (int)S.priceMiles, vehicleSpec(S.priceVehicle).shortName);
        text(b, 156, 120, t, amber, 1);
        snprintf(t, sizeof t, "YOU HAVE %.1f MI", g_prog.wallet[S.priceVehicle]);
        text(b, 156, 131, t, white, 1);
        bool can = progressCanBuy(nxt);
        rect(b, BUY.x, BUY.y, BUY.w, BUY.h, can ? rgb565(30, 120, 60) : rgb565(50, 54, 62));
        textC(b, BUY.x + BUY.w / 2, BUY.y + 8, "BUY", can ? white : grey, 2);
    } else {
        rect(b, 12, 32, 296, 112, panel);
        textC(b, 160, 70, "YOU OWN EVERY VEHICLE", lime, 1);
        textC(b, 160, 86, "THE WORLD IS YOURS", white, 1);
    }
    rect(b, TUNE.x, TUNE.y, TUNE.w, TUNE.h, rgb565(120, 60, 140));      // the tuning shop
    textC(b, TUNE.x + TUNE.w / 2, TUNE.y + 8, "TUNE", white, 2);
    {   char st[24]; snprintf(st, sizeof st, "STYLE %u", (unsigned)g_prog.style); text(b, 70, 146, st, lime, 1); }
    // garage
    text(b, 14, 146, "GARAGE", grey, 1);
    for (int v = 0; v < V_COUNT; v++) {
        if (!(g_prog.owned & (1u << v))) continue;
        Btn g = garageBtn(v);
        rect(b, g.x, g.y, g.w, g.h, v == g_prog.current ? rgb565(0, 90, 80) : rgb565(36, 42, 56));
        textC(b, g.x + g.w / 2, g.y + 12, vehicleSpec((uint8_t)v).shortName, v == g_prog.current ? lime : white, 1);
    }
    if (g_prog.offroadUnlocked) {
        rect(b, OFFR.x, OFFR.y, OFFR.w, OFFR.h, g_prog.offroadOn ? rgb565(120, 80, 20) : rgb565(36, 42, 56));
        textC(b, OFFR.x + OFFR.w / 2, OFFR.y + 11, g_prog.offroadOn ? "OFF-ROAD MODE: ON" : "OFF-ROAD MODE: OFF", amber, 1);
    }                                                   // (nothing about it until it's earned: it's a surprise)
    rect(b, DRIVE.x, DRIVE.y, DRIVE.w, DRIVE.h, rgb565(0, 100, 130));
    textC(b, DRIVE.x + DRIVE.w / 2, DRIVE.y + 8, "DRIVE ON", white, 2);
    if (s_msg[0]) textC(b, 160, 186 - 46, s_msg, lime, 1);
    overlayEnable(true);
}

bool storeUpdate(bool tap, int tx, int ty, bool& changed) {
    changed = false;
    if (!s_open) return false;
    if (tap && s_tune) {
        static const char* NAME[UPG_N] = {"HANDLING", "GRIP", "POWER"};
        for (int k = 0; k < UPG_N; k++)
            if (hit(upgBtn(k), tx, ty) && progressBuyUpgrade(k)) { changed = true; snprintf(s_msg, sizeof s_msg, "%s UP TO LEVEL %d", NAME[k], progressUpgradeLevel(k)); }
        if (hit(BACK, tx, ty)) { s_tune = false; s_msg[0] = 0; }
        if (hit(DRIVE, tx, ty)) { s_open = false; overlayEnable(false); return false; }
        s_dirty = true;
        tap = false;
    }
    if (tap) {
        if (hit(TUNE, tx, ty)) { s_tune = true; s_msg[0] = 0; s_dirty = true; tap = false; }
    }
    if (tap) {
        int nxt = progressNextLocked();
        if (nxt >= 0 && hit(BUY, tx, ty) && progressBuy(nxt)) { changed = true; snprintf(s_msg, sizeof s_msg, "SOLD! ENJOY THE %s", vehicleSpec((uint8_t)nxt).shortName); }
        for (int v = 0; v < V_COUNT; v++) {
            if (!(g_prog.owned & (1u << v))) continue;
            if (hit(garageBtn(v), tx, ty) && g_prog.current != v) { g_prog.current = (uint8_t)v; changed = true; progressSave(); }
        }
        if (g_prog.offroadUnlocked && hit(OFFR, tx, ty)) { g_prog.offroadOn = !g_prog.offroadOn; progressSave(); }
        if (hit(DRIVE, tx, ty)) { s_open = false; overlayEnable(false); return false; }
        s_dirty = true;
    }
    if (s_dirty) { draw(); s_dirty = false; }
    (void)s_msgT;
    return true;
}

}
