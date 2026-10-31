#include "audio.h"
#include "audio_int.h"
#include "scene.h"
#include "journey.h"
#include <string.h>
#include <math.h>
#ifndef HOST
#include <M5Unified.h>
#endif

namespace mantis {
CarAudio g_car;
namespace snd { dsp::Rng rng; }
using namespace snd;

// ---------------- game -> audio events: single producer, single consumer ----------------
enum : uint8_t { EV_ENG, EV_THUNDER, EV_CRITTER, EV_NBUMP };
struct Ev { uint8_t type, a; float x, y; };
static Ev s_q[32];
static volatile uint32_t s_head = 0, s_tail = 0;
static void post(const Ev& e) {
    uint32_t h = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
    uint32_t n = (h + 1) & 31u;
    if (n == __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE)) return;
    s_q[h] = e;
    __atomic_store_n(&s_head, n, __ATOMIC_RELEASE);
}
static void drain() {
    uint32_t t = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);
    while (t != __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
        const Ev& e = s_q[t];
        switch (e.type) {
            case EV_ENG: engineEvent(e.a, e.x); break;
            case EV_THUNDER: natureThunder(e.x, e.y, 1.f - e.x / 7.f); break;
            case EV_CRITTER: natureCritter(e.a, e.x); break;
            case EV_NBUMP: natureBump(e.a); break;
        }
        t = (t + 1) & 31u;
        __atomic_store_n(&s_tail, t, __ATOMIC_RELEASE);
    }
}
void sfxBump(float s) { post({EV_ENG, EE_BUMP, s, 0}); }
void sfxLand(float s) { post({EV_ENG, EE_LAND, s, 0}); }
void sfxShift(int d) { post({EV_ENG, EE_SHIFT, (float)d, 0}); }
void sfxHorn() { post({EV_ENG, EE_HORN, 0, 0}); }
void sfxThunder(float delay, float power) { post({EV_THUNDER, 0, delay, power}); }
void sfxSplash(float s) { post({EV_ENG, EE_SPLASH, s, 0}); }
void sfxCrash(float s, bool tree) { post({EV_ENG, EE_CRASH, s, 0}); post({EV_NBUMP, (uint8_t)(tree ? BUMP_TREE : BUMP_ROCK), 0, 0}); }
void sfxCritter(uint8_t kind, float dist) { post({EV_CRITTER, kind, dist, 0}); }
void sfxUi(uint8_t kind) { post({EV_ENG, EE_UI, (float)kind, 0}); }

// ---------------- master bus ----------------
static dsp::Reverb s_rev;
static dsp::DubDelay s_dly;
static bool s_running = false;
static Bus s_bus;
static float s_env = 0.f, s_gain = 1.f;
static dsp::SVF s_hp;
static volatile bool s_muted = false;
static float s_out = 1.f;
static NatScene s_nat;
static float s_still = 0.f, s_rainAgo = 999.f;

void audioSetMuted(bool m) { s_muted = m; }
bool audioMuted() { return s_muted; }

static void natureScene(int n) {
    const Scene& sc = g_scene;
    NatScene& s = s_nat;
    const Biome& A = biome(sc.biomeA);
    float dt = (float)n / dsp::SR;
    if (sc.speed < 0.5f) s_still += dt; else s_still = 0.f;
    if (sc.rain > 0.15f) s_rainAgo = 0.f; else s_rainAgo += dt;
    s.t = sc.t; s.tod = sc.tod; s.yaw = sc.yaw; s.speed = sc.speed * 0.05f; s.stillFor = s_still;
    s.cloud = sc.cloud; s.rain = sc.rain; s.rainAgo = s_rainAgo; s.storm = sc.storm; s.wet = sc.wet;
    s.forestNear = A.pine + A.broad + A.redwood > 0.5f ? 0.8f : 0.2f;
    s.lakeNear = A.water * 0.8f;
    s.oceanNear = sc.biomeA == B_COAST ? 0.8f : 0.f;
    s.streamNear = (sc.biomeA == B_PINE || sc.biomeA == B_REDWOOD || sc.biomeA == B_AUTUMN) ? 0.25f : 0.f;
    s.fallsNear = sc.biomeA == B_DEVILS ? 0.2f : 0.f;
    s.open = (sc.biomeA == B_MEADOW || sc.biomeA == B_CANYON || sc.biomeA == B_TUNDRA) ? 1.f : 0.3f;
    s.gust = sc.wind; s.hush = 0.f; s.murmur = 0.f;
    s.frogBoost = sc.biomeA == B_BAYOU ? 1.f : 0.f;
    s.snowy = sc.snow; s.tundra = sc.biomeA == B_TUNDRA ? 1.f : 0.f;
    s.tropical = sc.biomeA == B_BAYOU ? 0.6f : 0.f; s.desert = sc.biomeA == B_CANYON ? 1.f : 0.f;
    s.japan = 0.f; s.crows = sc.biomeA == B_MOOR ? 0.5f : 0.f; s.cuckoo = sc.biomeA == B_PINE ? 0.3f : 0.f; s.giant = 0.f;
}

void audioRender(int16_t* out, int n) {
    if (n > BLOCK) n = BLOCK;
    drain();
    memset(&s_bus, 0, sizeof(s_bus));
    natureScene(n);
    natureBlock(s_bus, n, s_nat);
    engineBlock(s_bus, n);
    const Scene& sc = g_scene;
    s_dly.target = AUDIO_SR * 0.21f;
    s_rev.fb = 0.6f + 0.1f * dsp::clampf(sc.fog, 0.f, 1.f) + (sc.biomeA == B_CANYON || sc.biomeA == B_DEVILS ? 0.1f : 0.f);
    s_rev.damp = 0.42f;
    s_hp.set(165.f, 0.72f);
    const float outTarget = s_muted ? 0.f : 1.f;
    for (int i = 0; i < n; i++) {
        float d = s_dly.tick(s_bus.dly[i]);
        float r = s_rev.tick(s_bus.rev[i] + d * 0.3f);
        float x = s_bus.dry[i] + d * 0.6f + r * 0.85f;
        x = s_hp.hp(x) * 1.25f;
        float a = fabsf(x);
        s_env = a > s_env ? s_env + (a - s_env) * 0.25f : s_env * 0.99985f;
        float g = s_env > 0.62f ? 0.62f / s_env : 1.f;
        s_gain += (g - s_gain) * (g < s_gain ? 0.2f : 0.0004f);
        x = dsp::softclip(x * s_gain * 1.05f);
        s_out += (outTarget - s_out) * 0.002f;
        out[i] = (int16_t)(x * s_out * 29000.f);
    }
}

#ifndef HOST
static void audioTask(void*) {
    static int16_t bufs[3][BLOCK];
    int k = 0;
    for (;;) {
        if (M5.Speaker.isPlaying(0) < 2) {
            audioRender(bufs[k], BLOCK);
            M5.Speaker.playRaw(bufs[k], BLOCK, AUDIO_SR, false, 1, 0, false);
            k = (k + 1) % 3;
        } else vTaskDelay(1);
    }
}
#endif

// Right after M5.begin(): the speaker driver's DMA buffers must come from internal RAM; top priority and a deep
// DMA cushion so a long frame can't make it stutter (as MantisPlanet).
void audioSpeakerBegin() {
#ifndef HOST
    if (M5.Speaker.isEnabled()) M5.Speaker.end();
    auto sc = M5.Speaker.config();
    sc.task_priority = 5;
    sc.dma_buf_len = 256;
    sc.dma_buf_count = 16;
    M5.Speaker.config(sc);
    M5.Speaker.begin();
    M5.Speaker.setVolume(170);
#endif
}

void audioBegin() {
#ifndef HOST
    if (!M5.Speaker.isEnabled()) audioSpeakerBegin();
#endif
    dsp::init();
    rng.s = 0x2545F491u;
    s_rev.begin();
    s_dly.begin(AUDIO_SR / 2);
    memset(&g_car, 0, sizeof(g_car));
    g_car.rpm = 900.f;
    natureInit();
    engineInit();
#ifndef HOST
    s_running = xTaskCreatePinnedToCore(audioTask, "audio", 6144, nullptr, 4, nullptr, 0) == pdPASS;
#else
    s_running = true;
#endif
}
bool audioRunning() { return s_running; }

}
