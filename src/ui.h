#pragma once
#include <stdint.h>
// A small 5x7 pixel font (capitals, digits, a little punctuation, '*' is a star) drawn into the
// full-resolution overlay, with a soft drop shadow so it reads over any scene.
namespace mantis {
int  uiTextWidth(const char* s, int scale);
void uiText(uint16_t* buf, int bw, int bh, int x, int y, const char* s, uint16_t color, int scale, bool shadow = true);
inline uint16_t rgb565(int r, int g, int b) { uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); return v ? v : 0x0821; }
}
