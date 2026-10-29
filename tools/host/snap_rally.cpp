#define HOST 1
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <stdint.h>
#include "Arduino.h"
#include "M5Unified.h"
uint32_t esp_random() {
  static uint32_t x = 0xC0FFEEu;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return x;
}
M5Class M5;
M5Canvas* g_hostLastCanvas = nullptr;
static uint64_t g_us = 1000000;
uint32_t millis() { return (uint32_t)(g_us / 1000); }
uint32_t micros() { return (uint32_t)g_us; }
void delay(uint32_t ms) { g_us += (uint64_t)ms * 1000; }

#include "universe.h"
#include "journey.h"
#include "terrain.h"
#include "field.h"
#include "scene.h"
#include "render.h"
#include "databank.h"

using namespace mantis;

static void writePPM(const char* path) {
  const uint8_t* fb = frameBuffer();
  const uint8_t* pal = framePaletteRGB();
  FILE* f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  fprintf(f, "P6\n320 240\n255\n");
  for (int y = 0; y < 120; y++) {
    for (int row = 0; row < 2; row++) {
      for (int x = 0; x < 160; x++) {
        uint8_t idx = fb[y * 160 + x];
        const uint8_t* c = pal + idx * 3;
        for (int col = 0; col < 2; col++)
          fputc(c[0], f), fputc(c[1], f), fputc(c[2], f);
      }
    }
  }
  fclose(f);
  printf("wrote %s\n", path);
}

static void frameAt(float carX, float carZ, float yaw, float speed, float throttle,
                    float grip, float slip, bool hb, float tod) {
  setTimeOfDay(tod, millis() / 1000.f, 200.f);
  for (int i = 0; i < 50; i++) {
    g_us += 33333;
    float t = millis() / 1000.f;
    // do not call journeyUpdate — keeps forced biome
    terrainUpdateRoad(carX, carZ, yaw, speed, 1.f / 30.f);
    fieldUpdate(carX, carZ, yaw, speed, 1.f / 30.f);
    float y = terrainHeight(carX, carZ) + 0.4f;
    sceneUpdate(t, 1.f / 30.f, carX, y, carZ, yaw, 0.04f, 300.f + i, speed, throttle, grip, slip, hb);
  }
  float t = millis() / 1000.f;
  float y = terrainHeight(carX, carZ) + 0.4f;
  setTimeOfDay(tod, t, 300.f);
  sceneUpdate(t, 1.f / 30.f, carX, y, carZ, yaw, 0.04f, 400.f, speed, throttle, grip, slip, hb);
  renderFrame(carX, y, carZ, yaw, 0.04f, t);
}

static void shot(const char* name, uint8_t biome, float tod,
                 float speed, float throttle, float grip, float slip, bool hb,
                 float cloud, float rain, float fog, float storm) {
  journeyForceBiome(biome, (biome + 3) % 17, 0.25f);
  frameAt(5.f, 8.f, 0.35f, speed, throttle, grip, slip, hb, tod);
  // stamp weather after scene so it sticks for the final frame
  g_scene.cloud = cloud; g_scene.rain = rain; g_scene.fog = fog; g_scene.storm = storm;
  g_scene.dust = journeyBiome(biome).dust * 0.5f;
  float t = millis() / 1000.f;
  float y = terrainHeight(5.f, 8.f) + 0.4f;
  renderFrame(5.f, y, 8.f, 0.35f, 0.04f, t);
  char path[128];
  snprintf(path, sizeof path, "snaps/%s.ppm", name);
  writePPM(path);
  printf("  %s  biome=%s tod=%.2f cloud=%.2f rain=%.2f\n",
         name, journeyBiome(journeyCurrent()).name, g_scene.tod, g_scene.cloud, g_scene.rain);
}

int main() {
  (void)system("mkdir -p snaps");
  uint32_t seed = 0x52414C4Cu;
  universeInit(seed);
  terrainInit(seed);
  journeyInit(seed);
  fieldInit();
  sceneInit();
  renderInit();
  databankInit();

  shot("01_dawn_forest",      BM_FOREST_GRAVEL, 0.23f, 20.f, 0.55f, 0.72f, 0.2f, false, 0.25f, 0.05f, 0.15f, 0.f);
  shot("02_noon_desert_drift", BM_DESERT_TRACK,  0.48f, 34.f, 0.5f,  0.55f, 0.75f, true,  0.15f, 0.0f,  0.05f, 0.f);
  shot("03_sunset_alpine",    BM_ALPINE_SNOW,   0.72f, 16.f, 0.4f,  0.48f, 0.15f, false, 0.35f, 0.0f,  0.25f, 0.f);
  shot("04_storm_moor",       BM_STORM_MOOR,    0.55f, 22.f, 0.7f,  0.55f, 0.35f, false, 0.9f,  0.75f, 0.4f,  0.7f);
  g_scene.flash = 0.6f;
  shot("05_coast_tarmac",     BM_COAST_TARMAC,  0.40f, 30.f, 0.85f, 0.95f, 0.05f, false, 0.2f,  0.05f, 0.05f, 0.f);
  shot("06_redwood_dusk",     BM_REDWOOD_DARK,  0.70f, 14.f, 0.4f,  0.7f,  0.1f,  false, 0.45f, 0.15f, 0.35f, 0.f);
  shot("07_night_city",       BM_NIGHT_CITY,    0.92f, 28.f, 0.6f,  1.0f,  0.1f,  false, 0.3f,  0.1f,  0.1f,  0.f);
  shot("08_volcano_ash",      BM_VOLCANO_ASH,   0.35f, 20.f, 0.5f,  0.65f, 0.25f, false, 0.5f,  0.05f, 0.2f,  0.1f);

  printf("done.\n");
  return 0;
}
