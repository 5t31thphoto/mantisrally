#pragma GCC optimize("Os")
#include "databank.h"
#include <string.h>

namespace mantis {

static float s_best[17];
static uint8_t s_scanned[32];
static int s_scanN = 0;

static const ScanCard CARDS[] = {
    { 0, "Scots Pine",   "Tall roadside conifer.", "Common on forest stages." },
    { 1, "Granite Outcrop", "Broken rock, low grip.", "Watch the outside line." },
    { 2, "Pace Note Board", "Crew left a warning.", "Hairpin in 200 m." },
    { 3, "Spectator",    "Cheering from the bank.", "Don't hit them." },
    { 4, "Hay Bale",     "Soft barrier.", "Better than a tree." },
    { 5, "Armco",        "Hard metal rail.", "Will stop you cold." },
    { 10, "Stage Sign",   "Points to a biome.", "Commit to the fork." },
};

void databankInit() {
    for (int i = 0; i < 17; i++) s_best[i] = 0.f;
    s_scanN = 0;
    memset(s_scanned, 0, sizeof(s_scanned));
}

void databankSave() { /* flash stub — real device writes NVS */ }
void databankLoad() { /* flash stub */ }

void databankRecordStage(uint8_t stage, float timeSec) {
    stage %= 17;
    if (s_best[stage] <= 0.f || timeSec < s_best[stage]) s_best[stage] = timeSec;
}

float databankBestTime(uint8_t stage) { return s_best[stage % 17]; }

void databankScan(uint8_t species) {
    for (int i = 0; i < s_scanN; i++) if (s_scanned[i] == species) return;
    if (s_scanN < 32) s_scanned[s_scanN++] = species;
}

bool databankHas(uint8_t species) {
    for (int i = 0; i < s_scanN; i++) if (s_scanned[i] == species) return true;
    return false;
}

const ScanCard* databankCard(uint8_t species) {
    for (size_t i = 0; i < sizeof(CARDS)/sizeof(CARDS[0]); i++)
        if (CARDS[i].species == species) return &CARDS[i];
    return &CARDS[0];
}

int databankScanCount() { return s_scanN; }

}
