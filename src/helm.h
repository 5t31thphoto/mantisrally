#pragma once
#include <stdint.h>

// Controls. Hold the Core2 like a wheel:
//   tilt left / right  steer            tip the top away  throttle        pull it back  brake (hold at a stop: reverse)
//   A (left button)    horn             B tap  change view (cockpit / eye level / chase)   B hold  re-centre the tilt
//   C (right button)   handbrake (hold)  in the air on the bike: tip forward / back to pitch the bike
//   touch the screen   use what's in front of you (the dealership)        hold the screen 1.5 s  mute
//   double-tap the screen   stuck? back on the road
//   swipe left / right across the screen   the radio dial (off, and the three stations)
//   swipe up / down                        the radio's volume
// The A/B/C buttons are zones of the touch panel below the picture: touches there are buttons only, never taps
// or the mute hold.

namespace mantis {

struct HelmEvents {
    bool horn, view, recenter, mute, cPress;
    bool tap; int tapX, tapY;      // panel pixels (320x240)
    bool doubleTap;                // two deliberate taps in quick succession
    int8_t swipeX, swipeY;         // a quick deliberate swipe: +1 right / -1 left, +1 up / -1 down
};

void  helmInit();
void  helmUpdate(float dt, HelmEvents& ev);
float helmSteer();      // -1 .. 1, + = right
float helmThrottle();   // 0 .. 1
float helmBrake();      // 0 .. 1
float helmTip();        // -1 .. 1 (+ = top away): air pitch on the bike
bool  helmHandbrake();
void  helmRecenter();
void  helmPoll();       // read the touch panel now (between the frame's phases): its taps and swipes come with the next update

}
