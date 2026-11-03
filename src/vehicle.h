#pragma once
#include <stdint.h>

// Vehicles: one parameter table, one physics model (bicycle model with load transfer, Pacejka-shaped tyres,
// friction circles per axle, drivetrain split, turbo, auto box, suspension with real air time, bike lean and
// wheelies). A new unlockable is a new row in VEHICLES (vehicle.cpp) plus its sprites (sprites.cpp).

namespace mantis {

enum VehicleId : uint8_t { V_STI = 0, V_JEEP, V_VETTE, V_BIKE, V_COUNT };
enum : uint8_t { DRIVE_AWD, DRIVE_RWD, DRIVE_4WD };
enum : uint8_t { COCKPIT_RALLY, COCKPIT_JEEP, COCKPIT_VETTE, COCKPIT_BIKE };
enum : uint8_t { ENG_BOXER4T, ENG_I6, ENG_V8, ENG_2STROKE };

struct VehicleSpec {
    const char* name;          // shown in the store
    const char* shortName;     // shown in the garage list
    const char* blurb1;        // store: what it is
    const char* blurb2;        // store: a hint (never the trick itself: the unlocks are surprises)
    bool bike;
    // chassis
    float mass, wheelbase, frontWeight, cgHeight, track, yawInertiaK;
    // steering
    float lockDeg, steerRefSpeed, steerRate, assist;     // assist = auto counter-steer in slides
    // engine / box
    uint8_t engine;            // sound model
    float torque, peakRpm, idleRpm, redline, launchRpm, turbo;   // turbo 0 = n/a, else lag seconds
    int   gears; float ratio[6]; float finalDrive, wheelRadius, shiftTime;
    // drivetrain / brakes
    uint8_t drive; float frontSplit, brakeG, brakeBias; bool abs;
    // tyres: grip per SURF_*, and shape
    float mu[8]; float tarmacC, looseC, peakSlip, rollMul;
    // aero / suspension
    float cdA, springHz, damping, rideHeight, travel;
    // roles
    float offroadBand;         // metres past the road edge before the off-road clock (999 = free roam)
    float fordDepth;           // water it can wade (m)
    float eyeHeight, eyeBack;  // cockpit camera
    uint8_t cockpit;
    // store
    uint8_t priceVehicle; float priceMiles;   // paid in miles driven in priceVehicle
    float statSpeed, statGrip, statOffroad;   // 0..1 bars in the store
};

const VehicleSpec& vehicleSpec(uint8_t id);

struct VehicleInput { float steer, throttle, brake; bool handbrake; float airPitch; };

struct VehicleState {
    float x, y, z, yaw;        // CG position (y = CG height), heading
    float u, v, r, vy;         // body-frame velocity (forward, right), yaw rate, vertical velocity
    float steer;               // road-wheel angle (rad)
    float rpm, boost, shiftT, revHold, limiterT;
    int gear; bool reverse;
    float spin, slipF, slipR;  // driven-wheel spin 0..1, axle saturation 0..1
    float latG, longG;
    bool air; float airTime, airPitch, airPitchV;
    float pitch, roll, pitchV, rollV;   // body angles (camera): chassis on its springs + ground attitude
    float groundPitch, groundRoll;
    float lean, wheelie;       // bike
    float load, comp;          // normal load factor, suspension compression 0..1
    float lastGY;
    // per-frame events (read and cleared by the game)
    float evLand, evBump, evShift, evSplash, evBottom;
    uint8_t surf, mat; float water, across, half, gripNow;
    float speed;               // |velocity| m/s
    float wheelRot;            // for the store sprite / dash
};

void vehicleReset(VehicleState& s, uint8_t id, float x, float z, float yaw);
void vehicleStep(VehicleState& s, uint8_t id, const VehicleInput& in, float dt);
// Collision response against a round obstacle (tree, rock) at (ox, oz) radius rad. Returns impact speed.
float vehicleHitPost(VehicleState& s, uint8_t id, float ox, float oz, float rad);

}
