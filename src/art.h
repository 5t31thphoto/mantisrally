#pragma once
#include <stdint.h>

// All the art (chase vehicles, cockpits, the store and photo-mode drawings, the MANTIS MOTORS signs) lives in
// flash deflate-compressed (art_packed.h, made by tools/sprites/pack_art.py) and is unpacked into PSRAM once at
// boot. The tables below point into that copy; every byte is exactly what the art tools made.

namespace mantis {
namespace art {

struct VehArt  { uint8_t w, h, frames; int8_t ax, ay; const uint8_t* pal; const uint8_t* px; };   // 4-bit chase frames
struct CockArt { const uint8_t* pal; const uint8_t* px; int8_t wx, wy, wr; };                       // 4-bit cockpits
struct SideArt { uint8_t w, h; const uint16_t* px; };                                               // RGB565, 0 = clear

extern VehArt  VEH_ART[4];
extern CockArt COCK_ART[4];
extern SideArt SIDE_ART[4];        // the store and photo mode: the side drawings
extern SideArt REAR_ART[4];        // photo mode: the rear drawings
extern const uint8_t* MM_SIGN;     // the arrow, pointing right / left (template bytes)
extern const uint8_t* MM_SIGN_L;
extern uint8_t MM_SIGN_W, MM_SIGN_H, MM_SIGN_AX, MM_SIGN_AXL;

bool artInit();                    // unpacks (once); false if it couldn't
bool artInflate(const uint8_t* z, uint32_t zn, uint8_t* out, uint32_t outN);   // zlib stream -> exactly outN bytes

}
}
