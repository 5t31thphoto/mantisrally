// The whole game on the desktop: real setup()/loop(), the tilt autopilot drives through the IMU path.
//   drive_sim [minutes] [vehicle] [view]   prints a log line every 10 s, timings at the end
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>
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
static void tilt(float steer, float tip) {
  float roll = -steer * 0.42f;                       // the helm inverts roll: + steer = lean right
  float p = 1.5708f + tip * 0.28f;
  M5.Imu.data.accel = {tanf(roll), cosf(p), sinf(p)};
}
static float wrapA(float a){ while(a>3.14159f)a-=6.28318f; while(a<-3.14159f)a+=6.28318f; return a;}
int main(int argc, char** argv) {
  float minutes = argc > 1 ? atof(argv[1]) : 3.f;
  int vsel = argc > 2 ? atoi(argv[2]) : 0;
  tilt(0, 0);
  setup();
  g_prog.owned = 15; g_prog.current = (uint8_t)vsel; switchVehicle((uint8_t)vsel);
  g_prog.view = argc > 3 ? atoi(argv[3]) : VIEW_CHASE;
  int frames = (int)(minutes * 60 * 30);
  double rtot = 0, rmax = 0; int nAir = 0, nOff = 0, nResets = 0; float maxMph = 0;
  for (int i = 0; i < frames; i++) {
    RoadHit h; float st = 0;
    if (roadNearest(car.x, car.z, h, 600)) {
      int dir = cosf(car.yaw - h.yaw) >= 0 ? 1 : -1; int n = h.arm ? roadArmCount() : roadCount();
      int k = h.idx + dir * (int)((8 + car.speed * 0.7f) / 3.f); k = k < 0 ? 0 : (k > n - 1 ? n - 1 : k);
      const RoadNode& p = h.arm ? roadArmNode(k) : roadNode(k);
      st = wrapA(atan2f(p.z - car.z, p.x - car.x) - car.yaw) * 2.2f - car.r * 0.25f;
    }
    // corner speed: brake for what's coming
    PaceNote pn = roadPaceNote(car.x, car.z, car.yaw, car.speed);
    float want = pn.valid && pn.dir ? (pn.sev <= 1 ? 9.f : pn.sev == 2 ? 15.f : pn.sev == 3 ? 23.f : 32.f) : 38.f;
    float tip = car.speed < want ? 0.9f : (car.speed > want + 4 ? -0.6f : 0.1f);
    if (i < 15) { st = 0; tip = 0; }
    tilt(st > 1 ? 1 : (st < -1 ? -1 : st), tip);
    g_us += 33333;
    auto a = std::chrono::high_resolution_clock::now();
    loop();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - a).count();
    rtot += ms; if (ms > rmax && i > 30) rmax = ms;
    if (car.air) nAir++;
    if (offT > 0) nOff++;
    if (car.speed * 2.237f > maxMph) maxMph = car.speed * 2.237f;
    if (i % 300 == 0)
      printf("t=%4.0fs %-18s -> %-7s %5.1f mph g%d rpm %4.0f  mi %.2f  wallet %.2f  road %s junc %s store %s  %s\n", i / 30.f,
        biome(journeyCurrent()).name, biome(journeyDestination()).sign, car.speed * 2.237f, car.gear, car.rpm, distance / 1609.f,
        g_prog.wallet[g_prog.current], fabsf(car.across) < car.half ? "on " : "off", roadJunction()->active ? "Y" : "-", roadStore()->active ? "Y" : "-",
        msgT > 0 ? msgBuf : "");
  }
  printf("frames %d  mean %.2f ms  worst %.2f ms   air %d frames  off-clock %d frames  top %.0f mph  miles %.2f\n", frames, rtot / frames, rmax, nAir, nOff, maxMph, distance / 1609.f);
}
