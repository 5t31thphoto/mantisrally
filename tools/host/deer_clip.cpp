// The Mantis Motors experience, recorded from the real game (screen + overlay + sound):
//   store_clip            -> clips/store_road (the store beside the road) and clips/store_visit (pull in, buy, drive on)
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
static FILE* s_au = nullptr; static int s_shot = 0; static char s_dir[64];
static void startClip(const char* name) {
  snprintf(s_dir, sizeof s_dir, "clips/%s", name);
  char cmd[160]; snprintf(cmd, sizeof cmd, "mkdir -p %s && rm -f %s/*.ppm", s_dir, s_dir); (void)system(cmd);
  char p[96]; snprintf(p, sizeof p, "%s/audio.raw", s_dir); if (s_au) fclose(s_au); s_au = fopen(p, "wb"); s_shot = 0;
}
static void frame(bool rec, int i) {
  g_us += 33333; loop();
  static int16_t ab[400];
  for (int k = 0; k < 2; k++) { audioRender(ab, 400); if (rec && s_au) fwrite(ab, 2, 400, s_au); }
  if (!rec || (i & 1)) return;
  char p[96]; snprintf(p, sizeof p, "%s/%04d.ppm", s_dir, s_shot++);
  FILE* f = fopen(p, "wb"); fprintf(f, "P6\n320 240\n255\n");
  static uint16_t row[320];
  for (int y = 0; y < 240; y++) {
    compositeRow(y, row);                                   // exactly what the panel shows: world + overlay
    for (int x = 0; x < 320; x++) { uint16_t v = row[x]; uint8_t c[3] = {(uint8_t)((v >> 8) & 0xF8), (uint8_t)((v >> 3) & 0xFC), (uint8_t)((v << 3) & 0xF8)}; fwrite(c, 1, 3, f); }
  }
  fclose(f);
}
static float follow(float look) {
  RoadHit h; if (!roadNearest(car.x, car.z, h, 600)) return 0;
  int dir = cosf(car.yaw - h.yaw) >= 0 ? 1 : -1; int n = h.arm ? roadArmCount() : roadCount();
  int k = h.idx + dir * (int)(look / 3.f); k = k < 0 ? 0 : (k > n - 1 ? n - 1 : k);
  const RoadNode& p = h.arm ? roadArmNode(k) : roadNode(k);
  return wrapA(atan2f(p.z - car.z, p.x - car.x) - car.yaw) * 2.2f - car.r * 0.25f;
}
static void tap(int x, int y, bool rec, int& i) {
  M5.Touch.x = x; M5.Touch.y = y; M5.Touch.down = true; for (int k = 0; k < 3; k++) frame(rec, i++);
  M5.Touch.down = false; for (int k = 0; k < 2; k++) frame(rec, i++);
}
int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "deer";
  tilt(0, 0);
  setup();
  uint32_t seed = 0xDEE5;
  bool rain = mode[0] == 'r';
  uint8_t b = rain ? B_MOOR : B_MEADOW;
  universeInit(seed); journeyInit(seed); journeyForce(b, b, 0.f); terrainInit(seed); sceneInit(seed); fieldInit(seed);
  g_prog.owned = 1; g_prog.current = V_STI; switchVehicle(V_STI); placeOnRoad(false);
  g_prog.view = VIEW_COCKPIT; msgT = 0;
  setTimeOfDay(rain ? 0.45f : 0.42f, 0, 0);
  int i = 0;
  if (rain) sceneForceWeather(0.85f, 0.7f, 0.3f, 0.1f, 0.f);
  for (int k = 0; k < 30 * 6; k++) { float st = follow(10 + car.speed * 0.7f); tilt(k < 15 ? 0 : fmaxf(-1.f, fminf(1.f, st)), k < 15 ? 0 : (car.speed < 22.f ? 0.8f : 0.1f)); frame(false, i++); }
  startClip(rain ? "rain_wipers" : "deer_hit"); i = 0;
  bool placed = false;
  for (int k = 0; k < 30 * (rain ? 8 : 6); k++) {
    if (!rain && !placed && k == 15) {                      // a deer that won't make it, standing in the road ahead
      Critter* c = const_cast<Critter*>(fieldCritters());
      RoadHit h; roadNearest(car.x, car.z, h);
      int dir = cosf(car.yaw - h.yaw) >= 0 ? 1 : -1; int idx = h.idx + dir * 16;
      const RoadNode& n = roadNode(idx);
      c[0].on = true; c[0].x = n.x; c[0].z = n.z; c[0].y = n.y; c[0].yaw = n.yaw + 1.57f; c[0].speed = 0.f; c[0].pose = 3; c[0].fear = 0.f; c[0].dodger = false; c[0].t = 0.f;
      placed = true;
    }
    if (!rain && placed) { const Critter* c = fieldCritters(); Critter* m = const_cast<Critter*>(c); if (m[0].on) { m[0].speed = 0.f; m[0].fear = 0.f; } }   // frozen in the headlights
    float st = follow(10 + car.speed * 0.7f);
    tilt(fmaxf(-1.f, fminf(1.f, st)), car.speed < 22.f ? 0.6f : 0.1f);
    frame(true, i++);
  }
  if (s_au) fclose(s_au);
  printf("%s done\n", mode);
}
