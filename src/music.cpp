// Stage score — builds with cruise, shifts colour by biome, kicks on big slip.
#include <math.h>
#include "audio.h"
#include "audio_int.h"
#include "scene.h"
#include "universe.h"

namespace mantis {
namespace snd {

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static float s_phase = 0.f, s_kick = 0.f, s_groove = 0.f;
static float s_bassPh = 0.f;

void musicInit() { s_phase = 0.f; s_kick = 0.f; s_groove = 0.f; s_bassPh = 0.f; }

void musicBlock(Bus& b, int n, const Scene& s) {
    float drive = clampf(s.cruiseFor / 35.f, 0.f, 1.f);
    float speedK = clampf(s.speed / 40.f, 0.f, 1.f);
    float hz = 42.f + s.stage * 1.7f + speedK * 8.f;
    float bassHz = 55.f + s.stage * 0.8f;

    // kick pulse on slip spikes
    if (s.slip > 0.55f) s_kick = fmaxf(s_kick, s.slip * 0.6f);
    s_kick = fmaxf(0.f, s_kick - 0.002f);

    for (int i = 0; i < n; i++) {
        s_phase += hz / (float)AUDIO_SR;
        if (s_phase > 1.f) s_phase -= 1.f;
        s_bassPh += bassHz / (float)AUDIO_SR;
        if (s_bassPh > 1.f) s_bassPh -= 1.f;

        float pad = sinf(s_phase * kTau) * 0.10f * (0.3f + drive * 0.7f);
        pad += sinf(s_phase * kTau * 1.5f) * 0.04f * drive;
        float bass = sinf(s_bassPh * kTau) * 0.07f * speedK;
        float kick = s_kick * sinf(s_phase * kTau * 2.f) * 0.12f;

        b.dry[i] += (pad + bass + kick) * soloGain(TAG_M_DRONE);
        b.rev[i] += pad * 0.45f * soloGain(TAG_M_PAD);
    }
    s_groove = drive * 0.7f + speedK * 0.3f;
}

void musicMoment(uint8_t) {}
float musicKick() { return s_kick; }
float musicGroove() { return s_groove; }
float musicDelaySamples() { return (float)AUDIO_SR * 0.18f; }

}
}
