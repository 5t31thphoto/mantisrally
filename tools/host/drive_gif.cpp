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
  x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x;
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
  if (!f) return;
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
}

int main() {
  (void)system("mkdir -p snaps/drive");
  uint32_t seed = 0x52414C4Cu;
  universeInit(seed);
  journeyInit(seed);
  terrainInit(seed);
  fieldInit();
  sceneInit();
  renderInit();
  databankInit();

  journeyForceBiome(BM_FOREST_GRAVEL, BM_MOUNTAIN_DIRT, 0.2f);
  setTimeOfDay(0.28f, 10.f, 50.f);

  float x = 0.f, z = 0.f, yaw = 0.4f;
  float speed = 18.f;
  float dist = 0.f;
  // warm
  for (int i = 0; i < 30; i++) {
    g_us += 33333;
    terrainUpdateRoad(x, z, yaw, speed, 1.f/30.f);
    fieldUpdate(x, z, yaw, speed, 1.f/30.f);
  }

  const int FRAMES = 48;
  for (int f = 0; f < FRAMES; f++) {
    g_us += 33333;
    float dt = 1.f / 30.f;
    // gentle weave
    float steer = sinf(f * 0.12f) * 0.35f + sinf(f * 0.05f) * 0.15f;
    yaw += steer * dt * 1.2f;
    float vx = cosf(yaw) * speed;
    float vz = sinf(yaw) * speed;
    x += vx * dt;
    z += vz * dt;
    dist += speed * dt;
    speed = 16.f + 8.f * sinf(f * 0.08f + 0.5f);

    terrainUpdateRoad(x, z, yaw, speed, dt);
    journeyUpdate(x, z, speed, dt);
    fieldUpdate(x, z, yaw, speed, dt);
    float y = terrainHeight(x, z) + 0.4f;
    float t = millis() / 1000.f;
    // breathe time of day slightly
    float tod = 0.26f + 0.08f * sinf(f * 0.03f);
    setTimeOfDay(tod, t, dist);
    sceneUpdate(t, dt, x, y, z, yaw, 0.03f + steer * 0.05f, dist, speed, 0.55f, 0.72f, fabsf(steer) * 0.4f, false);
    // camera lean
    float camYaw = yaw + steer * 0.15f;
    float camPitch = 0.04f - (speed - 18.f) * 0.002f;
    renderFrame(x, y, z, camYaw, camPitch, t);

    char path[64];
    snprintf(path, sizeof path, "snaps/drive/f%02d.ppm", f);
    writePPM(path);
    if (f % 8 == 0) printf("frame %d  pos %.0f,%.0f spd %.1f biome %s\n",
      f, x, z, speed, journeyBiome(journeyCurrent()).name);
  }
  printf("done %d frames\n", FRAMES);
  return 0;
}
