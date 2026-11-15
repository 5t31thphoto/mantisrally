#include "display.h"
#include "render.h"
#include <M5Unified.h>
#include <string.h>
#include <stdlib.h>
#include "mem.h"
#ifndef HOST
#include <esp_heap_caps.h>
#include <esp_timer.h>
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
    const uint16_t* pal = framePalette565() + (((y + frameBob()) & 1) << 9);   // this row's two dither positions
    int src = (y + frameBob()) >> 1;
    if (src > FB_H - 1) src = FB_H - 1;
    const uint8_t* srow = fb + src * FB_W;
    for (int x = 0; x < DW; x++) out[x] = swap16(pal[((x & 1) << 8) + srow[x >> 1]]);
    if (s_ovOn && s_ovRow[y]) { const uint16_t* o = s_ov + y * DW; for (int x = 0; x < DW; x++) if (o[x]) out[x] = o[x]; }
}

// ---------------------------------------------------------------------------------------------------------
// The pipeline. The drawing core renders the 160 x 120 indexed frame; the screen core shows it at 320 x 240.
//
// Default ("strip streaming"): at present time the drawing core copies just the frame (19 KB), its palette,
// the bob offset and the overlay row flags into a packet in fast internal RAM (~0.1 ms) and goes straight back
// to drawing. The screen core doubles the packet 16 rows at a time (pixel pairs as single 32-bit writes) into two
// strip buffers in DMA-capable internal RAM, and DMAs each strip to the LCD in its own byte order while it fills
// the other, with the bus held for the whole frame. The LCD clock is MP_LCD_MHZ. Nothing passes through PSRAM; the
// doubling costs the drawing core nothing and hides behind the transfer on the screen core.
// The screen task outranks the sound on that core, so a strip is queued the moment the last one is done (the sound
// renders a 21 ms block in one go: below it, the transfer stalled behind every block). While a strip is on the wire
// the task sleeps on a microsecond timer set for the strip's end, so the sound has the core.
//
// -DMP_LEGACY_DISPLAY restores the previous pipeline exactly (whole-frame PSRAM canvases).
// Both produce byte-identical pixels (tools/host/displaytest.cpp checks this on every golden scene).
// ---------------------------------------------------------------------------------------------------------
static constexpr int STRIP = 16;
struct Packet { uint8_t fb[FB_W * FB_H]; uint16_t pal[4 * 256]; int bob; bool ovOn; uint8_t ovRow[DH]; };   // pal: 2x2 dither
static Packet* s_pk = nullptr;
static uint16_t* s_strip[2] = {nullptr, nullptr};    // two 320 x 16 strips in DMA-capable internal RAM
static bool s_stream = false;
#ifndef MP_LCD_MHZ
#define MP_LCD_MHZ 80                                // the LCD write clock: 80 MHz is the Core2's proven fast setting; -DMP_LCD_MHZ=40 for the stock clock
#endif
static constexpr int64_t STRIP_US = (int64_t)DW * STRIP * 16 / MP_LCD_MHZ;   // one strip on the wire (2048 us at 40 MHz)
static constexpr int64_t STRIP_EARLY_US = 30;        // wake this much before the strip's end (the timer's dispatch)
static constexpr int64_t STRIP_MIN_SLEEP_US = 80;    // less than this left: just wait it out
#ifndef HOST
static esp_timer_handle_t s_stripTimer = nullptr;
static SemaphoreHandle_t s_stripDone = nullptr;
static int64_t s_dmaEnd = 0;                         // when the strip on the wire is done (esp_timer clock)
static void stripTimerCb(void*) { xSemaphoreGive(s_stripDone); }
// Until the strip on the wire is done: asleep for nearly all of it (the core is the sound's), then the last few us.
static void stripWait() {
    if (s_stripTimer) {
        int64_t left = s_dmaEnd - esp_timer_get_time() - STRIP_EARLY_US;
        if (left > STRIP_MIN_SLEEP_US && esp_timer_start_once(s_stripTimer, (uint64_t)left) == ESP_OK)
            xSemaphoreTake(s_stripDone, portMAX_DELAY);
    }
    M5.Display.waitDMA();
}
#endif

static uint16_t* stripAlloc() {
#ifdef HOST
    return (uint16_t*)calloc(DW * STRIP, sizeof(uint16_t));
#else
    return (uint16_t*)heap_caps_malloc(DW * STRIP * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
#endif
}

// One screen row from a frame: the shared doubling (stream, legacy and compositeRow all agree on these pixels). Each
// picture pixel is a 2x2 block on the panel; each position in it takes its own dithered copy of the palette.
static inline void doubleRow(const uint8_t* fb, const uint16_t* pal, int bob, bool ovOn, const uint8_t* ovRow, int y, uint16_t* d) {
    int src = (y + bob) >> 1;
    if (src > FB_H - 1) src = FB_H - 1;
    const uint8_t* s = fb + src * FB_W;
    const uint16_t* pa = pal + (((y + bob) & 1) << 9), *pb = pa + 256;
    uint32_t* d32 = (uint32_t*)d;
    for (int x = 0; x < FB_W; x++) { const int c = s[x]; d32[x] = (uint32_t)pa[c] | ((uint32_t)pb[c] << 16); }   // both pixels at once
    if (ovOn && ovRow[y]) { const uint16_t* o = s_ov + y * DW; for (int x = 0; x < DW; x++) if (o[x]) d[x] = swap16(o[x]); }
}

// The strips of one packet, written by `emit(stripTopY, rows, pixels)`.
template <class Emit> static void streamPacket(const Packet& P, uint16_t* const* strips, Emit emit) {
    for (int y0 = 0, k = 0; y0 < DH; y0 += STRIP, k++) {
        uint16_t* d = strips[k & 1];
        if (!d) return;
        for (int r = 0; r < STRIP; r++) {
            int y = y0 + r;
            doubleRow(P.fb, P.pal, P.bob, P.ovOn, P.ovRow, y, d + r * DW);   // (rows of a pair differ: their own dither)
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
    if (s_dispIdle) { xSemaphoreTake(s_dispIdle, portMAX_DELAY); xSemaphoreGive(s_dispIdle); }   // woken the moment it's done
#endif
}

void displayHoldBus() {
#ifndef HOST
    if (s_dispIdle) xSemaphoreTake(s_dispIdle, portMAX_DELAY);
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
            stripWait();                                            // the strip before last is done with this buffer's twin
            M5.Display.pushImageDMA(0, y0, DW, STRIP, (const lgfx::swap565_t*)d);
            s_dmaEnd = esp_timer_get_time() + STRIP_US;             // (it started inside the call: this is never early)
        });
        stripWait();
        M5.Display.endWrite();
#endif
        xSemaphoreGive(s_dispIdle);
    }
}
#endif

bool displayStreaming() { return s_stream; }

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
#ifndef HOST
        s_stripDone = xSemaphoreCreateBinary();
        if (s_stripDone) {                                            // no timer: the strips are waited out as before
            esp_timer_create_args_t ta = {};
            ta.callback = stripTimerCb;
            ta.name = "lcdstrip";
            if (esp_timer_create(&ta, &s_stripTimer) != ESP_OK) s_stripTimer = nullptr;
        }
#endif
        // above the sound task (4) and the speaker driver (5) on core 0: it only fills strips (~0.1 ms each) and sleeps
        xTaskCreatePinnedToCore(displayTask, "lcd", 4096, nullptr, 6, &s_dispTask, 0);
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
    for (int y = 0; y < DH; y++) {
        int src = (y + bob) >> 1;
        if (src > FB_H - 1) src = FB_H - 1;
        uint16_t* d = dst + y * DW;
        const uint8_t* s = fb + src * FB_W;
        const uint16_t* pa = pal + (((y + bob) & 1) << 9), *pb = pa + 256;
        for (int x = 0; x < FB_W; x++) { d[2 * x] = pa[s[x]]; d[2 * x + 1] = pb[s[x]]; }
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
        xSemaphoreTake(s_dispIdle, portMAX_DELAY);                       // the screen core has finished the last frame
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
    xSemaphoreTake(s_dispIdle, portMAX_DELAY);
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
