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
#include <stdint.h>

// ======================= DRIFT PHYSICS TUNING =======================
static constexpr float MASS          = 1.0f;
static constexpr float MAX_SPEED     = 48.f;     // m/s ~173 km/h
static constexpr float ENGINE_FORCE  = 62.f;     // accel at full throttle
static constexpr float BRAKE_FORCE   = 52.f;
static constexpr float DRAG          = 0.008f;
static constexpr float ROLL_RES      = 0.22f;
static constexpr float STEER_GAIN    = 2.6f;     // yaw response vs steer*speed
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
static float offRoadTimer = 0.f;
static bool offRoadActive = false;
static float fpsAvg = 30.f;
static float bannerT = 0.f;
static const char* bannerText = nullptr;
static char bannerBuf[64];
static uint8_t lastBiome = 255;
static float camLean = 0.f;
static float hintTimer = 0.f;
static float driftTime = 0.f, driftPoints = 0.f;
static uint32_t totalScore = 0;
static bool driftWasScoring = false;

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

static void overlayRect(uint16_t* b, int x, int y, int w, int h, uint16_t color) {
    if (!b) return;
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > 320) w = 320 - x;
    if (y + h > 240) h = 240 - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy) {
        uint16_t* row = b + yy * 320 + x;
        for (int xx = 0; xx < w; ++xx) row[xx] = color;
    }
}

static void overlayText(uint16_t* b, int x, int y, const char* text, uint16_t color, int scale=1) {
    if (b && text && *text) mantis::uiText(b, 320, 240, x, y, text, color, scale, true);
}

// The world renderer is intentionally text-free and tiny; this native-resolution
// overlay supplies the actual driving instruments without spending palette slots.
static void drawGameOverlay() {
    using namespace mantis;
    uint16_t* b = overlayBuffer();
    if (!b) return;
    overlayClear();
    const uint16_t panel = rgb565(8, 12, 20);
    const uint16_t white = rgb565(242, 246, 250);
    const uint16_t mint = rgb565(0, 220, 180);
    const uint16_t lime = rgb565(170, 255, 70);
    const uint16_t amber = rgb565(255, 190, 45);
    const uint16_t red = rgb565(255, 75, 65);
    overlayRect(b, 0, 0, 320, 39, panel);

    char text[64];
    const float kmh = fabsf(carSpeed) * 3.6f;
    snprintf(text, sizeof(text), "%03d KM/H", (int)clampf(kmh, 0.f, 999.f));
    overlayText(b, 7, 5, text, lime, 2);
    snprintf(text, sizeof(text), "%04.1f KM", distanceTravelled * 0.001f);
    overlayText(b, 220, 7, text, white, 1);
    const BiomeInfo& biome = journeyBiome(journeyCurrent());
    // Grip is written to the shared scene after the physics step.
    snprintf(text, sizeof(text), "%s  GRIP %02d%%", biome.shortName, (int)(clampf(g_scene.grip, 0.f, 1.f) * 100.f));
    overlayText(b, 8, 26, text, mint, 1);
    snprintf(text, sizeof(text), "SCORE %lu", (unsigned long)(totalScore + (uint32_t)driftPoints));
    overlayText(b, 220, 26, text, amber, 1);

    const Junction* j = terrainJunction();
    if (j && j->active) {
        float jdx = j->x - carX, jdz = j->z - carZ;
        float jd = sqrtf(jdx*jdx + jdz*jdz);
        if (jd < 85.f) {
            overlayRect(b, 12, 43, 296, 17, panel);
            snprintf(text, sizeof(text), "FORK %02dM  L:%s  R:%s", (int)jd,
                     journeyBiome(j->destL).shortName, journeyBiome(j->destR).shortName);
            overlayText(b, 18, 48, text, amber, 1);
        }
    }

    if (offRoadActive) {
        const int remainTenths = (int)ceilf(fmaxf(0.f, 3.f - offRoadTimer) * 10.f);
        overlayRect(b, 36, 91, 248, 47, panel);
        snprintf(text, sizeof(text), "OFF ROAD  %d.%d", remainTenths / 10, remainTenths % 10);
        int tw = uiTextWidth(text, 2);
        overlayText(b, (320 - tw) / 2, 99, text, red, 2);
        overlayRect(b, 48, 124, 224, 5, rgb565(55, 30, 35));
        overlayRect(b, 48, 124, (int)(224.f * clampf(offRoadTimer / 3.f, 0.f, 1.f)), 5, red);
    } else if (g_hud.banner && g_hud.bannerA > 0.1f) {
        overlayRect(b, 48, 95, 224, 23, panel);
        int tw = uiTextWidth(g_hud.banner, 2);
        overlayText(b, (320 - tw) / 2, 103, g_hud.banner, white, 2);
    }

    if (driftWasScoring && driftTime > 0.25f) {
        snprintf(text, sizeof(text), "DRIFT x%1.1f  +%lu", 1.f + fminf(driftTime, 5.f) * 0.25f,
                 (unsigned long)driftPoints);
        overlayRect(b, 8, 65, 152, 15, panel);
        overlayText(b, 13, 69, text, amber, 1);
    }
    if (scanKind) {
        overlayRect(b, 78, 178, 164, 18, panel);
        overlayText(b, 88, 184, "SCANNING OBJECT", mint, 1);
    } else if (scanDoneT > 0.f) {
        overlayRect(b, 78, 178, 164, 18, panel);
        overlayText(b, 88, 184, "SCAN COMPLETE", lime, 1);
    }
    if (audioMuted()) {
        overlayRect(b, 258, 65, 54, 16, panel);
        overlayText(b, 265, 70, "MUTED", red, 1);
    }
    if (hintTimer > 0.f) {
        overlayRect(b, 0, 220, 320, 20, panel);
        overlayText(b, 7, 226, "TILT DRIVE  TOUCH STEER/SCAN  A HORN  B LEVEL  C DRIFT", white, 1);
    }
    overlayEnable(true);
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
    mantis::journeyInit(seed);
    mantis::terrainInit(seed);
    mantis::fieldInit();
    mantis::sceneInit();
    mantis::renderInit();
    mantis::displayInit();
    mantis::databankInit();
    mantis::databankLoad();
    mantis::helmInit(0.f);
    mantis::audioBegin();
    hx::begin();

    mantis::TrackSample ts = mantis::terrainAt(0.f, 0.f);
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
    const float offRoadDistance = fabsf(ts.across) - ts.roadHalf;
    const bool offRoadNow = offRoadDistance > 0.35f;
    // Keep full 3-D movement available, but loose ground costs grip and speed.
    if (offRoadNow) {
        const float severity = clampf(offRoadDistance / 9.f, 0.f, 1.f);
        // A soft transition: just leaving the asphalt costs some grip, deep
        // shortcuts cost more, but never make the car uncontrollable.
        grip *= 0.76f - 0.24f * severity;
    }

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
    // Brake opposes motion. At a standstill it holds the car instead of
    // accidentally applying reverse thrust.
    float brk = fabsf(vLong) > 0.35f
        ? -brake * BRAKE_FORCE * (vLong > 0.f ? 1.f : -1.f) : 0.f;
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
    float steerYaw = steer * STEER_GAIN * clampf(fabsf(vLong) / 10.f, 0.f, 1.f) * fmaxf(gripAvg, 0.35f);
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
    if (frac < 0.9f) {
        velX *= 0.5f + 0.5f * frac;
        velZ *= 0.5f + 0.5f * frac;
    }

    // Off-road recovery is deliberately forgiving: the car can cut corners and
    // drive freely in 3-D, but rough ground scrubs speed and starts a 3-second clock.
    TrackSample afterMove = terrainAt(carX, carZ);
    const float afterOffRoadDistance = fabsf(afterMove.across) - afterMove.roadHalf;
    offRoadActive = afterOffRoadDistance > 0.35f;
    if (offRoadActive) {
        const float groundSeverity = clampf(afterOffRoadDistance / 9.f, 0.f, 1.f);
        grip = afterMove.grip * (0.76f - 0.24f * groundSeverity);
    } else {
        grip = afterMove.grip;
    }
    if (offRoadActive) {
        offRoadTimer += dt;
        const float severity = clampf(afterOffRoadDistance / 9.f, 0.f, 1.f);
        const float scrub = 0.06f + 0.08f * severity;
        const float damp = fmaxf(0.f, 1.f - scrub * dt);
        velX *= damp;
        velZ *= damp;
        // Off-road rumble is mixed into the normal haptic level below.
        (void)severity;
        if (offRoadTimer >= 3.0f) {
            terrainRecoverRoad(carX, carZ);
            carY = terrainHeight(carX, carZ) + 0.35f;
            // A recovery is a reset, not a crash: preserve some momentum but make
            // the return to the road noticeable and controllable.
            velX *= 0.62f;
            velZ *= 0.62f;
            offRoadTimer = 0.f;
            offRoadActive = false;
            wasBumped = false;
            hx::bump(0.35f);
            sfxBump(0.28f);
            snprintf(bannerBuf, sizeof(bannerBuf), "BACK ON ROAD");
            bannerText = bannerBuf;
            bannerT = 1.4f;
        }
    } else {
        offRoadTimer = 0.f;
        offRoadActive = false;
    }

    slipAmt = clampf(fabsf(slipAngle) * 1.4f + (hb ? 0.35f : 0.f), 0.f, 1.f);

    // Bank points for controlled, sustained slides. A short wiggle earns little;
    // holding a fast drift builds a multiplier, and ending it banks the combo.
    bool scoringDrift = fabsf(carSpeed) > 7.f && fabsf(slipAngle) > 0.16f;
    if (scoringDrift) {
        driftTime += dt;
        float multiplier = 1.f + fminf(driftTime, 5.f) * 0.25f;
        float scoredSlip = clampf(fabsf(slipAngle) * 1.6f, 0.f, 1.f);
        driftPoints += fabsf(carSpeed) * scoredSlip * (hb ? 1.35f : 1.f) * 8.f * multiplier * dt;
        driftWasScoring = true;
    } else if (driftWasScoring) {
        if (driftTime >= 0.45f && driftPoints >= 10.f) {
            uint32_t earned = (uint32_t)driftPoints;
            totalScore += earned;
            snprintf(bannerBuf, sizeof(bannerBuf), "DRIFT +%lu", (unsigned long)earned);
            bannerText = bannerBuf; bannerT = 1.5f;
            hx::kick(clampf(driftPoints / 500.f, 0.2f, 1.f));
            sfxUi(UI_SCAN_OK);
        }
        driftTime = driftPoints = 0.f;
        driftWasScoring = false;
    }

    if (bump > 0.12f && !wasBumped) {
        sfxBump(bump);
        hx::bump(bump);
        velX *= (1.f - bump * 0.4f);
        velZ *= (1.f - bump * 0.4f);
        wasBumped = true;
    } else if (bump < 0.05f) wasBumped = false;

    hx::engine(throttle, fabsf(carSpeed));
    hx::rumble(fmaxf(ts.roughness * 0.45f + slipAmt * 0.85f + (hb ? 0.25f : 0.f), offRoadActive ? 0.42f + 0.30f * clampf(afterOffRoadDistance / 9.f, 0.f, 1.f) : 0.f));
    hx::update(dt);

    distanceTravelled += sqrtf(dx * dx + dz * dz);
    stageTime += dt;

    // Road graph
    terrainUpdateRoad(carX, carZ, carYaw, fmaxf(0.f, carSpeed), dt);
    bool tookLeft = false;
    if (terrainBranchCommitted(tookLeft)) {
        uint8_t dest = journeyNext();
        snprintf(bannerBuf, sizeof(bannerBuf), "ROUTE: %s", journeyBiome(dest).name);
        bannerText = bannerBuf;
        bannerT = 2.5f;
        sfxUi(UI_SCAN_OK);
        hx::pop(0.3f, 0.04f);
    }

    journeyUpdate(carX, carZ, fmaxf(0.f, carSpeed), dt);
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

    hintTimer = fmaxf(0.f, hintTimer - dt);
    g_hud.throttle = throttle;
    g_hud.hintA = clampf(hintTimer / 2.f, 0.f, 1.f);
    g_hud.scanT = scanT;
    g_hud.banner = bannerT > 0.f ? bannerText : nullptr;
    g_hud.bannerA = bannerT > 0.f ? fminf(1.f, bannerT) : 0.f;
    if (offRoadActive) {
        const int remainTenths = (int)ceilf(fmaxf(0.f, 3.f - offRoadTimer) * 10.f);
        snprintf(bannerBuf, sizeof(bannerBuf), "OFF ROAD - %d.%d", remainTenths / 10, remainTenths % 10);
        g_hud.banner = bannerBuf;
        g_hud.bannerA = 0.95f;
    }
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
    drawGameOverlay();
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
