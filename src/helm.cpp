#pragma GCC optimize("Os")
#include <stdlib.h>
#include "helm.h"
#include "haptics.h"
#include <M5Unified.h>
#include <math.h>
#include <string.h>

namespace mantis {

// ======================= TUNING =======================
constexpr float STEER_DEADZONE_DEG = 2.0f;
constexpr float STEER_FULL_DEG     = 18.f;
constexpr float THROTTLE_DEAD_DEG  = 3.f;     // tip less than this: neutral (hold speed decay)
constexpr float THROTTLE_FULL_DEG  = 20.f;
constexpr float STICK_EXPO         = 0.45f;
constexpr bool  INVERT_STEER       = false;
constexpr bool  INVERT_THROTTLE    = false;  // false: tip top away = gas
constexpr float TURN_RATE          = 2.15f;  // rad/s at full stick
constexpr float ATTITUDE_MASS      = 0.10f;
constexpr float STICK_SMOOTH       = 0.05f;
constexpr float MAX_CAM_PITCH      = 0.22f;
constexpr float LEVEL_TIME         = 0.25f;
constexpr float SLIDE_YAW          = 0.012f;
constexpr uint32_t LONG_PRESS_MS   = 1000;
constexpr uint32_t TAP_MS          = 450;
// ======================================================

static float s_yaw = 0.f, s_pitch = 0.f;
static float s_throttle = 0.f, s_brake = 0.f;
static float s_rateYaw = 0.f, s_steer = 0.f;
static float s_tiltX = 0.f, s_tiltY = 0.f;
static float s_neutralPitch = 0.f, s_neutralRoll = 0.f;
static bool s_neutralValid = false;
static int s_settle = 0;
static float s_levelT = 0.f;
static bool s_handbrake = false;
static bool s_touchDown = false, s_dragging = false, s_sliding = false, s_longFired = false;
static int s_x0 = 0, s_y0 = 0, s_lx = 0, s_ly = 0;
static uint32_t s_t0 = 0;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

void helmInit(float yaw) {
    s_yaw = yaw; s_pitch = 0.f;
    s_throttle = 0.f; s_brake = 0.f;
    s_rateYaw = 0.f; s_steer = 0.f; s_tiltX = s_tiltY = 0.f;
    s_neutralValid = false; s_settle = 0; s_levelT = 0.f;
    s_handbrake = false;
    s_touchDown = s_dragging = s_sliding = s_longFired = false;
}

float helmYaw() { return s_yaw; }
float helmPitch() { return s_pitch; }
float helmThrottle() { return s_throttle; }
float helmBrake() { return s_brake; }
float helmSteer() { return s_steer; }
float helmTurnRate() { return s_rateYaw; }
bool helmDragging() { return s_dragging; }
bool helmHandbrake() { return s_handbrake; }
void helmSetAttitude(float yaw, float pitch) { s_yaw = yaw; s_pitch = pitch; s_rateYaw = 0.f; }
void helmNudge(float dyaw, float dpitch) {
    s_yaw += dyaw;
    s_pitch = clampf(s_pitch + dpitch, -MAX_CAM_PITCH, MAX_CAM_PITCH);
}

static void attitudeFrom(float ax, float ay, float az, float& pitch, float& roll) {
    pitch = atan2f(az, ay);
    roll = atan2f(ax, sqrtf(ay * ay + az * az));
}
static void captureNeutral() {
    if (M5.Imu.update()) {
        auto d = M5.Imu.getImuData();
        attitudeFrom(d.accel.x, d.accel.y, d.accel.z, s_neutralPitch, s_neutralRoll);
        s_neutralValid = true;
    }
}

void helmLevel() {
    captureNeutral();
    s_tiltX = s_tiltY = 0.f; s_rateYaw = 0.f; s_steer = 0.f;
    s_levelT = 1.0f;
}

void helmUpdate(float dt, bool driving, HelmEvents& ev) {
    memset(&ev, 0, sizeof(ev));
    M5.update();

    static bool aWas = false, bWas = false, cWas = false;
    bool a = M5.BtnA.isPressed(), b = M5.BtnB.isPressed(), c = M5.BtnC.isPressed();
    if (a && !aWas) { ev.aClick = true; ev.anyButton = true; }
    if (b && !bWas) { ev.bClick = true; ev.anyButton = true; }
    if (c && !cWas) { ev.cClick = true; ev.anyButton = true; }
    if (a && aWas) ev.aHold = true;
    if (b && bWas) ev.bHold = true;
    if (c && cWas) ev.cHold = true;
    aWas = a; bWas = b; cWas = c;

    // C = handbrake (held)
    s_handbrake = c;

    if (ev.bClick && driving) helmLevel();

    // ---- tilt ----
    if (s_settle < 12) { s_settle++; if (s_settle == 8) captureNeutral(); }
    float rawPitch = 0.f, rawRoll = 0.f;
    if (M5.Imu.update()) {
        auto d = M5.Imu.getImuData();
        attitudeFrom(d.accel.x, d.accel.y, d.accel.z, rawPitch, rawRoll);
        if (!s_neutralValid) {
            s_neutralPitch = rawPitch; s_neutralRoll = rawRoll; s_neutralValid = true;
        }
    }
    float dPitch = rawPitch - s_neutralPitch;
    float dRoll  = rawRoll  - s_neutralRoll;
    if (INVERT_THROTTLE) dPitch = -dPitch;
    if (INVERT_STEER) dRoll = -dRoll;

    auto stick = [](float deg, float dead, float full) {
        float a = fabsf(deg);
        if (a < dead) return 0.f;
        float t = clampf((a - dead) / (full - dead), 0.f, 1.f);
        float e = t * t * (3.f - 2.f * t);
        e = e * (1.f - STICK_EXPO) + e * e * STICK_EXPO;
        return (deg < 0.f ? -1.f : 1.f) * e;
    };

    float steerStick = stick(dRoll * (180.f / 3.14159265f), STEER_DEADZONE_DEG, STEER_FULL_DEG);
    // Tip top away (positive pitch in our attitude) = gas; toward you = brake
    float tip = stick(dPitch * (180.f / 3.14159265f), THROTTLE_DEAD_DEG, THROTTLE_FULL_DEG);

    float aS = 1.f - expf(-dt / fmaxf(STICK_SMOOTH, 0.001f));
    s_tiltX += (steerStick - s_tiltX) * aS;
    s_tiltY += (tip - s_tiltY) * aS;

    if (s_levelT > 0.f) {
        s_levelT -= dt;
        float k = clampf(s_levelT / LEVEL_TIME, 0.f, 1.f);
        s_tiltX *= k; s_tiltY *= k;
    }

    s_steer = s_tiltX;
    if (s_tiltY >= 0.f) {
        s_throttle = s_tiltY;
        s_brake = 0.f;
    } else {
        s_throttle = 0.f;
        s_brake = -s_tiltY;
    }

    float wantRate = s_steer * TURN_RATE;
    float aR = 1.f - expf(-dt / fmaxf(ATTITUDE_MASS, 0.001f));
    s_rateYaw += (wantRate - s_rateYaw) * aR;

    if (driving) {
        s_yaw += s_rateYaw * dt;
        // mild camera pitch follows tip
        float camWant = s_tiltY * MAX_CAM_PITCH * 0.35f;
        s_pitch += (camWant - s_pitch) * aR;
        s_pitch = clampf(s_pitch, -MAX_CAM_PITCH, MAX_CAM_PITCH);
    }

    // ---- touch (steer only, no throttle slider) ----
    auto touch = M5.Touch.getDetail();
    bool down = touch.isPressed();
    int tx = touch.x, ty = touch.y;

    if (down && !s_touchDown) {
        s_touchDown = true; s_t0 = millis(); s_x0 = s_lx = tx; s_y0 = s_ly = ty;
        s_dragging = s_sliding = s_longFired = false;
    } else if (down && s_touchDown) {
        int dx = tx - s_lx, dy = ty - s_ly;
        if (!s_sliding && (abs(dx) + abs(dy) > 6)) s_sliding = true;
        if (s_sliding && driving) {
            s_yaw += (float)dx * SLIDE_YAW;
            s_lx = tx; s_ly = ty;
        }
        if (!s_longFired && !s_sliding && (millis() - s_t0) > LONG_PRESS_MS) {
            s_longFired = true; ev.longPress = true; ev.anyButton = true;
        }
    } else if (!down && s_touchDown) {
        if (!s_sliding && !s_longFired && (millis() - s_t0) < TAP_MS) {
            ev.tap = true; ev.tapX = s_x0; ev.tapY = s_y0; ev.anyButton = true;
        }
        s_touchDown = s_sliding = false;
    }
}

}
