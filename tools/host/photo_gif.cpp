// Photo mode on the desktop: drive somewhere, stop, and take the four angles (A, A, A, A).
//   photo_shoot <biome> <tod> <vehicle> <seconds of driving> [weather]   -> images/rally/rally_NNNN.mg
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
static int s_shot = 0;
static void cap() {                                   // what the panel shows: world + overlay
  char p[64]; snprintf(p, sizeof p, "pg/%04d.ppm", s_shot++);
  FILE* f = fopen(p, "wb"); fprintf(f, "P6\n320 240\n255\n");
  static uint16_t row[320];
  for (int y = 0; y < 240; y++) { compositeRow(y, row); for (int x = 0; x < 320; x++) { uint16_t v = row[x]; uint8_t c3[3] = {(uint8_t)((v >> 8) & 0xF8), (uint8_t)((v >> 3) & 0xFC), (uint8_t)((v << 3) & 0xF8)}; fwrite(c3, 1, 3, f); } }
  fclose(f);
}
static int s_fr = 0;
static void step(int n, bool rec = false) { for (int k = 0; k < n; k++) { g_us += 33333; loop(); if (rec && ((s_fr++ & 1) == 0)) cap(); } }
static void press(HostBtn& b, bool rec) { b.down = true; step(1, rec); b.down = false; step(1, rec); }
int main() {
  (void)!system("mkdir -p pg && rm -f pg/*.ppm");
  tilt(0, 0);
  setup();
  uint32_t seed = 0x9070u + 2 * 17u;
  universeInit(seed); journeyInit(seed); journeyForce(B_LAKES, B_LAKES, 0.f); terrainInit(seed); sceneInit(seed); fieldInit(seed);
  g_prog.owned = 15; switchVehicle(V_STI); g_prog.current = V_STI; placeOnRoad(false);
  g_prog.view = VIEW_CHASE; msgT = 0;
  setTimeOfDay(0.69f, 0, 0);
  for (int i = 0; i < 30 * 10; i++) {
    RoadHit h; float st = 0;
    if (roadNearest(car.x, car.z, h, 600)) {
      int dir = cosf(car.yaw - h.yaw) >= 0 ? 1 : -1; int n = h.arm ? roadArmCount() : roadCount();
      int k = h.idx + dir * (int)((8 + car.speed * 0.7f) / 3.f); k = k < 0 ? 0 : (k > n - 1 ? n - 1 : k);
      const RoadNode& p = h.arm ? roadArmNode(k) : roadNode(k);
      st = wrapA(atan2f(p.z - car.z, p.x - car.x) - car.yaw) * 2.2f - car.r * 0.25f;
    }
    float tip = car.speed < 20.f ? 0.7f : 0.1f;
    if (i < 15) { st = 0; tip = 0; }
    tilt(fmaxf(-1.f, fminf(1.f, st)), tip);
    g_us += 33333; setTimeOfDay(0.69f, t, distance); sceneForceWeather(0.3f, 0.f, 0.1f, 0.f, 0.f); loop();
  }
  for (int i = 0; i < 30 * 8 && car.speed > 0.3f; i++) { tilt(0, -0.9f); g_us += 33333; loop(); }
  tilt(0, 0); step(20);
  // the camera: A
  press(M5.BtnA, true); step(20, true);
  // side angle: turn left, right, back; tilt up, down, back
  for (int k = 0; k < 90; k++) { tilt(sinf(k / 90.f * 6.2832f), 0); step(1, true); }
  for (int k = 0; k < 60; k++) { tilt(0, sinf(k / 60.f * 6.2832f)); step(1, true); }
  tilt(0, 0); step(10, true);
  // the shot (flash), and it moves on to the rear
  press(M5.BtnA, true); step(30, true);
  for (int k = 0; k < 60; k++) { tilt(0.8f * sinf(k / 60.f * 6.2832f), 0); step(1, true); }
  tilt(0, 0); step(8, true);
  // C, C: the other side, then the free look around from the driver's seat
  press(M5.BtnC, true); step(30, true);
  press(M5.BtnC, true); step(15, true);
  for (int k = 0; k < 150; k++) { tilt(0.9f, 0); step(1, true); }            // turn all the way round
  tilt(0, 0); step(5, true);
  for (int k = 0; k < 75; k++) { tilt(0, sinf(k / 75.f * 6.2832f)); step(1, true); }   // look up, then down
  tilt(0, 0); step(8, true);
  // B: back to driving
  press(M5.BtnB, true); step(25, true);
  printf("frames %d, photo mode %d\n", s_shot, photoActive());
}
