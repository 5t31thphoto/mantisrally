// Mantis Rally: a nature and weather simulator you explore by rally car (and, later, by Jeep, Stingray and bike).
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <M5Unified.h>
#include "helm.h"
#include "universe.h"
#include "journey.h"
#include "terrain.h"
#include "vehicle.h"
#include "field.h"
#include "scene.h"
#include "render.h"
#include "display.h"
#include "haptics.h"
#include "audio.h"
#include "progress.h"
#include "store.h"
#include "storage.h"
#include "photo.h"
#include "mem.h"

using namespace mantis;

// ======================= GAME TUNING =======================
static constexpr float OFFROAD_CLOCK = 5.f;      // seconds past a vehicle's off-road band before the reset
static constexpr float STALL_CLOCK = 1.6f;       // seconds in water too deep before the reset
static constexpr float STORE_STOP_SPEED = 1.2f;  // m/s: pull in and stop at the dealership
// ===========================================================

static VehicleState car;
static uint8_t veh = V_STI;
static float t = 0.f, distance = 0.f, offT = 0.f, stallT = 0.f, fpsAvg = 30.f;
static char msgBuf[48], subBuf[48];
static float msgT = 0.f;
static float paceA = 0.f; static PaceNote pace;
static float storeStopT = 0.f, storeHintT = 0.f, storeReqT = 0.f;
static bool storeArmed = true;
static char juncL[24], juncR[24];
static bool offRoadMsgShown = false;

static void message(const char* m, const char* sub = nullptr, float secs = 3.f) {
    snprintf(msgBuf, sizeof msgBuf, "%s", m);
    if (sub) snprintf(subBuf, sizeof subBuf, "%s", sub); else subBuf[0] = 0;
    msgT = secs;
}

static float s_onRoadT = -100.f; static int s_roadDir = 0;   // when you were last driving on the road, and which way
static void placeOnRoad(bool keepPos) {
    float x = car.x, z = car.z, yaw = car.yaw;
    if (!keepPos) { x = 0.f; z = 0.f; }
    int dir = (keepPos && t - s_onRoadT < 30.f) ? s_roadDir : 0;    // lost control on your way somewhere: back on course
    roadRecover(x, z, yaw, 1.5f, dir);
    vehicleReset(car, veh, x, z, yaw);
}

static void switchVehicle(uint8_t v) {
    float x = car.x, z = car.z, yaw = car.yaw;
    veh = v;
    vehicleReset(car, veh, x, z, yaw);
    renderSetVehicle(veh);
    g_car.engine = vehicleSpec(veh).engine;
}

void setup() {
    // Startup order is MantisPlanet / Seadiver's: internal RAM goes first come, first served.
    auto cfg = M5.config();
    cfg.internal_imu = true;
    cfg.internal_spk = true;
    cfg.internal_mic = false;                    // the Core2 shares one I2S port: the speaker keeps it
    M5.begin(cfg);
    audioSpeakerBegin();                         // the speaker's DMA buffers before anything else allocates
    audioBegin();                                // ...then the sound task's stack and buffers
    displayInit();                               // ...then the LCD strips (DMA-capable internal RAM, or it falls back to slow PSRAM)
    M5.Display.setRotation(1);
    M5.Display.setBrightness(110);
    if (M5.Imu.isEnabled()) {
        M5.Imu.loadOffsetFromNVS();
        for (int i = 0; i < 15; i++) { M5.Imu.update(); delay(4); }
    }
    uint32_t seed = (uint32_t)(millis() ^ 0xC0FFEEu);
#ifndef HOST
    seed ^= (uint32_t)esp_random();
#endif
    storageInit();                               // the SD card (shares the LCD's bus: after the display)
    progressInit();
    g_prog.view = VIEW_COCKPIT;                  // you always start in the driver's seat
    universeInit(seed);
    journeyInit(seed);
    journeySetVisited(g_prog.visited);
    terrainInit(seed);                           // the near cache: the hottest memory in the game
    sceneInit(seed);
    fieldInit(seed);
    renderInit();
    helmInit();
    hx::begin();
    {   // did the hot buffers get fast internal RAM? (shown under the FPS readout)
        static char note[40];
        bool lcd = displayStreaming(), grid = memIsFast(terrainNear().c), depth = renderDepthFast(), snd = audioRunning();
        if (lcd && grid && depth && snd) snprintf(note, sizeof note, "MEM OK");
        else snprintf(note, sizeof note, "MEM SLOW:%s%s%s%s", lcd ? "" : " LCD", grid ? "" : " GRID", depth ? "" : " DEPTH", snd ? "" : " SOUND");
        g_memNote = note;
    }
    veh = g_prog.current;
    placeOnRoad(false);
    renderSetVehicle(veh);
    g_car.engine = vehicleSpec(veh).engine;
    message(biome(journeyCurrent()).name, "TIP FORWARD TO DRIVE", 4.f);
}

void loop() {
    static uint32_t lastMs = 0;
    uint32_t now = millis();
    float dt = lastMs ? (now - lastMs) * 0.001f : 1.f / 30.f;
    lastMs = now;
    if (dt > 0.05f) dt = 0.05f;
    t += dt;
    fpsAvg += (1.f / fmaxf(dt, 0.001f) - fpsAvg) * 0.05f;

    HelmEvents ev;
    helmUpdate(dt, ev);
    if (ev.mute) { audioSetMuted(!audioMuted()); hx::confirmMute(audioMuted()); }

    // ---------------- the dealership ----------------
    if (storeActive()) {
        bool changed = false;
        if (!storeUpdate(ev.tap, ev.tapX, ev.tapY, changed)) { message("DRIVE SAFE", nullptr, 1.5f); progressSave(); }
        if (changed) { switchVehicle(g_prog.current); sfxUi(UI_SOLD); }
        displayPresent();
        hx::update(dt);
        return;
    }
    // ---------------- photo mode ----------------
    if (photoActive()) {
        if (ev.view) { photoExit(); sfxUi(UI_TICK); ev.view = false; g_prog.view = VIEW_COCKPIT; }   // B: back in the car, back to driving
        else {
            if (ev.cPress) { photoNextAngle(); sfxUi(UI_TICK); }    // C: the next angle
            photoUpdate(dt, helmSteer(), helmTip());
            terrainUpdate(car.x, car.z, car.yaw, 0.f, dt);
            sceneUpdate(t, dt, car.x, car.z, car.yaw, 0.f, distance);
            fieldUpdate(car, veh, dt);
            renderFrame();
            if (ev.horn) sfxImpact(IMP_SHUTTER, 1.f);                // A: the shutter
            photoCompose(ev.horn);
            displayPresent();
            hx::update(dt);
            return;
        }
    }
    if (ev.horn && car.speed < 0.6f && !storeActive()) {            // stopped: A opens the camera
        photoEnter(car, veh); sfxUi(UI_OK); ev.horn = false;
        g_car.throttle = 0.f;
        return;
    }
    if (ev.doubleTap) {                                             // stuck: a double tap puts you back on the road
        placeOnRoad(true); offT = 0.f; stallT = 0.f;
        message("BACK ON THE ROAD", nullptr, 1.5f); sfxUi(UI_WARN);
    }
    if (ev.view) { g_prog.view = (uint8_t)((g_prog.view + 1) % VIEW_N); sfxUi(UI_TICK); }
    if (ev.recenter) { sfxUi(UI_OK); hx::pop(0.3f, 0.04f); }
    if (ev.horn) { sfxHorn(); fieldHorn(car.x, car.z); hx::horn(); }

    const VehicleSpec& V = vehicleSpec(veh);
    // ---------------- drive ----------------
    VehicleInput in;
    in.steer = helmSteer(); in.throttle = helmThrottle(); in.brake = helmBrake(); in.handbrake = helmHandbrake();
    in.airPitch = -helmTip();
    // launch control (the STI's trick): brake + throttle at a standstill holds it on the boil
    if (veh == V_STI && car.speed < 0.5f && in.brake > 0.2f && helmThrottle() > 0.f) in.throttle = 0.f;
    float px = car.x, pz = car.z;
    vehicleStep(car, veh, in, dt);
    FieldHit hit = fieldCollide(car, veh);
    if (hit.kind == HIT_DEER) {
        sfxImpact(IMP_DEER, 1.f); hx::bump(0.6f);
        if (g_prog.view == VIEW_COCKPIT) renderDeerSplat();
    } else if (hit.strength > 2.f) {
        sfxImpact(hit.kind == HIT_TREE ? IMP_TREE : IMP_ROCK, fminf(3.f, hit.strength / 14.f));   // past 1: a collision
        sfxCrash(hit.strength, hit.kind == HIT_TREE);
        hx::bump(fminf(1.f, hit.strength / 12.f));
        if (hit.strength > 6.f) renderSplat(0.6f);
    }
    float moved = sqrtf((car.x - px) * (car.x - px) + (car.z - pz) * (car.z - pz));
    if (moved > 20.f) moved = 0.f;
    distance += moved;
    progressDrive(moved);
    // events from the chassis
    if (car.evLand > 6.f) {                                   // came down hard and fast: the crunch
        sfxImpact(IMP_CRUNCH, fminf(2.5f, (car.evLand - 4.f) / 8.f)); sfxLand(1.f); hx::bump(1.f);
    } else if (car.evLand > 1.2f) { sfxLand(fminf(1.f, car.evLand / 8.f)); hx::bump(fminf(1.f, car.evLand / 7.f)); }
    if (car.evBottom > 1.f) { sfxBump(fminf(1.f, car.evBottom / 5.f)); hx::bump(fminf(1.f, car.evBottom / 6.f)); }
    else if (car.evBump > 0.35f) { sfxBump(car.evBump); }
    if (car.evShift != 0.f) { sfxShift(car.evShift > 0 ? 1 : -1); hx::kick(0.35f); }
    if (car.evSplash > 0.f) { sfxSplash(car.evSplash); if (g_prog.view == VIEW_COCKPIT) renderWater(0.8f); }
    if (veh == V_VETTE && car.spin > 0.6f && car.surf != SURF_TARMAC && fabsf(car.r) > 1.2f && ((int)(t * 8.f) & 1)) renderSplat(0.3f + car.spin * 0.5f);   // donuts fling mud
    car.evLand = car.evBottom = car.evBump = car.evShift = car.evSplash = 0.f;

    // ---------------- the road, off-road, water ----------------
    float band = g_prog.offroadOn ? 999.f : V.offroadBand;
    float off = car.mat == MAT_LOT ? 0.f : fabsf(car.across) - car.half;    // the dealership's lot counts as road
    if (fabsf(car.across) - car.half < 1.f && car.speed > 4.f) { s_onRoadT = t; s_roadDir = roadTravelDir(); }   // on the road and going: remember the way
    {   const Store* sn = roadStore();                       // ...and so does pulling in to it
        if (sn->active && (sn->x - car.x) * (sn->x - car.x) + (sn->z - car.z) * (sn->z - car.z) < 40.f * 40.f) off = 0.f; }
    bool onRoad = off < 30.f;
    if (off > band) {
        if (!offRoadMsgShown && veh == V_JEEP) { message("TOO FAR FROM THE ROAD", nullptr, 1.5f); offRoadMsgShown = true; }
        offT += dt;
        if (offT > OFFROAD_CLOCK) { placeOnRoad(true); car.u *= 0.f; offT = 0.f; sfxUi(UI_WARN); message("BACK ON THE ROAD", nullptr, 1.5f); }
    } else { offT = 0.f; offRoadMsgShown = false; }
    if (car.water > V.fordDepth) {
        stallT += dt;
        if (stallT > STALL_CLOCK) { placeOnRoad(true); stallT = 0.f; message("TOO DEEP", nullptr, 2.5f); }
    } else stallT = 0.f;

    // ---------------- the journey ----------------
    terrainUpdate(car.x, car.z, car.yaw, car.u, dt);
    journeyUpdate(moved, car.yaw, onRoad);
    journeySetCar(car.x, car.z, car.yaw, moved);
    if (journeyBiomeEvent()) {
        g_prog.visited |= (uint16_t)(1u << journeyCurrent());
        if (journeyCurrent() == journeyDestination() || journeyArrivedPending()) {   // where you chose: the arrival
            message(biome(journeyCurrent()).name, nullptr, 3.5f);
            sfxUi(UI_ARRIVE);
        } else {                                               // on the way: where you are, and where you're headed
            snprintf(subBuf, sizeof subBuf, "%s %.0f MI", biome(journeyDestination()).sign, journeyMilesToDest() + 0.4f);
            message(biome(journeyCurrent()).name, subBuf, 3.f);
            sfxUi(UI_TICK);
        }
        if (progressAllBiomes() && !g_prog.offroadUnlocked) {
            g_prog.offroadUnlocked = true; g_prog.offroadOn = true;
            message("EVERY BIOME SEEN", "OFF-ROAD MODE FOR EVERY VEHICLE", 5.f);
        }
        progressSave();
    }
    if (journeyArrivedEvent()) { snprintf(subBuf, sizeof subBuf, "NEXT: %s", biome(journeyDestination()).sign); message("YOU MADE IT", subBuf, 4.f); }
    bool tookArm = false;
    if (roadJunctionEvent(tookArm)) {
        char s[40]; snprintf(s, sizeof s, "ROUTE: %s %.0f MI", biome(journeyDestination()).sign, journeyMilesToDest());
        message(s, nullptr, 2.5f); sfxUi(UI_FORK);
    }
    // the fork ahead: name both ways
    float juncA = 0.f;
    const Junction* j = roadJunction();
    if (j->active && !j->decided) {
        float dx = j->x - car.x, dz = j->z - car.z;
        float ahead = dx * cosf(car.yaw) + dz * sinf(car.yaw);
        if (ahead > 0.f && ahead < 380.f) {
            juncA = 1.f;
            bool armLeft = roadJunctionArmSide() < 0;                 // the sides as you see them at the split
            uint8_t dl = armLeft ? j->destArm : j->destMain, dr = armLeft ? j->destMain : j->destArm;
            float ml = armLeft ? j->milesArm : j->milesMain, mr = armLeft ? j->milesMain : j->milesArm;
            snprintf(juncL, sizeof juncL, "<%s %.0f", biome(dl).sign, ml);
            snprintf(juncR, sizeof juncR, "%s %.0f>", biome(dr).sign, mr);
        }
    }
    // the dealership: ask for one when you can afford the next car; pull in and stop to shop
    storeReqT += dt;
    int nxt = progressNextLocked();
    if (storeReqT > 20.f) { storeReqT = 0.f; if (nxt >= 0 && progressCanBuy(nxt)) roadRequestStore(); }
    float storeA = 0.f;
    const Store* st = roadStore();
    if (st->active) {
        float dx = st->x - car.x, dz = st->z - car.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < 400.f && d > 40.f && dx * cosf(car.yaw) + dz * sinf(car.yaw) > 0.f) storeA = 1.f;
        if (d > 45.f) storeArmed = true;                    // re-arms once you've driven away from it
        if (storeArmed && d < 26.f && car.speed < STORE_STOP_SPEED) { storeStopT += dt; if (storeStopT > 0.6f || ev.tap) { storeOpen(); storeArmed = false; storeStopT = 0.f; sfxUi(UI_OK); car.u = car.v = 0.f; } }
        else storeStopT = 0.f;
    }
    (void)storeHintT;
    // pace notes: the STI's co-driver
    if (veh == V_STI) {
        PaceNote p = roadPaceNote(car.x, car.z, car.yaw, car.speed);
        if (p.valid && car.speed > 8.f) { pace = p; paceA = 1.f; } else paceA = fmaxf(0.f, paceA - dt * 1.5f);
    } else paceA = 0.f;

    // ---------------- world ----------------
    sceneUpdate(t, dt, car.x, car.z, car.yaw, car.speed, distance);
    fieldUpdate(car, veh, dt);
    if (g_scene.thunderIn > 0.f && g_scene.boltAge < dt * 1.5f) sfxThunder(g_scene.thunderIn, g_scene.thunderPow);
    {   static float critT = 0.f; critT += dt;
        const Critter* c = fieldCritters();
        for (int k = 0; k < CRIT_N; k++) if (c[k].on && c[k].fear > 0.95f && critT > 4.f) { sfxCritter(CRIT_DEER, hypotf(c[k].x - car.x, c[k].z - car.z)); critT = 0.f; break; } }

    // ---------------- sound and feel ----------------
    g_car.engine = V.engine; g_car.rpm = car.rpm; g_car.rpm01 = car.rpm / V.redline; g_car.throttle = car.reverse ? in.brake : in.throttle;
    g_car.boost = car.boost; g_car.spin = car.spin; g_car.slide = fmaxf(car.slipR, car.slipF) * 0.6f + (in.handbrake ? 0.4f : 0.f) * (car.speed > 3.f ? 1.f : 0.f);
    g_car.speed = car.speed; g_car.water = car.water; g_car.surf = car.surf; g_car.air = car.air; g_car.limiter = car.limiterT > 0.f;
    g_car.reverse = car.reverse; g_car.cockpit = g_prog.view == VIEW_COCKPIT;
    hx::engine(in.throttle, car.rpm / V.redline * 10.f);
    float rough = car.surf == SURF_TARMAC ? 0.05f : (car.surf == SURF_GRAVEL ? 0.3f : 0.4f);
    hx::rumble(car.air ? 0.f : fminf(1.f, rough * fminf(1.f, car.speed / 20.f) + car.spin * 0.4f + (car.slipR > 1.f ? 0.3f : 0.f)));
    hx::update(dt);

    // ---------------- picture ----------------
    ViewInfo& v = g_view;
    Ground g; terrainGround(car.x, car.z, g);
    v.view = g_prog.view; v.vehicle = veh;
    v.x = car.x; v.y = car.y; v.z = car.z; v.yaw = car.yaw;
    v.groundY = car.y - V.rideHeight;
    v.pitch = car.pitch; v.roll = car.roll; v.lean = car.lean;
    v.steer = car.steer / (V.lockDeg * 0.0174533f);
    v.speedMph = car.speed * 2.23694f; v.rpm01 = car.rpm / V.redline;
    v.brake = in.brake > 0.1f || in.handbrake; v.reverse = car.reverse;
    v.bounce = (car.comp - 0.5f) * -0.12f;
    v.offroadLeft = offT > 0.f ? OFFROAD_CLOCK - offT : -1.f;
    v.msg = msgT > 0.f ? msgBuf : nullptr; v.sub = subBuf[0] ? subBuf : nullptr; v.msgA = msgT > 1.f ? 1.f : msgT;
    v.paceDir = pace.dir; v.paceSev = pace.sev; v.paceA = paceA; v.paceCrest = pace.crest;
    v.juncL = juncL; v.juncR = juncR; v.juncA = juncA; v.storeA = storeA;
    v.showFps = false; v.fps = (int)fpsAvg;
    if (msgT > 0.f) msgT -= dt;
    (void)g;
    renderFrame();
    { bool deerWipe; if (renderWipeEvent(deerWipe)) sfxImpact(deerWipe ? IMP_WIPE_DEER : IMP_WIPE, 1.f); }
    displayPresent();
}

#ifdef HOST
int main() { setup(); for (;;) loop(); }
#endif
