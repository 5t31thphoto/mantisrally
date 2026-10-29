#include "display.h"
#include "render.h"
#include <M5Unified.h>
#include <string.h>
#include <stdlib.h>
#include "mem.h"
#ifndef HOST
#include <esp_heap_caps.h>
#endif

namespace mantis {

static constexpr int DW = FB_W * 2, DH = FB_H * 2;

static M5Canvas s_fbA(&M5.Display), s_fbB(&M5.Display);
static M5Canvas* s_fb[2] = {&s_fbA, &s_fbB};
static int s_cur = 0;
static bool s_double = false;
static TaskHandle_t s_dispTask = nullptr;
static SemaphoreHandle_t s_dispIdle = nullptr;
static volatile int s_pushIdx = 0;
static uint16_t* s_ov = nullptr;
static bool s_ovOn = false;
static uint8_t s_ovRow[DH];
static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static void displayWaitIdle();
uint16_t* overlayBuffer() {
    displayWaitIdle();                                  // the screen core may still be reading it
    if (!s_ov) {
#ifdef HOST
        s_ov = (uint16_t*)calloc(DW * DH, sizeof(uint16_t));
#else
        s_ov = (uint16_t*)heap_caps_calloc(DW * DH, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_ov) s_ov = (uint16_t*)calloc(DW * DH, sizeof(uint16_t));
#endif
    }
    return s_ov;
}
void overlayClear() { if (overlayBuffer()) memset(s_ov, 0, DW * DH * sizeof(uint16_t)); }   // (overlayBuffer waits)
void overlayEnable(bool on) {
    s_ovOn = on && overlayBuffer();
    if (!s_ovOn) return;
    for (int y = 0; y < DH; y++) { s_ovRow[y] = 0; for (int x = 0; x < DW; x++) if (s_ov[y * DW + x]) { s_ovRow[y] = 1; break; } }
}
bool overlayEnabled() { return s_ovOn; }

void compositeRow(int y, uint16_t* out) {
    const uint8_t* fb = frameBuffer();
    const uint16_t* pal = framePalette565();
    int src = (y + frameBob()) >> 1;
    if (src > FB_H - 1) src = FB_H - 1;
    const uint8_t* srow = fb + src * FB_W;
    for (int x = 0; x < DW; x++) out[x] = swap16(pal[srow[x >> 1]]);
    if (s_ovOn && s_ovRow[y]) { const uint16_t* o = s_ov + y * DW; for (int x = 0; x < DW; x++) if (o[x]) out[x] = o[x]; }
}

// ---------------------------------------------------------------------------------------------------------
// The pipeline. The drawing core renders the 160 x 120 indexed frame; the screen core shows it at 320 x 240.
//
// Default ("strip streaming"): at present time the drawing core copies just the frame (19 KB), its palette,
// the bob offset and the overlay row flags into a packet in fast internal RAM (~0.1 ms) and goes straight back
// to drawing. The screen core doubles the packet 16 rows at a time (pixel pairs as single 32-bit writes) into two
// strip buffers in DMA-capable internal RAM, and DMAs each strip to the LCD in its own byte order while it fills
// the other, with the bus held for the whole frame. The LCD clock is 80 MHz (MP_LCD_MHZ). Nothing passes through
// PSRAM; the doubling costs the drawing core nothing and hides behind the transfer on the screen core.
//
// -DMP_LEGACY_DISPLAY restores the previous pipeline exactly (whole-frame PSRAM canvases).
// Both produce byte-identical pixels (tools/host/displaytest.cpp checks this on every golden scene).
// ---------------------------------------------------------------------------------------------------------
static constexpr int STRIP = 16;
struct Packet { uint8_t fb[FB_W * FB_H]; uint16_t pal[256]; int bob; bool ovOn; uint8_t ovRow[DH]; };
static Packet* s_pk = nullptr;
static uint16_t* s_strip[2] = {nullptr, nullptr};    // two 320 x 16 strips in DMA-capable internal RAM
static bool s_stream = false;
#ifndef MP_LCD_MHZ
#define MP_LCD_MHZ 80                                // the LCD write clock: 80 MHz is the Core2's proven fast setting; -DMP_LCD_MHZ=40 for the stock clock
#endif

static uint16_t* stripAlloc() {
#ifdef HOST
    return (uint16_t*)calloc(DW * STRIP, sizeof(uint16_t));
#else
    return (uint16_t*)heap_caps_malloc(DW * STRIP * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
#endif
}

// One screen row from a frame: the shared doubling (stream, legacy and compositeRow all agree on these pixels).
static inline void doubleRow(const uint8_t* fb, const uint16_t* pal, int bob, bool ovOn, const uint8_t* ovRow, int y, uint16_t* d) {
    int src = (y + bob) >> 1;
    if (src > FB_H - 1) src = FB_H - 1;
    const uint8_t* s = fb + src * FB_W;
    uint32_t* d32 = (uint32_t*)d;
    for (int x = 0; x < FB_W; x++) { uint32_t v = pal[s[x]]; d32[x] = v | (v << 16); }   // two identical pixels at once
    if (ovOn && ovRow[y]) { const uint16_t* o = s_ov + y * DW; for (int x = 0; x < DW; x++) if (o[x]) d[x] = swap16(o[x]); }
}
static inline int srcRow(int y, int bob) { int src = (y + bob) >> 1; return src > FB_H - 1 ? FB_H - 1 : src; }

// The strips of one packet, written by `emit(stripTopY, rows, pixels)`.
template <class Emit> static void streamPacket(const Packet& P, uint16_t* const* strips, Emit emit) {
    for (int y0 = 0, k = 0; y0 < DH; y0 += STRIP, k++) {
        uint16_t* d = strips[k & 1];
        if (!d) return;
        for (int r = 0; r < STRIP; r++) {
            int y = y0 + r;
            uint16_t* row = d + r * DW;
            bool ov = P.ovOn && P.ovRow[y];
            if (r > 0 && !ov && !(P.ovOn && P.ovRow[y - 1]) && srcRow(y, P.bob) == srcRow(y - 1, P.bob)) { memcpy(row, row - DW, DW * sizeof(uint16_t)); continue; }
            doubleRow(P.fb, P.pal, P.bob, P.ovOn, P.ovRow, y, row);
        }
        emit(y0, d);
    }
}

static void fillPacket() {
    memcpy(s_pk->fb, frameBuffer(), sizeof s_pk->fb);
    memcpy(s_pk->pal, framePalette565(), sizeof s_pk->pal);
    s_pk->bob = frameBob();
    s_pk->ovOn = s_ovOn;
    memcpy(s_pk->ovRow, s_ovRow, sizeof s_pk->ovRow);
}

// Wait until the screen core has finished with the current packet (and the overlay it reads).
static void displayWaitIdle() {
#ifndef HOST
    if (s_dispIdle) { while (xSemaphoreTake(s_dispIdle, 0) != pdTRUE) vTaskDelay(1); xSemaphoreGive(s_dispIdle); }
#endif
}

void displayHoldBus() {
#ifndef HOST
    if (s_dispIdle) while (xSemaphoreTake(s_dispIdle, 0) != pdTRUE) vTaskDelay(1);
#endif
}
void displayReleaseBus() {
#ifndef HOST
    if (s_dispIdle) xSemaphoreGive(s_dispIdle);
#endif
}

#ifdef MP_LEGACY_DISPLAY
static void displayTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_fb[s_pushIdx]->pushSprite(&M5.Display, 0, 0);
        xSemaphoreGive(s_dispIdle);
    }
}
#else
// Double-buffered DMA: while one strip is on its way to the LCD, the CPU fills the other. The bus is held for
// the whole frame (no per-strip transaction setup). Strips are byte-swapped RGB565 = the panel's own order.
static void displayTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#ifndef HOST
        M5.Display.startWrite();
        streamPacket(*s_pk, s_strip, [](int y0, uint16_t* d) {
            M5.Display.waitDMA();                                   // the strip before last is done with this buffer's twin
            M5.Display.pushImageDMA(0, y0, DW, STRIP, (const lgfx::swap565_t*)d);
        });
        M5.Display.waitDMA();
        M5.Display.endWrite();
#endif
        xSemaphoreGive(s_dispIdle);
    }
}
#endif

void displayInit() {
#ifndef MP_LEGACY_DISPLAY
#ifndef HOST
    if (auto* panel = M5.Display.getPanel()) {                        // the LCD write clock
        if (auto* bus = (lgfx::Bus_SPI*)panel->getBus()) {
            auto cfg = bus->config();
            cfg.freq_write = MP_LCD_MHZ * 1000000;
            bus->config(cfg);
            bus->init();
        }
    }
#endif
    s_strip[0] = stripAlloc(); s_strip[1] = stripAlloc();
    bool strips = s_strip[0] && s_strip[1];
    s_pk = (Packet*)memFast(sizeof(Packet));
    s_stream = strips && s_pk;
    if (s_stream) {
        s_dispIdle = xSemaphoreCreateBinary();
        xSemaphoreGive(s_dispIdle);
        xTaskCreatePinnedToCore(displayTask, "lcd", 4096, nullptr, 2, &s_dispTask, 0);
        return;
    }
    // could not get the strip buffers: fall back to the canvas pipeline below
#endif
    for (int i = 0; i < 2; i++) { s_fb[i]->setColorDepth(16); s_fb[i]->setPsram(true); }
    s_fb[0]->createSprite(DW, DH);
    s_double = s_fb[1]->createSprite(DW, DH) != nullptr;
    if (s_double) {
        s_dispIdle = xSemaphoreCreateBinary();
        xSemaphoreGive(s_dispIdle);
#ifdef MP_LEGACY_DISPLAY
        xTaskCreatePinnedToCore(displayTask, "lcd", 4096, nullptr, 2, &s_dispTask, 0);
#else
        s_double = false;                          // (the stream task isn't running: present synchronously)
#endif
    }
}

// The canvas pipeline: 2x nearest upscale into a whole-frame canvas (byte-swapped RGB565, same as the palette).
static void expand(M5Canvas* c) {
    uint16_t* dst = (uint16_t*)c->getBuffer();
    if (!dst) return;
    const uint8_t* fb = frameBuffer();
    const uint16_t* pal = framePalette565();
    const int bob = frameBob();
    int prevSrc = -1;
    for (int y = 0; y < DH; y++) {
        int src = (y + bob) >> 1;
        if (src > FB_H - 1) src = FB_H - 1;
        uint16_t* d = dst + y * DW;
        if (src == prevSrc) { memcpy(d, d - DW, DW * sizeof(uint16_t)); continue; }
        const uint8_t* s = fb + src * FB_W;
        for (int x = 0; x < FB_W; x++) {
            uint16_t v = pal[s[x]];
            d[2 * x] = v;
            d[2 * x + 1] = v;
        }
        prevSrc = src;
    }
    if (s_ovOn) {                                       // the overlay at full resolution
        for (int y = 0; y < DH; y++) {
            if (!s_ovRow[y]) continue;
            const uint16_t* o = s_ov + y * DW;
            uint16_t* d = dst + y * DW;
            for (int x = 0; x < DW; x++) if (o[x]) d[x] = swap16(o[x]);
        }
    }
}

void displayPresent() {
    if (s_stream) {
        while (xSemaphoreTake(s_dispIdle, 0) != pdTRUE) vTaskDelay(1);   // the screen core has finished the last frame
        fillPacket();
        xTaskNotifyGive(s_dispTask);
        return;
    }
    if (!s_double) {
        expand(s_fb[0]);
        s_fb[0]->pushSprite(&M5.Display, 0, 0);
        return;
    }
    // wait for the LCD to finish the previous frame
    while (xSemaphoreTake(s_dispIdle, 0) != pdTRUE) vTaskDelay(1);
    expand(s_fb[s_cur]);
    s_pushIdx = s_cur;
    xTaskNotifyGive(s_dispTask);
    s_cur ^= 1;
}

// ---- tests: both pipelines into a whole 320 x 240 screen (the pixels as sent to the LCD: byte-swapped) ----
void displayDebugLegacy(uint16_t* out) {
    static M5Canvas c(&M5.Display);
    static bool made = false;
    if (!made) { c.setColorDepth(16); c.createSprite(DW, DH); made = true; }
    expand(&c);
    memcpy(out, c.getBuffer(), DW * DH * sizeof(uint16_t));
}
void displayDebugStream(uint16_t* out) {
    static Packet* pk = nullptr;
    static uint16_t* st[2] = {nullptr, nullptr};
    if (!pk) { pk = (Packet*)memFast(sizeof(Packet)); st[0] = stripAlloc(); st[1] = stripAlloc(); }
    Packet* save = s_pk; s_pk = pk; fillPacket(); s_pk = save;
    streamPacket(*pk, st, [out](int y0, uint16_t* d) { memcpy(out + y0 * DW, d, DW * STRIP * sizeof(uint16_t)); });
}

}
