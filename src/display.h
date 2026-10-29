#pragma once
#include <stdint.h>
namespace mantis {
void displayInit();
// Expand the 160x120 indexed frame through the palette into a 320x240 canvas and hand it to
// the core-0 push task; rendering of the next frame overlaps the LCD transfer.
void displayPresent();

// A full-resolution overlay drawn over the doubled world: 320 x 240 native RGB565, 0 = transparent.
uint16_t* overlayBuffer();
void overlayEnable(bool on);           // rescans which rows carry anything
bool overlayEnabled();
void overlayClear();
// Exactly what the screen shows on row y (world + overlay), native RGB565.
void compositeRow(int y, uint16_t* out320);
// While saving to SD (it shares the SPI bus with the LCD) hold the bus: waits for the LCD push to finish.
void displayHoldBus();
void displayReleaseBus();
// tests: the whole screen as each pipeline would send it (byte-swapped RGB565)
void displayDebugLegacy(uint16_t* out320x240);
void displayDebugStream(uint16_t* out320x240);
}
