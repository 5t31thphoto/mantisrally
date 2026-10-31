// Record the real game to frames: clip <name> <biome> <tod> <vehicle> <view> <weather> <seconds> <mode>
//   weather: 0 clear 1 rain 2 storm 3 snow 4 fog      mode: 0 autopilot  1 donuts  2 autopilot+slides
// writes clips/<name>/NNNN.ppm (320x240) every other frame
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "Arduino.h"
#include "M5Unified.h"
M5Class M5; M5Canvas* g_hostLastCanvas = nullptr;
static uint64_t g_us = 1000000;
uint32_t millis() { return (uint32_t)(g_us / 1000); }
uint32_t micros() { return (uint32_t)g_us; }
void delay(uint32_t ms) { g_us += (uint64_t)ms * 1000; }
uint32_t esp_random() { static uint32_t x = 0xC0FFEEu; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
#define main game_main
#include "../../src/main.cpp"
#undef main
static void tilt(float steer, float tip) { float roll = -steer * 0.42f, p = 1.5708f + tip * 0.28f; M5.Imu.data.accel = {tanf(roll), cosf(p), sinf(p)}; }
static float wrapA(float a){ while(a>3.14159f)a-=6.28318f; while(a<-3.14159f)a+=6.28318f; return a;}
int main(int argc, char** argv) {
  const char* name = argv[1]; int b = atoi(argv[2]); float tod = atof(argv[3]); int vsel = atoi(argv[4]); int view = atoi(argv[5]);
  int wx = atoi(argv[6]); float secs = atof(argv[7]); int mode = atoi(argv[8]); float warm = argc > 9 ? atof(argv[9]) : 6.f;
  tilt(0, 0);
  setup();
  uint32_t seed = 0x5EED0000u + b * 131u + vsel;
  universeInit(seed); journeyInit(seed); journeyForce((uint8_t)b, (uint8_t)b, 0.f); terrainInit(seed); sceneInit(seed); fieldInit(seed);
  g_prog.owned = 15; switchVehicle((uint8_t)vsel); g_prog.current = (uint8_t)vsel; placeOnRoad(false);
  g_prog.view = (uint8_t)view; msgT = 0;
  setTimeOfDay(tod, 0, 0);
  char cmd[256]; snprintf(cmd, sizeof cmd, "mkdir -p clips/%s && rm -f clips/%s/*.ppm", name, name); (void)system(cmd);
  int total = (int)((warm + secs) * 30), shot = 0;
  snprintf(cmd, sizeof cmd, "clips/%s/audio.raw", name); FILE* au = fopen(cmd, "wb");
  static int16_t abuf[400];
  int lr = 0; float lean0 = 0;
  for (int i = 0; i < total; i++) {
    float st = 0, tip = 0;
    RoadHit h;
    if (mode == 1) { st = 1.f; tip = 1.f; if (i < 40) { st = 0; tip = 0.3f; } }
    else if (roadNearest(car.x, car.z, h, 600)) {
      int dir = cosf(car.yaw - h.yaw) >= 0 ? 1 : -1; int n = h.arm ? roadArmCount() : roadCount();
      int k = h.idx + dir * (int)((8 + car.speed * 0.7f) / 3.f); k = k < 0 ? 0 : (k > n - 1 ? n - 1 : k);
      const RoadNode& p = h.arm ? roadArmNode(k) : roadNode(k);
      st = wrapA(atan2f(p.z - car.z, p.x - car.x) - car.yaw) * 2.2f - car.r * 0.25f * (mode == 2 ? 0.f : 1.f);
      PaceNote pn = roadPaceNote(car.x, car.z, car.yaw, car.speed);
      float want = pn.valid && pn.dir ? (pn.sev <= 1 ? 9.f : pn.sev == 2 ? 15.f : pn.sev == 3 ? 23.f : 32.f) : 36.f;
      if (mode == 2) want += 6.f;
      tip = car.speed < want ? 0.9f : (car.speed > want + 4 ? -0.6f : 0.1f);
    }
    if (i < 15) { st = 0; tip = 0; }
    tilt(st > 1 ? 1 : (st < -1 ? -1 : st), tip);
    if (mode == 2) M5.BtnC.down = fabsf(st) > 0.8f && car.speed > 12.f;
    g_us += 33333;
    setTimeOfDay(tod, t, distance);
    switch (wx) {
      case 1: sceneForceWeather(0.85f, 0.75f, 0.35f, 0.15f, 0.f); break;
      case 2: sceneForceWeather(0.95f, 0.6f, 0.3f, 0.9f, 0.f); break;
      case 3: sceneForceWeather(0.6f, 0.f, 0.3f, 0.f, 0.8f); break;
      case 4: sceneForceWeather(0.5f, 0.f, 0.8f, 0.f, 0.f); break;
      default: sceneForceWeather(0.3f, 0.f, 0.1f, 0.f, 0.f); break;
    }
    loop();
    audioRender(abuf, 400); if (i >= warm * 30) fwrite(abuf, 2, 400, au);
    audioRender(abuf, 400); if (i >= warm * 30) fwrite(abuf, 2, 400, au);
    if (vsel == 3 && i >= warm * 30 && fabsf(car.r) > 0.15f && car.speed > 6.f) { lr++; if ((car.r > 0) == (car.lean > 0)) lean0 += 1; }
    if (i >= warm * 30 && (i & 1) == 0) {
      const uint8_t* fb = frameBuffer(); const uint8_t* pal = framePaletteRGB();
      char p[256]; snprintf(p, sizeof p, "clips/%s/%04d.ppm", name, shot++);
      FILE* f = fopen(p, "wb"); fprintf(f, "P6\n320 240\n255\n");
      for (int y = 0; y < 120; y++) for (int r = 0; r < 2; r++) for (int x = 0; x < 160; x++) { const uint8_t* c = pal + fb[y * 160 + x] * 3; fwrite(c, 1, 3, f); fwrite(c, 1, 3, f); }
      fclose(f);
    }
  }
  fclose(au);
  if (vsel == 3 && lr) printf("  bike leaning into the turn %.0f%% of turning frames\n", 100.f * lean0 / lr);
  printf("%s: %d frames, %.0f mph, %s\n", name, shot, car.speed * 2.237f, biome(journeyCurrent()).name);
}
