#pragma GCC optimize("Os")
#include "journey.h"
#include "universe.h"
#include <math.h>
#include <string.h>

namespace mantis {

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

#define C3(r, g, b) {r, g, b}
static const Biome BIOMES[B_COUNT] = {
  // name, sign, east, north,
  // surf, half, straight sweep tight hairpin, grade, crest, vRad, paved,
  // base, hills, ridge, cliff, water, rough, ground, snowLine,
  // pine broad redwood bush cactus snag boulder flowers,
  // cloud rain fog snow storm dust, fireflies aurora beams leaves,
  // grassD grassL soilD soilL rockD rockL leafD leafL accent water mtn, peaks
  {"Pine Forest", "PINES", 0.0f, 0.35f,
   SURF_GRAVEL, 3.6f, .30f, .50f, .40f, .05f, .09f, .50f, 70.f, 0,
   80.f, 22.f, 8.f, 0.f, .15f, .6f, MAT_GRASS, 900.f,
   .75f, .10f, 0.f, .20f, 0.f, .05f, .10f, .15f,
   .35f, .25f, .30f, 0.f, .10f, .10f, .4f, 0.f, .6f, 0.f,
   C3(22,48,30), C3(96,140,70), C3(60,44,30), C3(150,118,80), C3(50,52,58), C3(140,140,140),
   C3(12,34,28), C3(60,112,64), C3(220,170,210), C3(40,70,90), C3(70,80,112), .5f},
  {"Redwood Cathedral", "REDWOOD", 0.55f, 0.30f,
   SURF_TARMAC, 3.3f, .20f, .60f, .40f, 0.f, .07f, .20f, 150.f, 1,
   60.f, 18.f, 5.f, 0.f, 0.f, .4f, MAT_GRASS, 900.f,
   .25f, .05f, .60f, .40f, 0.f, .02f, .03f, .05f,
   .40f, .30f, .50f, 0.f, .05f, .05f, .2f, 0.f, 1.f, 0.f,
   C3(24,46,26), C3(90,130,60), C3(60,40,28), C3(140,96,62), C3(52,50,48), C3(130,124,110),
   C3(14,40,26), C3(70,120,60), C3(230,210,120), C3(40,70,80), C3(60,80,90), .3f},
  {"Lake Country", "LAKES", 0.45f, -0.10f,
   SURF_TARMAC, 3.5f, .40f, .60f, .20f, 0.f, .05f, .15f, 200.f, 1,
   20.f, 10.f, 2.f, 0.f, .75f, .3f, MAT_GRASS, 900.f,
   .35f, .25f, 0.f, .30f, 0.f, .02f, .05f, .30f,
   .35f, .20f, .30f, 0.f, .10f, 0.f, .5f, 0.f, .4f, 0.f,
   C3(30,60,32), C3(120,160,70), C3(70,56,40), C3(160,130,90), C3(60,60,64), C3(150,150,150),
   C3(16,40,30), C3(70,120,70), C3(240,220,120), C3(40,80,110), C3(90,90,140), .4f},
  {"Rain Moor", "MOOR", 0.80f, 0.55f,
   SURF_MUD, 3.2f, .40f, .50f, .30f, 0.f, .08f, .60f, 60.f, 0,
   50.f, 16.f, 4.f, 0.f, .30f, .8f, MAT_GRASS, 900.f,
   .15f, .02f, 0.f, .50f, 0.f, .10f, .25f, .05f,
   .80f, .70f, .60f, 0.f, .30f, 0.f, 0.f, 0.f, .1f, 0.f,
   C3(36,50,36), C3(110,130,80), C3(54,44,36), C3(120,100,80), C3(56,58,62), C3(130,132,136),
   C3(20,40,32), C3(70,100,70), C3(150,110,170), C3(50,60,70), C3(80,86,100), .35f},
  {"Devil's Pass", "DEVILS", -0.80f, 0.30f,
   SURF_TARMAC, 3.3f, .10f, .30f, .70f, .45f, .12f, .35f, 90.f, 1,
   150.f, 40.f, 35.f, 25.f, 0.f, .6f, MAT_GRASS, 900.f,
   .30f, 0.f, 0.f, .20f, 0.f, .05f, .30f, .05f,
   .50f, .30f, .35f, 0.f, .45f, .05f, .1f, 0.f, .3f, 0.f,
   C3(40,52,34), C3(120,130,80), C3(70,58,46), C3(150,130,100), C3(60,56,56), C3(160,150,140),
   C3(18,40,30), C3(70,110,70), C3(220,200,140), C3(40,60,80), C3(90,90,110), .9f},
  {"Alpine Summit", "SUMMIT", -0.55f, 0.80f,
   SURF_GRAVEL, 3.4f, .10f, .30f, .60f, .60f, .11f, .30f, 80.f, 0,
   260.f, 40.f, 40.f, 15.f, .10f, .5f, MAT_GRASS, 28.f,
   .25f, 0.f, 0.f, .10f, 0.f, .02f, .40f, .10f,
   .45f, .10f, .30f, .35f, .15f, 0.f, 0.f, .3f, .3f, 0.f,
   C3(40,62,40), C3(130,160,100), C3(80,70,60), C3(160,150,130), C3(70,72,80), C3(170,170,180),
   C3(18,42,36), C3(70,110,80), C3(200,200,240), C3(40,80,110), C3(110,120,150), 1.f},
  {"Red Canyon", "CANYON", -0.70f, -0.60f,
   SURF_DIRT, 3.6f, .30f, .50f, .40f, .10f, .09f, .60f, 55.f, 0,
   100.f, 15.f, 6.f, 30.f, 0.f, .7f, MAT_SAND, 900.f,
   0.f, 0.f, 0.f, .20f, .25f, .05f, .35f, 0.f,
   .15f, .05f, .05f, 0.f, .10f, .60f, 0.f, 0.f, .2f, 0.f,
   C3(120,70,40), C3(220,150,90), C3(110,60,36), C3(210,130,80), C3(110,50,34), C3(220,120,80),
   C3(40,60,30), C3(110,130,60), C3(240,200,90), C3(60,90,110), C3(150,90,80), .6f},
  {"Coast Highway", "COAST", 1.0f, 0.0f,
   SURF_TARMAC, 3.6f, .30f, .70f, .40f, .05f, .08f, .20f, 150.f, 1,
   12.f, 30.f, 10.f, 10.f, .60f, .4f, MAT_GRASS, 900.f,
   .20f, .10f, 0.f, .50f, 0.f, 0.f, .20f, .20f,
   .40f, .20f, .45f, 0.f, .15f, 0.f, 0.f, 0.f, .3f, 0.f,
   C3(40,64,36), C3(140,160,80), C3(90,76,56), C3(190,170,130), C3(70,66,62), C3(170,160,150),
   C3(20,46,32), C3(80,120,70), C3(240,240,200), C3(30,80,120), C3(90,110,140), .3f},
  {"Wildflower Meadow", "MEADOW", 0.0f, -0.30f,
   SURF_DIRT, 3.0f, .50f, .60f, .20f, 0.f, .05f, .40f, 70.f, 0,
   30.f, 8.f, 1.f, 0.f, .10f, .3f, MAT_GRASS, 900.f,
   .03f, .05f, 0.f, .10f, 0.f, 0.f, .03f, 1.f,
   .25f, .20f, .10f, 0.f, .15f, 0.f, .6f, 0.f, .2f, 0.f,
   C3(50,100,40), C3(150,210,70), C3(110,80,50), C3(200,160,100), C3(80,80,80), C3(170,170,160),
   C3(30,70,30), C3(100,160,60), C3(230,230,250), C3(50,100,140), C3(90,110,150), .25f},
  {"Autumn Ridge", "AUTUMN", -0.35f, 0.05f,
   SURF_GRAVEL, 3.3f, .20f, .50f, .50f, .15f, .09f, .45f, 75.f, 0,
   90.f, 30.f, 12.f, 0.f, .15f, .5f, MAT_GRASS, 900.f,
   .15f, .65f, 0.f, .30f, 0.f, .02f, .10f, .05f,
   .40f, .20f, .30f, 0.f, .10f, 0.f, 0.f, 0.f, .5f, 1.f,
   C3(70,60,30), C3(170,140,60), C3(80,50,30), C3(170,110,60), C3(64,58,54), C3(150,140,130),
   C3(110,40,16), C3(240,140,40), C3(200,40,30), C3(50,70,90), C3(110,80,100), .5f},
  {"Bayou Mud", "BAYOU", 0.60f, -0.70f,
   SURF_MUD, 3.2f, .40f, .50f, .30f, 0.f, .03f, .30f, 60.f, 0,
   5.f, 4.f, 0.f, 0.f, .85f, .5f, MAT_GRASS, 900.f,
   .05f, .30f, 0.f, .40f, 0.f, .40f, 0.f, .10f,
   .50f, .45f, .60f, 0.f, .25f, 0.f, 1.f, 0.f, .2f, 0.f,
   C3(40,56,24), C3(120,140,50), C3(50,40,24), C3(110,90,50), C3(50,50,40), C3(120,120,100),
   C3(30,50,24), C3(100,130,60), C3(200,230,120), C3(50,70,40), C3(70,90,80), .1f},
  {"Frost Tundra", "TUNDRA", 0.0f, 1.0f,
   SURF_SNOW, 3.4f, .50f, .50f, .30f, .05f, .07f, .40f, 70.f, 0,
   120.f, 14.f, 6.f, 0.f, .35f, .4f, MAT_SNOW, -900.f,
   .30f, 0.f, 0.f, 0.f, 0.f, .10f, .15f, 0.f,
   .50f, 0.f, .30f, .60f, .05f, 0.f, 0.f, 1.f, .2f, 0.f,
   C3(150,165,190), C3(240,245,255), C3(120,130,150), C3(200,210,225), C3(60,64,76), C3(150,156,170),
   C3(14,36,40), C3(60,100,90), C3(180,240,220), C3(60,90,120), C3(130,140,170), .7f},
};

const Biome& biome(uint8_t id) { return BIOMES[id % B_COUNT]; }

static uint32_t s_seed = 1;
static uint8_t  s_cur = 0, s_next = 0, s_dest = 0;
static float    s_blend = 0.f, s_dist = 0.f, s_len = BIOME_LEN;
static bool     s_trans = false, s_arrived = false, s_newBiome = false;
static uint16_t s_visited = 0;
static uint32_t s_routeSalt = 0;
static Land     s_land;
// base plane
static float s_b0 = 0.f, s_ax = 0.f, s_az = 0.f, s_gx = 0.f, s_gz = 0.f;

static constexpr float TRANS = 450.f;      // metres of blend between biomes

static inline float cdist(uint8_t a, uint8_t b) {
    float dx = BIOMES[a].east - BIOMES[b].east, dy = BIOMES[a].north - BIOMES[b].north;
    return sqrtf(dx * dx + dy * dy);
}

// One step across the climate map toward dest. Deterministic for a (cur, dest, salt).
static uint8_t stepToward(uint8_t cur, uint8_t dest, uint32_t salt) {
    if (cur == dest) return cur;
    float dCur = cdist(cur, dest);
    if (dCur <= 0.72f) return dest;
    int best = -1; float bestS = 1e9f;
    for (int b = 0; b < B_COUNT; b++) {
        if (b == cur) continue;
        float r = cdist(cur, (uint8_t)b);
        if (r > 0.78f) continue;
        float d = cdist((uint8_t)b, dest);
        if (d >= dCur - 0.05f) continue;                       // always make progress
        float s = d + 0.30f * hashf(salt * 31u + (uint32_t)b * 977u + s_seed);
        if (s < bestS) { bestS = s; best = b; }
    }
    return best < 0 ? dest : (uint8_t)best;
}

// One step across the climate map in a compass direction (free roaming).
static uint8_t stepHeading(uint8_t cur, float yaw, uint32_t salt) {
    float tx = BIOMES[cur].east + cosf(yaw) * 0.6f;          // east = +x
    float ty = BIOMES[cur].north - sinf(yaw) * 0.6f;         // north = -z
    tx = clampf(tx, -1.1f, 1.1f); ty = clampf(ty, -1.1f, 1.1f);
    int best = -1; float bestS = 1e9f;
    for (int b = 0; b < B_COUNT; b++) {
        if (b == cur) continue;
        if (cdist(cur, (uint8_t)b) > 0.95f) continue;
        float dx = BIOMES[b].east - tx, dy = BIOMES[b].north - ty;
        float s = sqrtf(dx * dx + dy * dy) + 0.25f * hashf(salt * 13u + (uint32_t)b * 389u + s_seed);
        if (s < bestS) { bestS = s; best = b; }
    }
    return best < 0 ? cur : (uint8_t)best;
}

static void blendLand() {
    const Biome& A = BIOMES[s_cur];
    const Biome& B = BIOMES[s_next];
    float t = s_blend, u = 1.f - t;
#define L(f) s_land.f = A.f * u + B.f * t
    L(hills); L(ridge); L(cliff); L(water); L(rough); L(snowLine);
    L(pine); L(broad); L(redwood); L(bush); L(cactus); L(snag); L(boulder); L(flowers);
    L(roadHalf); L(straight); L(sweep); L(tight); L(hairpin); L(grade); L(crest); L(vRadius);
#undef L
    s_land.surface = t < 0.5f ? A.surface : B.surface;
    s_land.ground = t < 0.5f ? A.ground : B.ground;
    s_land.paved = t < 0.5f ? A.paved : B.paved;
}

uint8_t journeyPickDestination(uint32_t salt, uint8_t avoid) {
    // Prefer somewhere not yet visited, not here, not `avoid`.
    uint8_t pick = 0; float best = -1.f;
    for (int b = 0; b < B_COUNT; b++) {
        if (b == s_cur || b == avoid) continue;
        float s = hashf(salt * 7919u + (uint32_t)b * 104729u + s_seed);
        if (!(s_visited & (1u << b))) s += 0.6f;
        float d = cdist(s_cur, (uint8_t)b);
        if (d < 0.5f) s -= 0.3f;                   // a destination should be a journey away
        if (s > best) { best = s; pick = (uint8_t)b; }
    }
    return pick;
}

void journeyInit(uint32_t seed) {
    s_seed = seed ? seed : 1;
    s_cur = (uint8_t)(hash32(s_seed) % B_COUNT);
    s_next = s_cur;
    s_blend = 0.f; s_dist = 0.f; s_trans = false;
    s_len = BIOME_LEN * (0.85f + 0.3f * hashf(s_seed + 3));
    s_routeSalt = hash32(s_seed + 77);
    s_visited |= (uint16_t)(1u << s_cur);
    s_dest = journeyPickDestination(s_seed + 5, 255);
    s_arrived = false; s_newBiome = false;
    s_b0 = BIOMES[s_cur].base; s_ax = s_az = 0.f; s_gx = s_gz = 0.f;
    blendLand();
}

void journeyUpdate(float dist, float yaw, bool onRoad) {
    if (dist < 0.f) dist = 0.f;
    s_dist += dist;
    if (!s_trans && s_dist > s_len - TRANS) {
        if (s_cur == s_dest) { s_dest = journeyPickDestination(s_routeSalt + 11, s_cur); }
        s_next = onRoad ? stepToward(s_cur, s_dest, s_routeSalt) : stepHeading(s_cur, yaw, s_routeSalt);
        if (s_next == s_cur) s_next = stepHeading(s_cur, yaw + 1.3f, s_routeSalt + 1);
        s_trans = true;
    }
    if (s_trans) {
        s_blend = clampf((s_dist - (s_len - TRANS)) / TRANS, 0.f, 1.f);
        if (s_blend >= 1.f) {
            s_cur = s_next; s_blend = 0.f; s_dist = 0.f; s_trans = false;
            s_routeSalt = hash32(s_routeSalt + 1);
            s_len = BIOME_LEN * (0.85f + 0.3f * hashf(s_routeSalt));
            s_visited |= (uint16_t)(1u << s_cur);
            s_newBiome = true;
            if (s_cur == s_dest) {
                s_arrived = true;
                s_dest = journeyPickDestination(s_routeSalt + 3, s_cur);
            }
        }
    }
    blendLand();
}

uint8_t journeyCurrent() { return s_cur; }
uint8_t journeyNext() { return s_next; }
float journeyBlend() { return s_blend; }
uint8_t journeyDestination() { return s_dest; }
uint16_t journeyVisited() { return s_visited; }
void journeySetVisited(uint16_t m) { s_visited = (uint16_t)(m | (1u << s_cur)); }
bool journeyArrivedEvent() { bool a = s_arrived; s_arrived = false; return a; }
bool journeyBiomeEvent() { bool a = s_newBiome; s_newBiome = false; return a; }
const Land& journeyLand() { return s_land; }

float journeyMilesTo(uint8_t dest) {
    float left = fmaxf(0.f, s_len - s_dist) / MILE;
    uint8_t c = s_trans ? s_next : s_cur;
    if (c == dest) return s_trans ? left : (s_cur == dest ? 0.f : left);
    uint32_t salt = s_routeSalt;
    int n = 0;
    while (c != dest && n < 12) { c = stepToward(c, dest, salt); salt = hash32(salt + 1); n++; }
    return left + (float)n * (BIOME_LEN / MILE) - (s_trans ? 0.f : 0.f);
}
float journeyMilesToDest() { return journeyMilesTo(s_dest); }

void journeyFollowRoad(uint8_t dest) {
    s_dest = dest % B_COUNT;
    if (!s_trans) return;
    // already blending: keep it (the land can't jump), the route picks up from the next biome
}

void journeyForce(uint8_t cur, uint8_t next, float blend) {
    s_cur = cur % B_COUNT; s_next = next % B_COUNT;
    s_blend = clampf(blend, 0.f, 1.f);
    s_trans = s_cur != s_next;
    s_dist = s_trans ? (s_len - TRANS) + s_blend * TRANS : 0.f;
    s_b0 = BIOMES[s_cur].base * (1.f - s_blend) + BIOMES[s_next].base * s_blend;
    s_gx = s_gz = 0.f;
    blendLand();
}

// ---- base plane ----
float journeyBase(float x, float z) { return s_b0 + s_gx * (x - s_ax) + s_gz * (z - s_az); }

void journeySetCar(float x, float z, float yaw, float dist) {
    float here = journeyBase(x, z);
    float target = BIOMES[s_cur].base * (1.f - s_blend) + BIOMES[s_next].base * s_blend;
    float s = clampf((target - here) / 700.f, -0.055f, 0.055f);
    float tgx = cosf(yaw) * s, tgz = sinf(yaw) * s;
    // tilt changes are rate-limited by distance travelled so neighbouring land always agrees
    float k = clampf(dist * 0.00008f / 0.01f, 0.f, 1.f) * 0.01f;
    float dx = tgx - s_gx, dz = tgz - s_gz;
    float m = sqrtf(dx * dx + dz * dz);
    if (m > k && m > 1e-6f) { dx *= k / m; dz *= k / m; }
    s_b0 = here; s_ax = x; s_az = z;
    s_gx += dx; s_gz += dz;
}

}
