#pragma once
#include <stdint.h>

// Pixel art made at boot (no bitmaps in flash): vegetation and props as lit templates, the deer, the dealership,
// the four vehicles seen from behind, and a 3x5 font for the instruments.

namespace mantis {

// Palette layout shared by the renderer and the art
enum : uint8_t {
    P_SKY = 0, P_SUN = 16, P_SPECIAL = 24, P_CLOUD = 32, P_MTN = 48, P_HAZE = 64,
    P_GRASS = 80, P_SOIL = 96, P_ROCK = 112, P_ROAD = 128, P_LEAF = 144,
    P_LEAF2 = 160, P_SNOW = 168, P_BARK = 176, P_WATER = 184,
    P_VEH = 192,        // 1..15: the current vehicle's colours (from the sprite sheet)
    P_COCK = 208,       // 1..15: its cockpit's colours
    P_FLOWER = 224,
    // emissive (fixed) colours
    P_HUD_W = 232, P_HUD_A, P_HUD_R, P_HUD_G, P_HUD_K, P_FIREFLY, P_WINDOW, P_BEAM,
    // lit paint: 4-step ramps
    P_LINE_W = 240, P_LINE_Y = 244,
    P_SIGN_G = 248, P_SIGN_W, P_TAIL_D, P_TAIL, P_AMBER, P_BLACK, P_BOLT, P_MUD
};

// Template pixel: high nibble material, low nibble shade (0..15). Material 0 = clear.
enum : uint8_t { TM_NONE = 0, TM_LEAF, TM_LEAF2, TM_BARK, TM_ROCK, TM_SNOW, TM_FLOWER, TM_WALL, TM_ROOF, TM_GLASS, TM_SIGN, TM_SIGNTXT, TM_WHITE, TM_SIGNY, TM_INK };
struct Tpl { uint8_t w, h; uint8_t* px; int8_t ax; };   // ax: anchor column (base)

enum : uint8_t { T_PINE0 = 0, T_PINE1, T_PINE2, T_PINESNOW, T_BROAD0, T_BROAD1, T_REDWOOD, T_BUSH, T_CACTUS, T_SNAG, T_BOULDER,
                 T_FLOWERS, T_STORE, T_DEER0, T_DEER1, T_DEER2, T_DEER3, T_MMSIGN, T_MMSIGN_L, T_TPL_N };
const Tpl& tpl(uint8_t id);


void spritesInit();

// 3x5 font into an indexed buffer (dark outline). Returns the width drawn.
int  font3Width(const char* s, int scale = 1);
int  font3(uint8_t* fb, int W, int H, int x, int y, const char* s, uint8_t c, uint8_t outline, int scale = 1);

}
