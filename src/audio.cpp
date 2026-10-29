#include "audio.h"
#include "audio_int.h"
#include <string.h>
#ifndef HOST
#include <M5Unified.h>
#endif

namespace mantis {
namespace snd {
dsp::Rng rng;
uint32_t samples = 0;
}
using namespace snd;

// ---------------- game -> audio events: single producer, single consumer ----------------
enum : uint8_t { EV_BUMP, EV_THUNDER, EV_CRITTER, EV_MOMENT, EV_UI, EV_SONAR };
struct Ev { uint8_t type, a; float x, y; };
static Ev s_q[32];
static volatile uint32_t s_head = 0, s_tail = 0;

static void post(const Ev& e) {
    uint32_t h = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
    uint32_t n = (h + 1) & 31u;
    if (n == __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE)) return;    // full: drop, never block the game
    s_q[h] = e;
    __atomic_store_n(&s_head, n, __ATOMIC_RELEASE);
}
static void drain() {
    uint32_t t = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);
    while (t != __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
        const Ev& e = s_q[t];
        switch (e.type) {
            case EV_BUMP:    oceanBump(e.x); break;
            case EV_THUNDER: oceanThunder(e.x, e.y); break;
            case EV_CRITTER: oceanCritter(e.a, e.x); break;
            case EV_MOMENT:  musicMoment(e.a); oceanMoment(e.a); break;
            case EV_UI:      oceanUi(e.a); break;
            case EV_SONAR:   oceanSonar(e.x, 0.f); break;  // horn
        }
        t = (t + 1) & 31u;
        __atomic_store_n(&s_tail, t, __ATOMIC_RELEASE);
    }
}

void sfxBump(float s) { post({EV_BUMP, 0, s, 0}); }
void sfxThunder(float delay, float power) { post({EV_THUNDER, 0, delay, power}); }
void sfxCritter(uint8_t kind, float dist) { post({EV_CRITTER, kind, dist, 0}); }
void sfxMoment(uint8_t kind) { post({EV_MOMENT, kind, 0, 0}); }
void sfxUi(uint8_t kind) { post({EV_UI, kind, 0, 0}); }
void sfxHorn(float strength) { post({EV_SONAR, 0, strength, 0.f}); }

// ---------------- master bus ----------------
static dsp::Reverb s_rev;
#ifdef HOST
namespace snd { int g_solo = -1; }
#endif
static dsp::DubDelay s_dly;
static bool s_running = false;
static Bus s_bus;
static float s_env = 0.f, s_gain = 1.f;
static dsp::SVF s_hp;
static volatile float s_kick = 0.f, s_groove = 0.f;
static volatile bool s_muted = false;
static float s_out = 1.f;          // speaker gain, ramped so muting never clicks
static const int LATENCY = BLOCK * 2 + 2600;    // queued block + the speaker driver's (deeper) DMA buffers

float audioBeats() {
    int32_t heard = (int32_t)samples - LATENCY;
    if (heard < 0) heard = 0;
    return heard * (MUSIC_BPM / 60.f) / AUDIO_SR;
}
float audioKick() { return s_kick; }
float audioGroove() { return s_groove; }
void audioSetMuted(bool m) { s_muted = m; }
bool audioMuted() { return s_muted; }

void audioRender(int16_t* out, int n) {
    if (n > BLOCK) n = BLOCK;
    drain();
    memset(&s_bus, 0, sizeof(s_bus));
    const Scene& sc = g_scene;
    const float outTarget = s_muted ? 0.f : 1.f;
    oceanBlock(s_bus, n, sc);
    musicBlock(s_bus, n, sc);
    s_kick = musicKick();
    s_groove = musicGroove();
    s_dly.target = musicDelaySamples();
    // outdoor stage: shorter tail, grows a little with fog / distance
    s_rev.fb = 0.62f + 0.08f * dsp::clampf(sc.fog, 0.f, 1.f) + 0.04f * dsp::clampf(sc.distance / 2000.f, 0.f, 1.f);
    s_rev.damp = 0.42f;
    s_hp.set(165.f, 0.72f);    // the Core2 speaker can't move air down there: spend the headroom where it sings
    for (int i = 0; i < n; i++) {
        float d = s_dly.tick(s_bus.dly[i]);
        float r = s_rev.tick(s_bus.rev[i] + d * 0.3f);
        float x = s_bus.dry[i] + d * 0.85f + r * 0.9f;
        x = s_hp.hp(x) * 1.25f;
        float a = fabsf(x);
        s_env = a > s_env ? s_env + (a - s_env) * 0.25f : s_env * 0.99985f;
        float g = s_env > 0.62f ? 0.62f / s_env : 1.f;
        s_gain += (g - s_gain) * (g < s_gain ? 0.2f : 0.0004f);
        x = dsp::softclip(x * s_gain * 1.05f);
        s_out += (outTarget - s_out) * 0.002f;          // ~20 ms fade
        out[i] = (int16_t)(x * s_out * 29000.f);
    }
    samples += n;
}

#ifndef HOST
static void audioTask(void*) {
    static int16_t bufs[3][BLOCK];
    int k = 0;
    const int CH = 0;
    for (;;) {
        if (M5.Speaker.isPlaying(CH) < 2) {
            audioRender(bufs[k], BLOCK);
            M5.Speaker.playRaw(bufs[k], BLOCK, AUDIO_SR, false, 1, CH, false);
            k = (k + 1) % 3;
        } else {
            vTaskDelay(1);
        }
    }
}
#endif

// Call right after M5.begin(), before anything else allocates: the speaker driver's DMA buffers must come
// from internal RAM. The driver runs its own task that feeds the I2S DMA. By default it shares a priority
// with the LCD push (which keeps core 0 busy for most of each frame) and its DMA cushion is short, so it can
// run dry: a stutter even when the frame rate is fine. Top priority and ~85 ms of DMA ride out any hiccup.
void audioSpeakerBegin() {
#ifndef HOST
    if (M5.Speaker.isEnabled()) M5.Speaker.end();
    auto sc = M5.Speaker.config();
    sc.task_priority = 5;
    sc.dma_buf_len = 256;
    sc.dma_buf_count = 16;
    M5.Speaker.config(sc);
    M5.Speaker.begin();
    M5.Speaker.setVolume(165);
#endif
}

void audioBegin() {
#ifndef HOST
    if (!M5.Speaker.isEnabled()) audioSpeakerBegin();
#endif
    dsp::init();
    rng.s = 0x2545F491u;
    s_rev.begin();
    s_dly.begin((int)(AUDIO_SR * 60.f / MUSIC_BPM * 0.75f) + 600);   // a dotted eighth at the score's tempo, with room for the wow
    oceanInit();
    musicInit();
#ifndef HOST
    s_running = xTaskCreatePinnedToCore(audioTask, "audio", 6144, nullptr, 4, nullptr, 0) == pdPASS;   // above the LCD push (2), below the driver (5)
#else
    s_running = true;
#endif
}
bool audioRunning() { return s_running; }

}
