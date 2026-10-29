#pragma once
#include <stdint.h>

namespace mantis {

// Internal framebuffer: 160x120, 8-bit palette indices. Scaled 2x to the 320x240 panel.
constexpr int FB_W = 160;
constexpr int FB_H = 120;

// What the instruments show (written by the main loop each frame).
struct HudInfo {
    float throttle;             // 0..1
    float scanT;                // 0 idle, 0..1 reading, >1 done (fading)
    int scanX, scanY;           // panel pixels of the thing being read
    const char* banner;         // a place name arriving, or a message
    float bannerA;              // 0..1
    float hintA;                // the controls hint, fading out after the start
    bool showFps; int fps;
    bool muteIcon; bool muted; float muteA;
};
extern HudInfo g_hud;

void renderInit();
void renderFrame(float x, float y, float z, float yaw, float pitch, float timeSec);
// A world point on the panel (320x240 pixels). false if behind you / off screen. dist = metres.
bool renderProject(float wx, float wy, float wz, int& px, int& py, float& dist);
float renderFov();

const uint8_t*  frameBuffer();       // FB_W*FB_H indices
const uint16_t* framePalette565();   // 256 entries, byte-swapped RGB565 (panel order)
const uint8_t*  framePaletteRGB();   // 256*3 RGB888 (host previews)
int             frameBob();          // vertical bob in panel pixels

}
