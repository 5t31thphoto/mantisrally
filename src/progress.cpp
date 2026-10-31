#pragma GCC optimize("Os")
#include "progress.h"
#include "journey.h"
#include "storage.h"
#include <string.h>
#ifndef HOST
#include <Preferences.h>
#endif

namespace mantis {

Progress g_prog;
static float s_unsaved = 0.f;
static const uint32_t MAGIC = 0x4D52414Cu;   // "MRAL"
struct Blob { uint32_t magic; Progress p; };

// Saved to the SD card (/rally_saves); the internal flash is only a fallback when there is no card.
// Saves happen on events that matter (buying, switching, a new biome, the off-road switch, leaving the store) and
// every 5 miles of driving: the card is never hammered, and the flash almost never written.
static void fresh() {
    memset(&g_prog, 0, sizeof(g_prog));
    g_prog.owned = 1u << V_STI; g_prog.current = V_STI; g_prog.view = 0;
}

void progressInit() {
    fresh();
    Blob b;
    bool got = storageOk() && storageLoad(&b, sizeof(b)) && b.magic == MAGIC;
#ifndef HOST
    if (!got) {
        Preferences pr;
        if (pr.begin("mrally", true)) {
            got = pr.getBytes("prog", &b, sizeof(b)) == sizeof(b) && b.magic == MAGIC;
            pr.end();
        }
    }
#endif
    if (got) g_prog = b.p;
    g_prog.owned |= 1u << V_STI;
    if (!(g_prog.owned & (1u << g_prog.current))) g_prog.current = V_STI;
}

void progressSave() {
    Blob b; b.magic = MAGIC; b.p = g_prog;
    if (!storageSave(&b, sizeof(b))) {
#ifndef HOST
        Preferences pr;                                  // no card: the flash, rarely
        if (pr.begin("mrally", false)) { pr.putBytes("prog", &b, sizeof(b)); pr.end(); }
#endif
    }
    s_unsaved = 0.f;
}

void progressDrive(float m) {
    if (m <= 0.f) return;
    float mi = m / MILE;
    g_prog.miles[g_prog.current] += mi;
    g_prog.wallet[g_prog.current] += mi;
    s_unsaved += mi;
    if (s_unsaved > 5.f) progressSave();               // every 5 miles of driving
}

int progressNextLocked() {
    for (int v = 0; v < V_COUNT; v++) if (!(g_prog.owned & (1u << v))) return v;
    return -1;
}
bool progressCanBuy(int v) {
    if (v < 0 || v >= V_COUNT || (g_prog.owned & (1u << v))) return false;
    const VehicleSpec& S = vehicleSpec((uint8_t)v);
    return g_prog.wallet[S.priceVehicle] >= S.priceMiles;
}
bool progressBuy(int v) {
    if (!progressCanBuy(v)) return false;
    const VehicleSpec& S = vehicleSpec((uint8_t)v);
    g_prog.wallet[S.priceVehicle] -= S.priceMiles;
    g_prog.owned |= (uint8_t)(1u << v);
    g_prog.current = (uint8_t)v;
    progressSave();
    return true;
}
bool progressAllBiomes() { return (g_prog.visited & ((1u << B_COUNT) - 1)) == ((1u << B_COUNT) - 1); }

}
