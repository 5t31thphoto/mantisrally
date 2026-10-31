// Showcase stills from the real game code: tools/host/snap -> snaps/*.ppm (+ sheet via render.py)
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "Arduino.h"
#include "M5Unified.h"
#include "universe.h"
#include "journey.h"
#include "terrain.h"
#include "scene.h"
#include "field.h"
#include "vehicle.h"
#include "render.h"
using namespace mantis;
uint32_t esp_random() { static uint32_t x = 0xC0FFEEu; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
M5Class M5; M5Canvas* g_hostLastCanvas = nullptr;
static uint64_t g_us = 1000000;
uint32_t millis() { return (uint32_t)(g_us / 1000); }
uint32_t micros() { return (uint32_t)g_us; }
void delay(uint32_t ms) { g_us += (uint64_t)ms * 1000; }

static void ppm(const char* path) {
  const uint8_t* fb = frameBuffer(); const uint8_t* pal = framePaletteRGB();
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n320 240\n255\n");
  for (int y = 0; y < 120; y++) for (int r = 0; r < 2; r++) for (int x = 0; x < 160; x++) { const uint8_t* c = pal + fb[y * 160 + x] * 3; for (int k = 0; k < 2; k++) fwrite(c, 1, 3, f); }
  fclose(f);
}
static float wrapA(float a){ while(a>3.14159f)a-=6.28318f; while(a<-3.14159f)a+=6.28318f; return a;}
// drive along the road a while with an autopilot, then shoot
static void shot(const char* name, uint8_t b, float tod, uint8_t veh, uint8_t view, float cloud, float rain, float fog, float storm, float snow, float drive, float steerBias = 0.f) {
  universeInit(0x52414C4Cu + b * 7); journeyInit(0x52414C4Cu + b * 7); journeyForce(b, b, 0.f);
  terrainInit(0x52414C4Cu + b * 7); sceneInit(7); fieldInit(7);
  VehicleState s; RoadHit h; roadNearest(0, 0, h); vehicleReset(s, veh, h.across * 0.f, 0, h.yaw);
  float t = 0;
  setTimeOfDay(tod, 0, 0);
  for (int i = 0; i < (int)(drive * 30); i++) {
    RoadHit rh; float st = 0;
    if (roadNearest(s.x, s.z, rh, 500)) {
      int dir = cosf(s.yaw - rh.yaw) >= 0 ? 1 : -1; int n = rh.arm ? roadArmCount() : roadCount();
      int k = rh.idx + dir * (int)((10 + s.speed * 0.6f) / 3.f); k = k < 0 ? 0 : (k > n - 1 ? n - 1 : k);
      const RoadNode& p = rh.arm ? roadArmNode(k) : roadNode(k);
      st = wrapA(atan2f(p.z - s.z, p.x - s.x) - s.yaw) * 2.5f + steerBias; st = st > 1 ? 1 : (st < -1 ? -1 : st);
    }
    VehicleInput in{st, s.speed < 22.f ? 0.7f : 0.25f, 0, false, 0};
    vehicleStep(s, veh, in, 1 / 30.f); terrainUpdate(s.x, s.z, s.yaw, s.u, 1 / 30.f);
    t += 1 / 30.f; setTimeOfDay(tod, t, 0);
    sceneUpdate(t, 1 / 30.f, s.x, s.z, s.yaw, s.speed, 0);
    fieldUpdate(s, veh, 1 / 30.f);
  }
  sceneForceWeather(cloud, rain, fog, storm, snow);
  sceneUpdate(t, 1 / 30.f, s.x, s.z, s.yaw, s.speed, 0);
  if (storm > 0.5f) { g_scene.boltAge = 0.05f; g_scene.boltAz = s.yaw + 0.3f; g_scene.boltSeed = 1234; }
  Ground g; terrainGround(s.x, s.z, g);
  g_view.view = view; g_view.vehicle = veh; g_view.x = s.x; g_view.y = s.y; g_view.z = s.z; g_view.yaw = s.yaw;
  g_view.groundY = g.h + (g.mat == MAT_WATER ? g.water : 0.f); g_view.pitch = s.pitch; g_view.roll = s.roll; g_view.lean = s.lean;
  g_view.steer = s.steer * 3.f; g_view.speedMph = s.speed * 2.237f; g_view.offroadLeft = -1; g_view.msg = nullptr;
  renderFrame();
  char p[128]; snprintf(p, sizeof p, "snaps/%s.ppm", name); ppm(p); printf("%s\n", name);
}
int main() {
  (void)system("mkdir -p snaps");
  renderInit();
  shot("01_pines_dawn_cockpit", B_PINE, 0.27f, V_STI, VIEW_COCKPIT, 0.3f, 0, 0.2f, 0, 0, 8);
  shot("02_redwood_morning_eye", B_REDWOOD, 0.33f, V_STI, VIEW_EYE, 0.35f, 0, 0.3f, 0, 0, 6);
  shot("03_lakes_sunset_chase", B_LAKES, 0.72f, V_VETTE, VIEW_CHASE, 0.35f, 0, 0.1f, 0, 0, 10);
  shot("04_moor_rain_cockpit", B_MOOR, 0.45f, V_JEEP, VIEW_COCKPIT, 0.9f, 0.7f, 0.5f, 0.2f, 0, 8);
  shot("05_devils_storm_chase", B_DEVILS, 0.5f, V_STI, VIEW_CHASE, 0.95f, 0.5f, 0.3f, 0.8f, 0, 10);
  shot("06_alpine_noon_eye", B_ALPINE, 0.5f, V_JEEP, VIEW_EYE, 0.25f, 0, 0.1f, 0, 0, 8);
  shot("07_canyon_dusk_chase", B_CANYON, 0.76f, V_BIKE, VIEW_CHASE, 0.2f, 0, 0.05f, 0, 0, 8, 0.3f);
  shot("08_meadow_noon_chase", B_MEADOW, 0.48f, V_JEEP, VIEW_CHASE, 0.3f, 0, 0.05f, 0, 0, 8);
  shot("09_tundra_night_cockpit", B_TUNDRA, 0.95f, V_VETTE, VIEW_COCKPIT, 0.15f, 0, 0.1f, 0, 0.2f, 8);
  shot("10_bayou_dusk_eye", B_BAYOU, 0.8f, V_BIKE, VIEW_COCKPIT, 0.4f, 0, 0.5f, 0, 0, 8);
  shot("11_autumn_morning_chase", B_AUTUMN, 0.36f, V_STI, VIEW_CHASE, 0.3f, 0, 0.15f, 0, 0, 8);
  shot("12_coast_noon_cockpit", B_COAST, 0.55f, V_VETTE, VIEW_COCKPIT, 0.3f, 0, 0.2f, 0, 0, 8);
  return 0;
}
