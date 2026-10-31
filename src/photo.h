#pragma once
#include <stdint.h>
#include "vehicle.h"

// Photo mode. Stopped, press A: the screen becomes a viewfinder. Tilt and tip turn and tilt the camera (only
// that: the car is your hero drawing, so the camera never walks around it). A takes the photo (it moves on to the
// next angle), C changes the angle, B goes back to driving.
//   angles: the car's right side (the side drawing), behind it (the rear drawing), its left side (the side drawing
//   mirrored), and from the driver's seat with no car in it (turn all the way round, look up and down)
// Photos: /images/rally/rally_NNNN.mg on the SD card.

namespace mantis {

enum : uint8_t { SHOT_RIGHT = 0, SHOT_REAR, SHOT_LEFT, SHOT_FREE, SHOT_N };

void photoEnter(const VehicleState& car, uint8_t vehicle);
void photoExit();
bool photoActive();
void photoNextAngle();
// per frame, before renderFrame(): camera from the tilt (steer -1..1 turns, tip -1..1 tilts)
void photoUpdate(float dt, float steer, float tip);
// after renderFrame(): sets the car into the frame; shoot = take the photo now. Returns the photo number, -1, or 0.
int  photoCompose(bool shoot);

}
