#pragma once
#include <stdint.h>

// The motor is the low end: the Core2 speaker can't move air below ~200 Hz, so weight,
// impacts and the engine live in your hands. Taps, continuous rumble and keyframed gestures
// are max-mixed, remapped above the motor's dead zone, and written only when the level changes.

namespace hx {
void begin();
void pop(float level, float seconds);             // short tap (detents, UI, checkpoint)
void bump(float strength);                        // hit a bank, rock, barrier
void kick(float level);                           // score kick, felt in the hands
void swell();                                     // crest / jump landing breath
void heartbeat();                                 // countdown pulse at stage start
void confirmMute(bool muted);
void horn();                                      // short blast felt
void echo(float strength);                        // unused / reserved
void creak(float loadK);                          // chassis stress under load
void thunder(float power);                        // distant storm
void scanTick(float level);                       // scanner / pace-note read
void scanDone(bool fresh);
void eruption(float power);                       // reserved
void engine(float throttle, float speed);         // continuous: revs, per frame
void rumble(float amount);                        // continuous: gravel / slip, per frame
void update(float dt);
}
