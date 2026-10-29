#pragma once
#include <stdint.h>

// Roadside objects, checkpoints, spectators, rival cars. Collisions and scans live here.

namespace mantis {

enum ObjType : uint8_t {
    OBJ_NONE = 0,
    OBJ_TREE, OBJ_ROCK, OBJ_SIGN, OBJ_BARRIER, OBJ_SPECTATOR,
    OBJ_CHECKPOINT, OBJ_RIVAL, OBJ_HAYBALE, OBJ_BRIDGE, OBJ_COUNT
};

struct FieldObj {
    uint8_t type, var;
    float x, y, z;      // y = height of base
    float a, b;         // size / height extras
    float yaw;
    bool  on;
};

constexpr int FIELD_N = 48;
constexpr int RIVAL_N = 4;

void fieldInit();
void fieldUpdate(float carX, float carZ, float carYaw, float speed, float dt);

// Move the car, collide with solid objects and banks. Returns fraction of move that happened.
// *bump = impact strength 0..1; *surface from terrain.
float fieldMove(float& x, float& y, float& z, float& yaw,
                float dx, float dy, float dz, float dyaw,
                float* bump, float* grip);

const FieldObj* fieldObjects();
float fieldNearest(uint8_t objType);
void  fieldScare(float x, float z, float radius);   // horn / pass-by

}
