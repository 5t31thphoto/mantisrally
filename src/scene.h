#pragma once
#include <stdint.h>

// The sky, the light and the weather: one struct the game writes, the renderer and the audio read.

namespace mantis {

struct Scene {
    float t, dt, tod, sunElev, sunAz, night, dayLight, moon;
    float cloud, rain, storm, fog, wind, windAz, wet, dust, snow, mist;
    float flash, boltAz, boltAge; uint32_t boltSeed; float thunderIn, thunderPow;
    float rainbow, aurora, beams, fireflies, leaves;
    float speed, distance, x, z, yaw;
    uint8_t biomeA, biomeB; float blend;
    float lights;          // headlights 0..1 (automatic)
    float awe;             // how spectacular this moment is (the score swells)
    float front;           // the weather system's wander 0 (clear) .. 1 (the worst this biome does)
};
extern Scene g_scene;

void sceneInit(uint32_t seed);
void sceneUpdate(float t, float dt, float x, float z, float yaw, float speed, float distance);
// previews / tests
void sceneForceWeather(float cloud, float rain, float fog, float storm, float snow);

}
