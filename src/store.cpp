#pragma GCC optimize("Os")
#include "store.h"
#include "progress.h"
#include "vehicle.h"
#include "journey.h"
#include "display.h"
#include "ui.h"
#include "vehicle_art.h"
#include <stdio.h>
#include <string.h>

namespace mantis {

static bool s_open = false, s_dirty = true;
static int s_msgT = 0; static char s_msg[40];

void storeOpen() { s_open = true; s_dirty = true; s_msg[0] = 0; }
bool storeActive() { return s_open; }

static void rect(uint16_t* b, int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++) { if (yy < 0 || yy >= 240) continue; for (int xx = x; xx < x + w; xx++) if (xx >= 0 && xx < 320) b[yy * 320 + xx] = c; }
}
static void frame(uint16_t* b, int x, int y, int w, int h, uint16_t c) { rect(b, x, y, w, 2, c); rect(b, x, y + h - 2, w, 2, c); rect(b, x, y, 2, h, c); rect(b, x + w - 2, y, 2, h, c); }
static void text(uint16_t* b, int x, int y, const char* s, uint16_t c, int sc = 1) { uiText(b, 320, 240, x, y, s, c, sc, true); }
static void textC(uint16_t* b, int cx, int y, const char* s, uint16_t c, int sc = 1) { text(b, cx - uiTextWidth(s, sc) / 2, y, s, c, sc); }

static void drawVehicle(uint16_t* b, int v, int cx, int baseY, int sc) {
    const art::VehArt& a = art::VEH_ART[v];
    const uint8_t* px = a.px;
    for (int y = 0; y < a.h; y++) for (int x = 0; x < a.w; x++) {
        int k = y * a.w + x;
        uint8_t p = (k & 1) ? (px[k >> 1] & 15) : (px[k >> 1] >> 4);
        if (!p) continue;
        const uint8_t* c = a.pal + (p - 1) * 3;
        rect(b, cx + (x - a.ax) * sc, baseY + (y - a.ay) * sc, sc, sc, rgb565(c[0], c[1], c[2]));
    }
}

// layout (overlay pixels)
struct Btn { int x, y, w, h; };
static const Btn BUY = {182, 150, 124, 30}, DRIVE = {182, 196, 124, 30}, OFFR = {14, 196, 156, 30};
static Btn garageBtn(int i) { Btn g = {14 + i * 40, 154, 36, 30}; return g; }
static bool hit(const Btn& b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static void draw() {
    uint16_t* b = overlayBuffer();
    if (!b) return;
    const uint16_t bg = rgb565(10, 14, 22), panel = rgb565(18, 26, 38), teal = rgb565(0, 200, 180), lime = rgb565(150, 255, 70),
                   white = rgb565(240, 244, 250), amber = rgb565(255, 190, 50), grey = rgb565(90, 96, 110), mag = rgb565(200, 60, 170);
    rect(b, 0, 0, 320, 240, bg);
    frame(b, 4, 4, 312, 232, teal);
    textC(b, 160, 10, "$ MANTIS MOTORS $", lime, 2);
    int nxt = progressNextLocked();
    if (nxt >= 0) {
        const VehicleSpec& S = vehicleSpec((uint8_t)nxt);
        rect(b, 12, 32, 296, 112, panel);
        drawVehicle(b, nxt, 82, 118, nxt == V_BIKE ? 2 : 2);
        text(b, 156, 38, S.name, white, 1);
        text(b, 156, 52, S.blurb1, grey, 1);
        text(b, 156, 64, S.blurb2, mag, 1);
        const char* lab[3] = {"SPEED", "GRIP", "OFF-ROAD"};
        float val[3] = {S.statSpeed, S.statGrip, S.statOffroad};
        for (int i = 0; i < 3; i++) {
            text(b, 156, 80 + i * 12, lab[i], grey, 1);
            rect(b, 214, 81 + i * 12, 86, 6, rgb565(40, 46, 60));
            rect(b, 214, 81 + i * 12, (int)(86 * val[i]), 6, i == 2 ? amber : teal);
        }
        char t[48];
        snprintf(t, sizeof t, "COST %d MI IN THE %s", (int)S.priceMiles, vehicleSpec(S.priceVehicle).shortName);
        text(b, 156, 118, t, amber, 1);
        snprintf(t, sizeof t, "YOU HAVE %.1f MI", g_prog.wallet[S.priceVehicle]);
        text(b, 156, 130, t, white, 1);
        bool can = progressCanBuy(nxt);
        rect(b, BUY.x, BUY.y, BUY.w, BUY.h, can ? rgb565(30, 120, 60) : rgb565(50, 54, 62));
        textC(b, BUY.x + BUY.w / 2, BUY.y + 8, "BUY", can ? white : grey, 2);
    } else {
        rect(b, 12, 32, 296, 112, panel);
        textC(b, 160, 70, "YOU OWN EVERY VEHICLE", lime, 1);
        textC(b, 160, 86, "THE WORLD IS YOURS", white, 1);
    }
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
    } else {
        char t[40]; int n = 0; for (int i = 0; i < B_COUNT; i++) if (g_prog.visited & (1u << i)) n++;
        snprintf(t, sizeof t, "BIOMES SEEN %d/%d", n, (int)B_COUNT);
        text(b, 14, 204, t, grey, 1);
        text(b, 14, 216, "SEE THEM ALL: OFF-ROAD FOR ALL", grey, 1);
    }
    rect(b, DRIVE.x, DRIVE.y, DRIVE.w, DRIVE.h, rgb565(0, 100, 130));
    textC(b, DRIVE.x + DRIVE.w / 2, DRIVE.y + 8, "DRIVE ON", white, 2);
    if (s_msg[0]) textC(b, 160, 186 - 46, s_msg, lime, 1);
    overlayEnable(true);
}

bool storeUpdate(bool tap, int tx, int ty, bool& changed) {
    changed = false;
    if (!s_open) return false;
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
