#pragma once
#include <stdint.h>

namespace mantis {

// 160x120 palette-indexed frame, doubled to the 320x240 panel by display.cpp.
constexpr int FB_W = 160;
constexpr int FB_H = 120;

enum : uint8_t { VIEW_COCKPIT = 0, VIEW_EYE, VIEW_CHASE, VIEW_N };

// Everything the renderer needs from the game this frame.
struct ViewInfo {
    uint8_t view, vehicle;
    // the car
    float x, y, z, yaw;            // y: CG height
    float groundY;                 // ground under the car
    float pitch, roll;             // body attitude (rad): nose up +, right side down +
    float lean;                    // bike lean (rad, right +)
    float steer;                   // -1..1 (wheel turned)
    float speedMph, rpm01;
    bool brake, reverse;
    float bounce;                  // suspension heave for the camera
    // hud
    float offroadLeft;             // seconds before the recovery, < 0 hidden
    const char* msg; float msgA;   // centre message
    const char* sub;               // small line under it
    int paceDir, paceSev; float paceA; bool paceCrest;
    const char* juncL; const char* juncR; float juncA;
    float storeA;                  // "MANTIS MOTORS AHEAD"
    bool showFps; int fps;
    // photo mode: the camera is placed by photo.cpp; no HUD, no car sprite, no cockpit
    bool photo; float pX, pY, pZ, pYaw, pPitch;
};
extern ViewInfo g_view;

void renderInit();
void renderSetVehicle(uint8_t v);               // loads its colours / cockpit
void renderFrame();
// a world point on the framebuffer (160x120). false if behind the camera.
bool renderProject(float wx, float wy, float wz, float& sx, float& sy, float& depth);
void renderSplat(float strength);               // mud on the glass / lens
void renderWater(float strength);               // spray on the glass
void renderDeerSplat();                         // light-red specks on the glass, then one slow wipe
bool renderWipeEvent(bool& deerWipe);           // a wiper sweep just started (for its sound)

// Photo mode: hero art (native RGB565, 0 = clear) set into the full-resolution overlay where the car stands, lit by
// the scene, behind anything nearer the camera, with its shadow on the ground. Returns false if it isn't in view.
struct PhotoArt { const uint16_t* px; int w, h; bool mirror; float x, y, z; float shadowW; };
bool renderPhotoCompose(const PhotoArt& a);
void renderPhotoClear();
const uint8_t*  frameBuffer();
bool            renderDepthFast();       // the depth buffer got internal RAM
extern const char* g_memNote;          // shown under the FPS readout
const uint16_t* framePalette565();
const uint8_t*  framePaletteRGB();
int             frameBob();

}
