// Renders the car radio to WAV files on the host: what each station sounds like, songs and the breaks between them.
//   radio_demo <station 1..3> <seconds> <out.wav> [break [radio.bin]]
//        "break": start a few seconds before the next break; radio.bin: a bank from tools/radio/radio_studio.py
//   radio_demo time                                          (cost of the radio vs the rest of the sound, per second)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include "Arduino.h"
#include "M5Unified.h"
#include "audio.h"
#include "radio.h"
#include "scene.h"
#include "universe.h"
#include "journey.h"
#include "terrain.h"
#include "dsp.h"
using namespace mantis;
namespace mantis { extern float g_radioSolo[8]; }
uint32_t esp_random() { return 1; }
M5Class M5; M5Canvas* g_hostLastCanvas = nullptr;
uint32_t millis() { return 0; } uint32_t micros() { return 0; } void delay(uint32_t) {}

static void wav(const char* path, const std::vector<int16_t>& s) {
  FILE* f = fopen(path, "wb");
  uint32_t n = s.size() * 2, sr = AUDIO_SR, br = sr * 2;
  fwrite("RIFF", 1, 4, f); uint32_t c = 36 + n; fwrite(&c, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  uint32_t fl = 16; uint16_t fmt = 1, ch = 1, ba = 2, bits = 16;
  fwrite(&fl, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f); fwrite(s.data(), 2, s.size(), f); fclose(f);
}

int main(int argc, char** argv) {
  dsp::init();
  universeInit(3); journeyInit(3); terrainInit(3); sceneInit(3);
  setTimeOfDay(0.45f, 0, 0);
  if (argc > 1 && !strcmp(argv[1], "time")) {
    audioBegin();
    static int16_t out[512];
    const int secs = 60, blocks = secs * AUDIO_SR / 512;
    double base = 0, with = 0;
    for (int pass = 0; pass < 2; pass++) {
      for (int st = 0; st < 4; st++) {
        radioSetStation(pass ? (uint8_t)st : RADIO_OFF);
        auto a = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; b++) audioRender(out, 512);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        if (!pass) base += ms; else with += ms;
        if (pass) printf("station %d: %.1f ms of work per %d s of sound (%.3f%% of real time)\n", st, ms, secs, ms / (secs * 10.0));
      }
    }
    printf("radio off: %.1f ms per %d s\n", base / 4, secs);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "parts")) {
    radioInit();                // each part's loudness, station by station
    static const char* P[8] = {"strings", "piano", "bass", "lead", "kick", "snare", "cymbals", "record"};
    for (int st = 1; st <= 3; st++) {
      printf("station %d:", st);
      for (int k = 0; k < 8; k++) {
        radioInit();
        for (int j = 0; j < 8; j++) g_radioSolo[j] = j == k ? 1.f : 0.f;
        radioSetStation((uint8_t)st); radioSetVolume(RADIO_VOL_MAX);
        static float b2[512], r2[512]; double e = 0; long c = 0;
        for (int b = 0; b < 40 * AUDIO_SR / 512; b++) { memset(b2, 0, sizeof b2); radioBlock(b2, r2, 512); for (int i = 0; i < 512; i++) { e += b2[i] * b2[i]; c++; } }
        printf("  %s %.4f", P[k], sqrt(e / c));
        radioSetStation(RADIO_OFF); for (int b = 0; b < 40; b++) { memset(b2, 0, sizeof b2); radioBlock(b2, r2, 512); }
      }
      printf("\n");
    }
    return 0;
  }
  static std::vector<uint8_t> bank;
  if (argc > 5) {
    FILE* bf = fopen(argv[5], "rb");
    if (!bf) { printf("can't open %s\n", argv[5]); return 1; }
    int ch; while ((ch = fgetc(bf)) != EOF) bank.push_back((uint8_t)ch);
    fclose(bf);
    radioUseBank(bank.data(), (uint32_t)bank.size());
    printf("bank: %d clips\n", radioClipCount());
  }
  radioInit();
  int st = atoi(argv[1]); float secs = atof(argv[2]);
  bool brk = argc > 4 && !strcmp(argv[4], "break");
  radioSetStation((uint8_t)st); radioSetVolume(RADIO_VOL_MAX);
  static float buf[512], rev[512];
  for (int i = 0; i < 20; i++) { memset(buf, 0, sizeof buf); radioBlock(buf, rev, 512); }   // past the tuning static
  if (brk) radioSkipToBreak((uint8_t)st);
  std::vector<int16_t> out;
  float peak = 0, sum = 0; long cnt = 0, clip = 0;
  int blocks = (int)(secs * AUDIO_SR / 512);
  for (int b = 0; b < blocks; b++) {
    memset(buf, 0, sizeof buf);
    radioBlock(buf, rev, 512);
    for (int i = 0; i < 512; i++) {
      float x = buf[i] * 1.6f;                 // the master bus's makeup, roughly
      peak = fmaxf(peak, fabsf(x)); sum += x * x; cnt++;
      if (fabsf(x) > 0.99f) clip++;
      out.push_back((int16_t)(fmaxf(-1.f, fminf(1.f, x)) * 32000.f));
    }
  }
  wav(argv[3], out);
  printf("%s: %.0f s, peak %.2f, rms %.3f, clipped samples %ld\n", argv[3], secs, peak, sqrtf(sum / cnt), clip);
}
