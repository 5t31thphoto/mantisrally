#pragma GCC optimize("Os")
#include "storage.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef HOST
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include "display.h"
#endif

namespace mantis {

static bool s_ok = false;
static int s_nextPhoto = 1;

#ifndef HOST
static int scanMax(const char* dir) {
    int best = 0;
    File d = SD.open(dir);
    if (!d) return 0;
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
        int n = 0;
        const char* nm = e.name();
        const char* b = strrchr(nm, '/'); b = b ? b + 1 : nm;
        if (sscanf(b, "rally_%d.mg", &n) == 1 && n > best) best = n;
        e.close();
    }
    d.close();
    return best;
}
#endif

bool storageInit() {
#ifndef HOST
    displayHoldBus();                                   // the card shares the SPI bus with the LCD
    SPI.begin(GPIO_NUM_18, GPIO_NUM_38, GPIO_NUM_23, GPIO_NUM_4);       // Core2 pins, exactly as MantisPlanet
    s_ok = SD.begin(GPIO_NUM_4, SPI, 25000000);
    if (s_ok) {
        if (!SD.exists("/rally_saves")) SD.mkdir("/rally_saves");
        if (!SD.exists("/images")) SD.mkdir("/images");
        if (!SD.exists("/images/rally")) SD.mkdir("/images/rally");
        s_nextPhoto = scanMax("/images/rally") + 1;
    }
    displayReleaseBus();
#endif
    return s_ok;
}
bool storageOk() { return s_ok; }

// little checksum so a half-written file is never trusted
static uint32_t sum32(const uint8_t* p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }
struct SaveHead { uint32_t magic, size, sum; };
static const uint32_t SAVE_MAGIC = 0x5352414Du;          // "MARS"

bool storageSave(const void* data, size_t n) {
#ifndef HOST
    if (!s_ok) return false;
    displayHoldBus();
    bool good = false;
    File f = SD.open("/rally_saves/progress.tmp", FILE_WRITE);
    if (f) {
        SaveHead h = {SAVE_MAGIC, (uint32_t)n, sum32((const uint8_t*)data, n)};
        good = f.write((const uint8_t*)&h, sizeof h) == sizeof h && f.write((const uint8_t*)data, n) == n;
        f.close();
    }
    if (good) {                                         // keep the last one as the backup, then swap the new one in
        if (SD.exists("/rally_saves/progress.bak")) SD.remove("/rally_saves/progress.bak");
        if (SD.exists("/rally_saves/progress.bin")) SD.rename("/rally_saves/progress.bin", "/rally_saves/progress.bak");
        good = SD.rename("/rally_saves/progress.tmp", "/rally_saves/progress.bin");
    }
    displayReleaseBus();
    return good;
#else
    (void)data; (void)n; return false;
#endif
}

bool storageLoad(void* data, size_t n) {
#ifndef HOST
    if (!s_ok) return false;
    const char* paths[2] = {"/rally_saves/progress.bin", "/rally_saves/progress.bak"};
    bool good = false;
    displayHoldBus();
    for (int k = 0; k < 2 && !good; k++) {
        File f = SD.open(paths[k]);
        if (!f) continue;
        SaveHead h;
        if (f.read((uint8_t*)&h, sizeof h) == sizeof h && h.magic == SAVE_MAGIC && h.size == n &&
            f.read((uint8_t*)data, n) == (int)n && sum32((const uint8_t*)data, n) == h.sum) good = true;
        f.close();
    }
    displayReleaseBus();
    return good;
#else
    (void)data; (void)n; return false;
#endif
}

int storagePhoto(void (*row)(int y, uint16_t* out)) {
#ifndef HOST
    if (!s_ok) return -1;
    char p[48]; snprintf(p, sizeof p, "/images/rally/rally_%04d.mg", s_nextPhoto);
    static uint16_t line[320];
    static uint8_t bytes[640];
    displayHoldBus();
    File f = SD.open(p, FILE_WRITE);
    if (!f) { displayReleaseBus(); return -1; }
    const uint8_t head[8] = {'M', 'G', '0', '1', 320 & 255, 320 >> 8, 240 & 255, 240 >> 8};
    f.write(head, 8);
    for (int y = 0; y < 240; y++) {
        row(y, line);
        for (int x = 0; x < 320; x++) { bytes[x * 2] = (uint8_t)(line[x] & 255); bytes[x * 2 + 1] = (uint8_t)(line[x] >> 8); }
        f.write(bytes, 640);
    }
    f.close();
    displayReleaseBus();
    return s_nextPhoto++;
#else
    // the desktop build writes the same file to ./images/rally (tests, screenshots)
    (void)!system("mkdir -p images/rally");
    char p[64]; snprintf(p, sizeof p, "images/rally/rally_%04d.mg", s_nextPhoto);
    FILE* f = fopen(p, "wb");
    if (!f) return -1;
    const uint8_t head[8] = {'M', 'G', '0', '1', 320 & 255, 320 >> 8, 240 & 255, 240 >> 8};
    fwrite(head, 1, 8, f);
    static uint16_t line[320];
    for (int y = 0; y < 240; y++) { row(y, line); fwrite(line, 2, 320, f); }
    fclose(f);
    return s_nextPhoto++;
#endif
}

}
