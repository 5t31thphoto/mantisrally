#pragma once
#include <stdint.h>

// Shared world state — same idea as MantisPlanet: one struct the main loop writes,
// renderer + audio both read. Floats only (atomic 32-bit stores on ESP32).

namespace mantis {

struct Scene {
    float t, dt, tod, yaw, pitch, distance, speed, throttle;
    float x, y, z;
    float altitude;
    float cruiseFor, stillFor;

    float sunElev, sunAz;
    // weather (Planet-style)
    float cloud, rain, storm, fog, wind, wet, dust, snow;
    float rainAgo;          // seconds since last proper rain
    float flash;            // lightning 0..1
    float boltAge;
    float dayLight, light;

    uint8_t stage;          // current biome id
    float grip, roughness;
    float engineLoad, slip, handbrake;
    float checkpointNear;
    float dustTrail, sparks;

    // moments
    float jump, crest, passBy, horn, finish;
    float beams;            // crepuscular strength
    float rainbow;
    bool  lights;
    uint32_t daySeed;
};

extern Scene g_scene;

void sceneInit();
void sceneUpdate(float t, float dt,
                 float x, float y, float z,
                 float yaw, float pitch,
                 float distance, float speed, float throttle,
                 float grip, float slip, bool handbrake);

}
