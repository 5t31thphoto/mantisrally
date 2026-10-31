#pragma GCC optimize("Os")
#include "helm.h"
#include <M5Unified.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

namespace mantis {

// ======================= TUNING =======================
constexpr float STEER_DEAD_DEG  = 1.0f;
constexpr float STEER_FULL_DEG  = 24.f;    // a wheel's worth of tilt
constexpr float STEER_EXPO      = 0.55f;   // fine near the centre, quick at the ends
constexpr float TIP_DEAD_DEG    = 2.0f;
constexpr float TIP_FULL_DEG    = 16.f;
constexpr float SMOOTH          = 0.03f;   // s
constexpr bool  INVERT_STEER    = true;    // M5's roll is + leaning left on the Core2
constexpr bool  INVERT_TIP      = false;   // + = top away = throttle
constexpr uint32_t HOLD_B_MS    = 650;
constexpr uint32_t MUTE_HOLD_MS = 1500;
constexpr uint32_t TAP_MS       = 400;
// ======================================================

static float s_steer = 0.f, s_tip = 0.f;
static float s_nPitch = 0.f, s_nRoll = 0.f;
static bool s_nValid = false;
static int s_settle = 0;
static bool s_hb = false;
static uint32_t s_bDown = 0; static bool s_bWas = false, s_bHeld = false;
static bool s_aWas = false;
static bool s_touch = false, s_moved = false, s_muteFired = false;
static int s_tx0 = 0, s_ty0 = 0; static uint32_t s_tt0 = 0;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static void attitude(float ax, float ay, float az, float& pitch, float& roll) {
    pitch = atan2f(az, ay);
    roll = atan2f(ax, sqrtf(ay * ay + az * az));
}
static bool readAttitude(float& p, float& r) {
    if (!M5.Imu.update()) return false;
    auto d = M5.Imu.getImuData();
    attitude(d.accel.x, d.accel.y, d.accel.z, p, r);
    return true;
}

void helmInit() { s_steer = s_tip = 0.f; s_nValid = false; s_settle = 0; s_hb = false; }
void helmRecenter() { float p, r; if (readAttitude(p, r)) { s_nPitch = p; s_nRoll = r; s_nValid = true; } s_steer = 0.f; }
float helmSteer() { return s_steer; }
float helmThrottle() { return s_tip > 0.f ? s_tip : 0.f; }
float helmBrake() { return s_tip < 0.f ? -s_tip : 0.f; }
float helmTip() { return s_tip; }
bool helmHandbrake() { return s_hb; }

static float stick(float deg, float dead, float full, float expo) {
    float a = fabsf(deg);
    if (a < dead) return 0.f;
    float u = clampf((a - dead) / (full - dead), 0.f, 1.f);
    float e = u * (1.f - expo) + u * u * expo;
    return deg < 0.f ? -e : e;
}

void helmUpdate(float dt, HelmEvents& ev) {
    memset(&ev, 0, sizeof(ev));
    M5.update();
    bool a = M5.BtnA.isPressed(), b = M5.BtnB.isPressed(), c = M5.BtnC.isPressed();
    if (a && !s_aWas) ev.horn = true;
    s_aWas = a;
    s_hb = c;
    uint32_t now = millis();
    if (b && !s_bWas) { s_bDown = now; s_bHeld = false; }
    if (b && !s_bHeld && now - s_bDown > HOLD_B_MS) { s_bHeld = true; ev.recenter = true; helmRecenter(); }
    if (!b && s_bWas && !s_bHeld) ev.view = true;
    s_bWas = b;

    // ---- tilt ----
    float p = 0.f, r = 0.f;
    if (readAttitude(p, r)) {
        if (!s_nValid || s_settle < 10) { if (++s_settle == 10 || !s_nValid) { s_nPitch = p; s_nRoll = r; s_nValid = true; } }
        float dR = (r - s_nRoll) * 57.2958f, dP = (p - s_nPitch) * 57.2958f;
        if (INVERT_STEER) dR = -dR;
        if (INVERT_TIP) dP = -dP;
        float k = 1.f - expf(-dt / SMOOTH);
        s_steer += (stick(dR, STEER_DEAD_DEG, STEER_FULL_DEG, STEER_EXPO) - s_steer) * k;
        s_tip += (stick(dP, TIP_DEAD_DEG, TIP_FULL_DEG, 0.3f) - s_tip) * k;
    }

    // ---- touch: the picture only (the button strip below it is A/B/C) ----
    auto t = M5.Touch.getDetail();
    bool down = t.isPressed() && t.y < 236 && !(a || b || c);
    if (down && !s_touch) { s_touch = true; s_moved = false; s_muteFired = false; s_tx0 = t.x; s_ty0 = t.y; s_tt0 = now; }
    else if (down && s_touch) {
        if (abs(t.x - s_tx0) + abs(t.y - s_ty0) > 14) s_moved = true;
        if (!s_moved && !s_muteFired && now - s_tt0 > MUTE_HOLD_MS) { s_muteFired = true; ev.mute = true; }
    } else if (!down && s_touch) {
        if (!s_moved && !s_muteFired && now - s_tt0 < TAP_MS) { ev.tap = true; ev.tapX = s_tx0; ev.tapY = s_ty0; }
        s_touch = false;
    }
}

}
