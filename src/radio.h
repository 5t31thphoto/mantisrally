#pragma once
#include <stdint.h>

// The car radio: three stations of never-ending procedural music (lo-fi hip-hop, math rock, country/folk) with DJ
// breaks and Mantis Motors ads recorded with tools/radio/radio_studio.py and read from /radio/radio.bin on the SD
// card at boot (no card or no file: each station plays a short musical ident in the breaks instead).
// Every station keeps playing while you're tuned elsewhere (each runs on the same clock), so tuning back in lands
// mid-song, like a real radio. It's synthesised at 12 kHz inside the sound task on core 0 (half the sound's rate: the
// sound of a car speaker) and costs nothing while it's off.

namespace mantis {

enum : uint8_t { RADIO_OFF = 0, RADIO_LOFI, RADIO_MATH, RADIO_FOLK, RADIO_N };
constexpr int RADIO_VOL_MAX = 7;

// game side (any thread; never blocks)
void radioSetStation(uint8_t s);         // RADIO_OFF .. RADIO_FOLK
uint8_t radioStation();
void radioSetVolume(int step);           // 1 .. RADIO_VOL_MAX
int  radioVolume();
void radioSetPaused(bool paused);        // photo mode, the dealership: the radio stops (the stations play on)
const char* radioFreq(uint8_t s);        // "88.7 FM"
const char* radioName(uint8_t s);        // "LO-FI MANTIS"
void radioLoadClips();                   // boot, after storageInit(): the DJ and ad recordings into PSRAM
int  radioClipCount();

// sound task side
void radioInit();                        // buffers (from audioBegin)
void radioBlock(float* dry, float* rev, int n);   // adds the radio to the bus, n samples at AUDIO_SR (n even, <= 512)

#ifdef HOST
// host tools: put a recording bank in from memory, and jump a station to its next break
void radioUseBank(const uint8_t* data, uint32_t bytes);
void radioSkipToBreak(uint8_t s);
#endif

}
