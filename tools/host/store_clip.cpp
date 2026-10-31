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
int main() {
  tilt(0, 0);
  setup();
  uint32_t seed = 0x5707E;
  universeInit(seed); journeyInit(seed); journeyForce(B_LAKES, B_LAKES, 0.f); terrainInit(seed); sceneInit(seed); fieldInit(seed);
  g_prog.owned = 1; g_prog.current = V_STI; switchVehicle(V_STI); placeOnRoad(false);
  g_prog.wallet[V_STI] = 24.f;                              // enough miles in the STI for the Jeep
  g_prog.view = VIEW_CHASE; msgT = 0;
  setTimeOfDay(0.45f, 0, 0);
  int i = 0;
  // drive (unrecorded) until a store is placed ahead of us
  for (int k = 0; k < 30 * 600; k++) {
    float st = follow(10 + car.speed * 0.7f); float tip = car.speed < 24.f ? 0.8f : 0.1f;
    if (k < 15) { st = 0; tip = 0; }
    tilt(fmaxf(-1.f, fminf(1.f, st)), tip); frame(false, i++);
    if ((k % 300) == 0) roadRequestStore();
    const MotorSign* ms = roadMotorSign();
    if (ms->active) { float ah = (ms->x - car.x) * cosf(car.yaw) + (ms->z - car.z) * sinf(car.yaw); if (ah > 120.f && ah < 220.f) break; }
  }
  printf("store found after %.2f miles\n", distance / 1609.f);
  // 1. the store on the side of the road: drive up to it
  startClip("store_road"); i = 0;
  bool roadRec = false;
  const Store* s = roadStore();
  int phase = 0, stopped = 0;
  for (int k = 0; k < 30 * 40 && phase < 2; k++) {
    float d = s->active ? hypotf(s->x - car.x, s->z - car.z) : 1e9f;
    float st = follow(10 + car.speed * 0.7f), tip;
    if (d > 130.f) tip = car.speed < 18.f ? 0.6f : 0.1f;
    else {                                                  // ease off, then pull onto the lot beside the store
      if (phase == 0 && d < 95.f) { phase = 1; startClip("store_visit"); i = 0; }
      float rx = -sinf(s->yaw), rz = cosf(s->yaw);
      float side = ((s->x - car.x) * rx + (s->z - car.z) * rz) > 0 ? 1.f : -1.f;
      float tx = s->x - rx * side * 9.f, tz = s->z - rz * side * 9.f;      // the lot in front of the building
      st = wrapA(atan2f(tz - car.z, tx - car.x) - car.yaw) * 2.f;
      float want = fminf(20.f, (hypotf(tx - car.x, tz - car.z) - 4.f) * 0.45f);
      tip = car.speed > want + 1.f ? -0.8f : (car.speed < want - 1.f ? 0.4f : 0.f);
      if (want < 1.f) tip = -0.7f;
    }
    roadRec = true;
    tilt(fmaxf(-1.f, fminf(1.f, st)), tip); frame(roadRec || phase > 0, i++);
    if (storeActive()) { stopped++; if (stopped > 30) phase = 2; }
  }
  printf("store opened: %d\n", storeActive());
  tilt(0, 0);
  for (int k = 0; k < 40; k++) frame(true, i++);            // look at the offer
  tap(244, 165, true, i);                                    // BUY
  for (int k = 0; k < 50; k++) frame(true, i++);            // SOLD!
  tap(244, 211, true, i);                                    // DRIVE ON
  for (int k = 0; k < 30 * 4; k++) { float st = follow(10 + car.speed * 0.7f); tilt(fmaxf(-1.f, fminf(1.f, st)), k < 20 ? 0.f : 0.7f); frame(true, i++); }
  if (s_au) fclose(s_au);
  printf("now driving: %s, owned %d\n", vehicleSpec(veh).name, g_prog.owned);
}
