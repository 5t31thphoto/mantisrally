#pragma GCC optimize("Os")
#include "photo.h"
#include "render.h"
#include "terrain.h"
#include "display.h"
#include "art.h"
#include "storage.h"
#include "sprites.h"
#include <math.h>
#include <string.h>

namespace mantis {

static bool s_on = false;
static uint8_t s_angle = SHOT_RIGHT, s_veh = 0;
static float s_cx, s_cz, s_cg, s_yaw;         // the parked car: ground point and heading
static float s_pan = 0.f, s_tilt = 0.f, s_freeYaw = 0.f;
static float s_label = 0.f, s_flash = 0.f;

// Each vehicle's drawing covers this much car (length for the side drawing, width for the rear); the camera
// stands where the drawing's size matches the world (the art is at 320 x 240, focal 192 there).
struct Dims { float len, wid, hgt; };
static const Dims DIMS[V_COUNT] = {{4.42f, 1.80f, 1.48f}, {3.90f, 1.80f, 1.95f}, {4.60f, 2.05f, 1.85f}, {2.15f, 0.82f, 1.25f}};
static constexpr float F320 = 192.f;
static constexpr float PAN_MAX = 0.17f, TILT_MAX = 0.13f;   // turn +-10 deg, tilt +-7.5 deg: the drawing stays true

bool photoActive() { return s_on; }

void photoEnter(const VehicleState& car, uint8_t vehicle) {
    s_on = true; s_veh = vehicle; s_angle = SHOT_RIGHT;
    s_cx = car.x; s_cz = car.z; s_yaw = car.yaw;
    s_cg = terrainHeight(car.x, car.z);
    s_pan = s_tilt = 0.f; s_freeYaw = car.yaw; s_label = 1.5f; s_flash = 0.f;
}
void photoExit() { s_on = false; renderPhotoClear(); g_view.photo = false; }
void photoNextAngle() { s_angle = (uint8_t)((s_angle + 1) % SHOT_N); s_pan = s_tilt = 0.f; s_freeYaw = s_yaw; s_label = 1.2f; }

void photoUpdate(float dt, float steer, float tip) {
    float fx = cosf(s_yaw), fz = sinf(s_yaw), rx = -fz, rz = fx;
    const Dims& D = DIMS[s_veh];
    const art::SideArt& sa = art::SIDE_ART[s_veh];
    const art::SideArt& ra = art::REAR_ART[s_veh];
    float k = 1.f - expf(-dt * 6.f);
    ViewInfo& v = g_view;
    v.photo = true;
    if (s_angle == SHOT_FREE) {                                      // the driver's seat: all the way round
        s_freeYaw += steer * 1.5f * dt;
        s_tilt += (tip * 1.0f - s_tilt) * k;
        const VehicleSpec& V = vehicleSpec(s_veh);
        v.pX = s_cx + fx * V.eyeBack; v.pZ = s_cz + fz * V.eyeBack; v.pY = s_cg + V.eyeHeight;
        v.pYaw = s_freeYaw; v.pPitch = s_tilt;
    } else {
        s_pan += (steer * PAN_MAX - s_pan) * k;
        s_tilt += (tip * TILT_MAX - s_tilt) * k;
        float dist, h, yaw, ox, oz;
        if (s_angle == SHOT_REAR) {
            dist = F320 * D.wid / ra.w; h = D.hgt * 0.55f;
            ox = -fx * dist; oz = -fz * dist; yaw = s_yaw;
        } else {
            dist = F320 * D.len / sa.w; h = D.hgt * 0.5f;
            float side = s_angle == SHOT_RIGHT ? 1.f : -1.f;          // the drawing faces right: seen from the car's right
            ox = rx * dist * side; oz = rz * dist * side;
            yaw = s_yaw - side * 1.5707963f;
        }
        float gy = terrainHeight(s_cx + ox, s_cz + oz);
        v.pX = s_cx + ox; v.pZ = s_cz + oz; v.pY = fmaxf(s_cg + h, gy + 0.4f);
        v.pYaw = yaw + s_pan; v.pPitch = s_tilt;
    }
    if (s_label > 0.f) s_label -= dt;
    if (s_flash > 0.f) s_flash -= dt;
}

// viewfinder marks: corners, and the angle's name for a moment (never in the photo)
static void marks() {
    uint16_t* o = overlayBuffer();
    if (!o) return;
    const uint16_t W = 0xFFFF;
    for (int k = 0; k < 18; k++) {
        int a = 10 + k;
        o[10 * 320 + a] = o[a * 320 + 10] = W;
        o[10 * 320 + 309 - k] = o[a * 320 + 309] = W;
        o[229 * 320 + a] = o[(229 - k) * 320 + 10] = W;
        o[229 * 320 + 309 - k] = o[(229 - k) * 320 + 309] = W;
    }
    if (s_label > 0.f) {
        static const char* NAMES[SHOT_N] = {"SIDE", "REAR", "OTHER SIDE", "LOOK AROUND"};
        uint8_t tmp[60 * 7]; memset(tmp, 0, sizeof tmp);
        int w = font3(tmp, 60, 7, 1, 1, NAMES[s_angle], 1, 0);
        int x0 = 160 - w, y0 = 18;
        for (int y = 0; y < 7; y++) for (int x = 0; x < 60; x++) if (tmp[y * 60 + x])
            for (int q = 0; q < 4; q++) { int X = x0 + x * 2 + (q & 1), Y = y0 + y * 2 + (q >> 1); if ((unsigned)X < 320u && (unsigned)Y < 240u) o[Y * 320 + X] = W; }
    }
    if (s_flash > 0.f) {                                              // the shutter's flash
        for (int y = 0; y < 240; y += (s_flash > 0.08f ? 1 : 2)) for (int x = 0; x < 320; x++) o[y * 320 + x] = 0xFFFF;
    }
    overlayEnable(true);
}

int photoCompose(bool shoot) {
    if (!s_on) return 0;
    overlayClear();
    if (s_angle != SHOT_FREE) {
        PhotoArt a;
        bool rear = s_angle == SHOT_REAR;
        const art::SideArt& art = rear ? art::REAR_ART[s_veh] : art::SIDE_ART[s_veh];
        a.px = art.px; a.w = art.w; a.h = art.h; a.mirror = s_angle == SHOT_LEFT;
        a.x = s_cx; a.y = s_cg; a.z = s_cz;
        a.shadowW = rear ? DIMS[s_veh].wid * 1.1f : DIMS[s_veh].len * 0.95f;
        renderPhotoCompose(a);
    }
    int n = 0;
    if (shoot) {
        n = storagePhoto(compositeRow);                              // exactly the frame: world + car, no marks
        s_flash = 0.15f;
        photoNextAngle();
    }
    marks();
    return n;
}

}
