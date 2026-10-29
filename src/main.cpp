#pragma GCC optimize("Os")
// Mantis Rally — infinite branching rally / drift on M5Stack Core2.
// Tip forward = gas, pull back = brake, tilt = steer, C = handbrake.
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <M5Unified.h>
#include "helm.h"
#include "universe.h"
#include "journey.h"
#include "terrain.h"
#include "field.h"
#include "scene.h"
#include "render.h"
#include "display.h"
#include "haptics.h"
#include "audio.h"
#include "databank.h"
#include "ui.h"

// ======================= DRIFT PHYSICS TUNING =======================
static constexpr float MASS          = 1.0f;
static constexpr float MAX_SPEED     = 48.f;     // m/s ~173 km/h
static constexpr float ENGINE_FORCE  = 38.f;     // accel at full throttle
static constexpr float BRAKE_FORCE   = 52.f;
static constexpr float DRAG          = 0.012f;
static constexpr float ROLL_RES      = 0.8f;
static constexpr float STEER_GAIN    = 2.4f;     // yaw response vs steer*speed
static constexpr float GRIP_LAT      = 14.f;     // lateral tire force scale
static constexpr float GRIP_LONG     = 11.f;
static constexpr float HB_REAR_CUT   = 0.22f;    // handbrake rear grip multiplier
static constexpr float SLIP_SMOOTH   = 0.12f;
// ====================================================================

static float carX = 0.f, carY = 0.f, carZ = 0.f;
static float carYaw = 0.f;
static float velX = 0.f, velZ = 0.f;     // world velocity
static float carSpeed = 0.f;             // signed forward speed
static float slipAngle = 0.f;            // rad
static float slipAmt = 0.f;              // 0..1 for FX
static float distanceTravelled = 0.f;
static float stageTime = 0.f;
static bool wasBumped = false;
static float fpsAvg = 30.f;
static float bannerT = 0.f;
static const char* bannerText = nullptr;
static char bannerBuf[64];
static uint8_t lastBiome = 255;
static float camLean = 0.f;

static uint8_t scanKind = 0;
static int scanIdx = -1;
static uint8_t scanSp = 255;
static float scanT = 0.f, scanTick = 0.f, scanDoneT = 0.f;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float wrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static void tryScan(int tx, int ty) {
    using namespace mantis;
    float best = 1e9f;
    int bi = -1; uint8_t bsp = 255;
    const FieldObj* o = fieldObjects();
    for (int i = 0; i < FIELD_N; i++) {
        if (!o[i].on) continue;
        int px, py; float d;
        float h = o[i].type == OBJ_TREE ? o[i].b * 0.5f : o[i].a * 0.4f;
        if (!renderProject(o[i].x, o[i].y + h, o[i].z, px, py, d) || d > 28.f) continue;
        float dd = sqrtf((float)((px - tx) * (px - tx) + (py - ty) * (py - ty)));
        float slack = 28.f + fminf(40.f, 180.f / d);
        if (dd < slack && dd < best) { best = dd; bi = i; bsp = (uint8_t)o[i].type; }
    }
    // also signs
    int sn = 0; const RoadSign* sg = terrainSigns(sn);
    for (int i = 0; i < sn; i++) {
        if (!sg[i].active) continue;
        int px, py; float d;
        if (!renderProject(sg[i].x, terrainHeight(sg[i].x, sg[i].z) + 1.5f, sg[i].z, px, py, d) || d > 30.f) continue;
        float dd = sqrtf((float)((px - tx)*(px-tx)+(py-ty)*(py-ty)));
        if (dd < 36.f && dd < best) { best = dd; bi = -2; bsp = 10; }
    }
    if (bi == -1) { hx::pop(0.08f, 0.01f); return; }
    scanKind = 1; scanIdx = bi; scanSp = bsp; scanT = 0.001f; scanTick = 0.f; scanDoneT = 0.f;
    hx::scanTick(0.45f);
    sfxUi(UI_SCAN_TICK);
}

static void serviceScan(float dt) {
    using namespace mantis;
    if (scanDoneT > 0.f) { scanDoneT -= dt; if (scanDoneT <= 0.f) scanT = 0.f; }
    if (scanKind == 0) return;
    scanT += dt;
    scanTick += dt;
    if (scanTick > 0.18f) { scanTick = 0.f; sfxUi(UI_SCAN_TICK); hx::scanTick(0.2f); }
    if (scanT > 1.0f) {
        databankScan(scanSp);
        sfxUi(UI_SCAN_OK);
        scanDoneT = 2.0f;
        scanKind = 0;
        const ScanCard* c = databankCard(scanSp);
        bannerText = c->name;
        bannerT = 2.2f;
    }
}

void setup() {
    auto cfg = M5.config();
    cfg.internal_imu = true;
    M5.begin(cfg);
    mantis::audioSpeakerBegin();

    uint32_t seed = (uint32_t)(millis() ^ 0xC0FFEEu);
#ifndef HOST
    seed ^= (uint32_t)esp_random();
#endif
    mantis::universeInit(seed);
    mantis::terrainInit(seed);
    mantis::journeyInit(seed);
    mantis::fieldInit();
    mantis::sceneInit();
    mantis::renderInit();
    mantis::displayInit();
    mantis::databankInit();
    mantis::databankLoad();
    mantis::helmInit(0.f);
    mantis::audioBegin();
    hx::begin();

    TrackSample ts = mantis::terrainAt(0.f, 0.f);
    carY = ts.height + 0.4f;
    snprintf(bannerBuf, sizeof(bannerBuf), "%s", mantis::journeyBiome(mantis::journeyCurrent()).name);
    bannerText = bannerBuf;
    bannerT = 3.2f;
    lastBiome = mantis::journeyCurrent();
}

void loop() {
    using namespace mantis;
    static uint32_t lastMs = 0;
    uint32_t now = millis();
    float dt = lastMs ? (now - lastMs) * 0.001f : 0.016f;
    lastMs = now;
    if (dt > 0.05f) dt = 0.05f;
    static float t = 0.f;
    t += dt;

    float inst = dt > 0.0001f ? 1.f / dt : 60.f;
    fpsAvg += (inst - fpsAvg) * 0.08f;

    HelmEvents ev;
    helmUpdate(dt, true, ev);

    if (ev.longPress) { audioSetMuted(!audioMuted()); sfxUi(UI_LEVEL); }
    if (ev.aClick) {
        sfxHorn(0.8f);
        fieldScare(carX, carZ, 18.f);
        hx::horn();
    }
    if (ev.tap) tryScan(ev.tapX, ev.tapY);

    // ---- rally / drift physics ----
    TrackSample ts = terrainAt(carX, carZ);
    float grip = ts.grip;
    float slope = ts.slope;
    float camber = ts.camber;

    float throttle = helmThrottle();
    float brake = helmBrake();
    bool hb = helmHandbrake();
    float steer = helmSteer();

    // Forward / right basis
    float fwdX = cosf(carYaw), fwdZ = sinf(carYaw);
    float rightX = -fwdZ, rightZ = fwdX;

    // Velocity in car space
    float vLong = velX * fwdX + velZ * fwdZ;
    float vLat  = velX * rightX + velZ * rightZ;
    carSpeed = vLong;

    // Slip angle
    float wantSlip = (fabsf(vLong) > 1.0f) ? atanf(vLat / fabsf(vLong)) : 0.f;
    slipAngle += (wantSlip - slipAngle) * (1.f - expf(-dt / SLIP_SMOOTH));

    // Tire grip: handbrake kills rear → more slip allowed
    float gripF = grip;
    float gripR = hb ? grip * HB_REAR_CUT : grip;
    float gripAvg = (gripF + gripR) * 0.5f;

    // Lateral force (simple Pacejka-ish clamp)
    float latLimit = GRIP_LAT * gripAvg;
    float latForce = clampf(-vLat * 6.5f - slipAngle * 18.f + camber * 14.f, -latLimit, latLimit);

    // Longitudinal: engine + brake + drag
    float eng = throttle * ENGINE_FORCE * (1.f - clampf(fabsf(vLong) / MAX_SPEED, 0.f, 1.f));
    float brk = -brake * BRAKE_FORCE * (vLong > 0.f ? 1.f : -1.f);
    if (hb && fabsf(vLong) > 2.f) brk -= (vLong > 0.f ? 1.f : -1.f) * 18.f * grip;
    // gravity on slope: downhill pulls you, uphill fights
    float grav = -slope * 18.f;
    float longForce = eng + brk + grav - vLong * fabsf(vLong) * DRAG - (vLong > 0.f ? ROLL_RES : -ROLL_RES) * grip;

    // Integrate velocity
    float aLong = longForce / MASS;
    float aLat  = latForce / MASS;
    vLong += aLong * dt;
    vLat  += aLat * dt;

    // World velocity
    velX = fwdX * vLong + rightX * vLat;
    velZ = fwdZ * vLong + rightZ * vLat;

    // Yaw from steering + from slip (drift rotation)
    float steerYaw = steer * STEER_GAIN * clampf(fabsf(vLong) / 12.f, 0.15f, 1.f) * gripAvg;
    // Counter-steer / oversteer: when slipping, yaw rate follows slip
    float driftYaw = slipAngle * 1.8f * clampf(fabsf(vLong) / 15.f, 0.f, 1.f);
    if (hb) driftYaw *= 1.6f;
    float yawRate = steerYaw + driftYaw * 0.35f;
    // clamp crazy spins
    yawRate = clampf(yawRate, -2.8f, 2.8f);

    float dx = velX * dt;
    float dz = velZ * dt;
    float bump = 0.f, g = grip;
    float frac = fieldMove(carX, carY, carZ, carYaw, dx, 0.f, dz, yawRate * dt, &bump, &g);
    grip = g;
    if (frac < 0.95f) {
        // kill velocity into the obstacle
        velX *= frac; velZ *= frac;
    }

    slipAmt = clampf(fabsf(slipAngle) * 1.4f + (hb ? 0.35f : 0.f), 0.f, 1.f);

    if (bump > 0.12f && !wasBumped) {
        sfxBump(bump);
        hx::bump(bump);
        velX *= (1.f - bump * 0.4f);
        velZ *= (1.f - bump * 0.4f);
        wasBumped = true;
    } else if (bump < 0.05f) wasBumped = false;

    hx::engine(throttle, fabsf(carSpeed));
    hx::rumble(ts.roughness * 0.45f + slipAmt * 0.85f + (hb ? 0.25f : 0.f));

    distanceTravelled += sqrtf(dx * dx + dz * dz);
    stageTime += dt;

    // Road graph
    terrainUpdateRoad(carX, carZ, carYaw, fabsf(carSpeed), dt);
    bool tookLeft = false;
    if (terrainBranchCommitted(tookLeft)) {
        uint8_t dest = journeyNext();
        snprintf(bannerBuf, sizeof(bannerBuf), "→ %s", journeyBiome(dest).name);
        bannerText = bannerBuf;
        bannerT = 2.5f;
        sfxUi(UI_SCAN_OK);
        hx::pop(0.3f, 0.04f);
    }

    journeyUpdate(carX, carZ, fabsf(carSpeed), dt);
    if (journeyCurrent() != lastBiome) {
        lastBiome = journeyCurrent();
        snprintf(bannerBuf, sizeof(bannerBuf), "%s", journeyBiome(lastBiome).name);
        bannerText = bannerBuf;
        bannerT = 2.8f;
        stageTime = 0.f;
        sfxMoment(MOMENT_STAGE);
    }

    fieldUpdate(carX, carZ, carYaw, fabsf(carSpeed), dt);
    sceneUpdate(t, dt, carX, carY, carZ, carYaw, helmPitch(),
                distanceTravelled, fabsf(carSpeed), throttle, grip, slipAmt, hb);
    serviceScan(dt);

    g_hud.throttle = throttle;
    g_hud.scanT = scanT;
    g_hud.banner = bannerT > 0.f ? bannerText : nullptr;
    g_hud.bannerA = bannerT > 0.f ? fminf(1.f, bannerT) : 0.f;
    if (bannerT > 0.f) bannerT -= dt;
    g_hud.showFps = false;
    g_hud.fps = (int)(fpsAvg + 0.5f);
    g_hud.muted = audioMuted();
    g_hud.muteIcon = false;

    // Camera: lean into turns / drifts, dip on brake, lift on gas
    float leanWant = steer * 0.12f + slipAngle * 0.25f;
    camLean += (leanWant - camLean) * (1.f - expf(-dt * 6.f));
    float camPitch = helmPitch() - brake * 0.06f + throttle * 0.03f - slipAmt * 0.04f;
    renderFrame(carX, carY, carZ, carYaw + camLean * 0.35f, camPitch, t);
    displayPresent();
}

#ifndef HOST
#else
int main() {
    setup();
    for (;;) loop();
    return 0;
}
#endif
