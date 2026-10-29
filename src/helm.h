#pragma once
#include <stdint.h>

// Rally helm: tilt steer, tip-to-throttle, C handbrake.
//  * Lean left / right: steering rate (mass so a resting hand holds a line).
//  * Tip top away: accelerate. Tip top toward you: brake / coast.
//  * Slide finger: direct steer.
//  * C click / hold: handbrake.
//  * B: centre wheel + re-take neutral pose.
//  * A: horn.  Hold A: map.  Long-press: mute.
//  * Tap: scan roadside thing.

namespace mantis {

struct HelmEvents {
    bool tap; int tapX, tapY;
    bool longPress;
    bool aClick, aHold, bClick, bHold, cClick, cHold;
    bool anyButton;
};

void helmInit(float yaw);
void helmUpdate(float dt, bool driving, HelmEvents& ev);

float helmYaw();
float helmPitch();           // camera pitch only
float helmThrottle();        // 0 .. 1 gas from tip-forward
float helmBrake();           // 0 .. 1 from tip-back
float helmSteer();           // -1 .. +1
float helmTurnRate();        // rad/s
bool  helmDragging();
bool  helmHandbrake();       // C held
void  helmLevel();
void  helmSetAttitude(float yaw, float pitch);
void  helmNudge(float dyaw, float dpitch);

}
