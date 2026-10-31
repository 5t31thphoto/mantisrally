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
static void step(int n) { for (int k = 0; k < n; k++) { g_us += 33333; loop(); } }
int main(int argc, char** argv) {
  int b = atoi(argv[1]); float tod = atof(argv[2]); int vsel = atoi(argv[3]); float secs = atof(argv[4]); int wx = argc > 5 ? atoi(argv[5]) : 0;
  tilt(0, 0);
  setup();
  uint32_t seed = 0x9070u + b * 17u + vsel;
  universeInit(seed); journeyInit(seed); journeyForce((uint8_t)b, (uint8_t)b, 0.f); terrainInit(seed); sceneInit(seed); fieldInit(seed);
  g_prog.owned = 15; switchVehicle((uint8_t)vsel); g_prog.current = (uint8_t)vsel; placeOnRoad(false);
  g_prog.view = VIEW_CHASE; msgT = 0;
  setTimeOfDay(tod, 0, 0);
  for (int i = 0; i < (int)(secs * 30); i++) {
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
    g_us += 33333; setTimeOfDay(tod, t, distance);
    if (wx == 1) sceneForceWeather(0.85f, 0.7f, 0.3f, 0.1f, 0.f); else if (wx == 3) sceneForceWeather(0.6f, 0.f, 0.3f, 0.f, 0.8f); else sceneForceWeather(0.3f, 0.f, 0.1f, 0.f, 0.f);
    loop();
  }
  // stop
  for (int i = 0; i < 30 * 8 && car.speed > 0.3f; i++) { tilt(0, -0.9f); g_us += 33333; loop(); }
  tilt(0, 0); step(20);
  printf("parked at %.0f mph, %s\n", car.speed * 2.237f, biome(journeyCurrent()).name);
  // A: the camera
  M5.BtnA.down = true; step(1); M5.BtnA.down = false; step(30);
  for (int s = 0; s < 4; s++) {
    M5.BtnA.down = true; step(1); M5.BtnA.down = false; step(25);
  }
  printf("photo mode %d\n", photoActive());
}
