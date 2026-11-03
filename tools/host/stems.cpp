// Audio stems for critique: each sound on its own, rendered by the game's synthesizers through the device's
// output stage (reverb, echo, the 165 Hz low cut, limiter). 24 kHz mono WAV.   stems -> stems/*.wav + stems/INDEX.txt
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include "audio.h"
#include "audio_int.h"
#include "scene.h"
#include "universe.h"
#include "journey.h"
#include "terrain.h"
#include "vehicle.h"
using namespace mantis;
namespace mantis { namespace snd { extern uint32_t g_stemMute; extern uint32_t g_sndVariant; } }
using snd::g_stemMute;

static dsp::Reverb s_rev; static dsp::DubDelay s_dly; static dsp::SVF s_hp;
static float s_env = 0.f, s_gain = 1.f;
static std::vector<int16_t> s_out;
static FILE* s_index;
static void master(snd::Bus& b, int n) {
    s_hp.set(165.f, 0.72f);
    for (int i = 0; i < n; i++) {
        float d = s_dly.tick(b.dly[i]); float r = s_rev.tick(b.rev[i] + d * 0.3f);
        float x = s_hp.hp(b.dry[i] + d * 0.6f + r * 0.85f) * 1.25f;
        float a = fabsf(x); s_env = a > s_env ? s_env + (a - s_env) * 0.25f : s_env * 0.99985f;
        float g = s_env > 0.62f ? 0.62f / s_env : 1.f; s_gain += (g - s_gain) * (g < s_gain ? 0.2f : 0.0004f);
        s_out.push_back((int16_t)(dsp::softclip(x * s_gain * 1.05f) * 29000.f));
    }
}
static snd::NatScene s_nat;
static bool s_natOn = false;
// render 1/30 s: the car layers and (optionally) the nature bed
static void block(int n) {                          // exactly as audioRender mixes it
    snd::Bus b; memset(&b, 0, sizeof b);
    static snd::Bus nb; static dsp::SVF natLP; memset(&nb, 0, sizeof nb);
    float* nearB = snd::natureNearBuffer(); memset(nearB, 0, sizeof(float) * snd::BLOCK);
    if (s_natOn) snd::natureBlock(nb, n, s_nat);
    natLP.set(g_car.cockpit ? 2400.f : 4200.f, 0.6f);
    float inCab = g_car.cockpit ? 0.8f : 1.f;
    for (int i = 0; i < n; i++) { float d = natLP.lp(nb.dry[i]) * inCab; b.dry[i] += d * 0.8f + nearB[i]; b.rev[i] += nb.rev[i] + d * 0.35f; b.dly[i] += nb.dly[i]; }
    snd::engineBlock(b, n);
    master(b, n);
}
static void frame() { block(800 > snd::BLOCK ? snd::BLOCK : 800); if (800 > snd::BLOCK) block(800 - snd::BLOCK); }
static void begin() { s_out.clear(); s_env = 0.f; s_gain = 1.f; s_rev.begin(); s_dly.begin(AUDIO_SR / 2); s_dly.target = AUDIO_SR * 0.21f; s_rev.fb = 0.6f; s_rev.damp = 0.42f; }
static void save(const char* name, const char* what) {
    std::string p = std::string("stems/") + name + ".wav";
    FILE* f = fopen(p.c_str(), "wb");
    uint32_t n = (uint32_t)s_out.size() * 2, sr = AUDIO_SR, br = sr * 2;
    fwrite("RIFF", 1, 4, f); uint32_t c = 36 + n; fwrite(&c, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fl = 16; uint16_t pcm = 1, ch = 1, ba = 2, bits = 16; fwrite(&fl, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&n, 4, 1, f);
    fwrite(s_out.data(), 2, s_out.size(), f); fclose(f);
    fprintf(s_index, "%-34s %s\n", name, what); printf("%s\n", name);
}
static void carIdle(uint8_t eng) { memset(&g_car, 0, sizeof g_car); g_car.engine = eng; g_car.rpm = 900.f; g_car.cockpit = true; g_car.surf = SURF_TARMAC; }

static const char* ENG_NAME[4] = {"sti_flat4_turbo", "jeep_inline6", "vette_v8", "bike_2stroke"};

int main(int argc, char** argv) {
    snd::g_sndVariant = argc > 1 ? (uint32_t)atoi(argv[1]) : 0u;
    (void)!system("mkdir -p stems && rm -f stems/*");
    s_index = fopen("stems/INDEX.txt", "w");
    fprintf(s_index, "Mantis Rally audio stems (sound pass 1). 24 kHz mono, through the device's output stage.\n\n");
    dsp::init(); snd::rng.s = 0x2545F491u; snd::natureInit(); snd::engineInit();
    memset(&g_scene, 0, sizeof g_scene); g_scene.tod = 0.45f;

    // ---- A. engines: free revs in place (idle, sweep to the limiter, lift off, back to idle) ----
    int k = 0;
    for (int v = 0; v < 4; v++) {
        const VehicleSpec& V = vehicleSpec((uint8_t)v);
        begin(); carIdle(V.engine); g_stemMute = 2 | 4 | 16;
        float boost = 0.f;
        for (int f = 0; f < 30 * 9; f++) {
            float t = f / 30.f, rpm, thr;
            if (t < 1.f) { rpm = V.idleRpm; thr = 0.f; }
            else if (t < 5.f) { thr = 1.f; rpm = V.idleRpm + (V.redline * 1.02f - V.idleRpm) * ((t - 1.f) / 4.f); }
            else if (t < 5.6f) { thr = 1.f; rpm = V.redline * 1.03f; }
            else if (t < 8.f) { thr = 0.f; rpm = V.redline - (V.redline - V.idleRpm) * fminf(1.f, (t - 5.6f) / 2.f); }
            else { thr = 0.f; rpm = V.idleRpm; }
            boost += ((V.turbo > 0.f ? thr * fminf(1.f, fmaxf(0.f, (rpm - 2300.f) / 1700.f)) : 0.f) - boost) * 0.08f;
            g_car.rpm = rpm; g_car.rpm01 = rpm / V.redline; g_car.throttle = thr; g_car.boost = boost; g_car.limiter = rpm > V.redline;
            frame();
        }
        char nm[64], what[160]; snprintf(nm, sizeof nm, "A%d_%s_free_rev", ++k, ENG_NAME[v]);
        snprintf(what, sizeof what, "%s: idle 1 s, full throttle to the limiter, lift off (overrun), back to idle", V.name);
        save(nm, what);
    }
    // ---- B. engines under load: the real physics, full throttle through the gears on tarmac, then lift ----
    for (int v = 0; v < 4; v++) {
        const VehicleSpec& V = vehicleSpec((uint8_t)v);
        universeInit(9); journeyInit(9); journeyForce(B_LAKES, B_LAKES, 0); terrainInit(9);
        RoadHit h; roadNearest(0, 0, h); VehicleState s; vehicleReset(s, (uint8_t)v, 0, 0, h.yaw);
        for (int pass = 0; pass < 2; pass++) {
            vehicleReset(s, (uint8_t)v, 0, 0, h.yaw);
            begin(); carIdle(V.engine); g_stemMute = pass == 0 ? (2 | 4 | 16) : 16;
            for (int f = 0; f < 30 * 20; f++) {
                RoadHit r; float st = 0;
                if (roadNearest(s.x, s.z, r, 400)) { int n = roadCount(); int kk = r.idx + (int)((8 + s.speed * 0.7f) / 3.f); kk = kk > n - 1 ? n - 1 : kk;
                    const RoadNode& q = roadNode(kk); float e = atan2f(q.z - s.z, q.x - s.x) - s.yaw; while (e > 3.14159f) e -= 6.28318f; while (e < -3.14159f) e += 6.28318f; st = e * 2.2f; }
                float thr = f < 30 * 16 ? 1.f : 0.f;
                VehicleInput in{fmaxf(-1, fminf(1, st)), thr, f >= 30 * 18 ? 0.6f : 0.f, false, 0};
                vehicleStep(s, (uint8_t)v, in, 1 / 30.f); terrainUpdate(s.x, s.z, s.yaw, s.u, 1 / 30.f);
                if (s.evShift != 0.f) snd::engineEvent(snd::EE_SHIFT, s.evShift);
                s.evShift = 0.f; s.evLand = s.evBump = s.evBottom = s.evSplash = 0.f;
                g_car.rpm = s.rpm; g_car.rpm01 = s.rpm / V.redline; g_car.throttle = thr; g_car.boost = s.boost; g_car.spin = s.spin;
                g_car.speed = s.speed; g_car.surf = s.surf; g_car.limiter = s.limiterT > 0.f; g_car.slide = fmaxf(s.slipR, s.slipF) * 0.6f;
                frame();
            }
            char nm[64], what[200];
            snprintf(nm, sizeof nm, "B%d_%s_through_gears%s", v + 1, ENG_NAME[v], pass == 0 ? "_engine_only" : "_in_car");
            snprintf(what, sizeof what, "%s: real physics, full throttle from a stop through the gears 16 s, lift, brake. %s",
                     V.name, pass == 0 ? "Engine only." : "With road and wind, as heard from the cockpit.");
            save(nm, what);
        }
    }
    // ---- C. tyres and surfaces (engine muted) ----
    struct TS { const char* nm; uint8_t surf; float speed, slideTo, spin; const char* what; };
    const TS TY[] = {
        {"C1_squeal_tarmac_slide", SURF_TARMAC, 20.f, 1.0f, 0.f, "Tarmac at 45 mph: slide builds over 1 s, held 3 s, released (attack, sustain, decay)"},
        {"C2_squeal_tarmac_wheelspin", SURF_TARMAC, 5.f, 0.f, 1.f, "Tarmac: wheelspin off the line for 3 s"},
        {"C3_gravel_30mph", SURF_GRAVEL, 13.f, 0.f, 0.f, "Gravel at 30 mph, rolling"},
        {"C4_gravel_60mph_sliding", SURF_GRAVEL, 27.f, 0.8f, 0.f, "Gravel at 60 mph with a slide"},
        {"C5_dirt_50mph", SURF_DIRT, 22.f, 0.3f, 0.f, "Dirt at 50 mph"},
        {"C6_mud_30mph", SURF_MUD, 13.f, 0.3f, 0.f, "Mud at 30 mph"},
        {"C7_snow_35mph", SURF_SNOW, 16.f, 0.3f, 0.f, "Snow at 35 mph"},
        {"C8_sand_40mph", SURF_SAND, 18.f, 0.3f, 0.f, "Sand at 40 mph"},
        {"C9_road_roar_tarmac_60mph", SURF_TARMAC, 27.f, 0.f, 0.f, "Tarmac rolling at 60 mph, no slide"},
    };
    for (const TS& t : TY) {
        begin(); carIdle(ENG_BOXER4T); g_stemMute = 1 | 4 | 16;
        for (int f = 0; f < 30 * 6; f++) {
            float tt = f / 30.f;
            float env = tt < 1.f ? 0.f : (tt < 2.f ? tt - 1.f : (tt < 5.f ? 1.f : fmaxf(0.f, 1.f - (tt - 5.f) * 1.5f)));
            g_car.surf = t.surf; g_car.speed = t.speed; g_car.slide = t.slideTo * env; g_car.spin = t.spin * (tt > 1.f && tt < 4.f ? 1.f : 0.f);
            frame();
        }
        save(t.nm, t.what);
    }
    // ---- D. wind ----
    const float WS[3] = {13.f, 27.f, 40.f};
    for (int c = 0; c < 2; c++) for (int w = 0; w < 3; w++) {
        begin(); carIdle(ENG_BOXER4T); g_stemMute = 1 | 2 | 16; g_car.cockpit = c == 0;
        for (int f = 0; f < 30 * 4; f++) { g_car.speed = WS[w]; frame(); }
        char nm[64], what[120]; snprintf(nm, sizeof nm, "D%d_wind_%dmph_%s", c * 3 + w + 1, (int)(WS[w] * 2.237f + 0.5f), c == 0 ? "cockpit" : "chase");
        snprintf(what, sizeof what, "Wind at %d mph, %s", (int)(WS[w] * 2.237f + 0.5f), c == 0 ? "cockpit" : "chase / eye view"); save(nm, what);
    }
    // ---- E. impacts and one-shots (each alone, with a little room after) ----
    struct OS { const char* nm; int kind; uint8_t a; float s; const char* what; };   // kind 0 nature impact, 1 engine event
    const OS SH[] = {
        {"E01_tree_light", 0, IMP_TREE, 0.3f, "Hitting a tree, glancing"}, {"E02_tree_hard", 0, IMP_TREE, 1.f, "Hitting a tree, hard"},
        {"E03_rock_light", 0, IMP_ROCK, 0.3f, "Hitting a rock, glancing"}, {"E04_rock_hard", 0, IMP_ROCK, 1.f, "Hitting a rock, hard"},
        {"E05_deer", 0, IMP_DEER, 1.f, "Hitting a deer (thud, splat, drips)"},
        {"E06_landing_crunch_medium", 0, IMP_CRUNCH, 0.5f, "Landing too fast, medium"}, {"E07_landing_crunch_hard", 0, IMP_CRUNCH, 1.f, "Landing too fast, hard"},
        {"E08_landing_normal", 1, snd::EE_LAND, 0.6f, "A normal landing after air"}, {"E09_bump", 1, snd::EE_BUMP, 0.5f, "A bump / suspension bottoming"},
        {"E10_crash_body", 1, snd::EE_CRASH, 10.f, "The body thump that goes with any crash"}, {"E11_splash", 1, snd::EE_SPLASH, 1.f, "Driving into water"},
        {"E12_shutter", 0, IMP_SHUTTER, 1.f, "Photo mode shutter"},
        {"E30_collision_tree_40mph", 0, IMP_TREE, 1.3f, "A collision: into a tree at ~40 mph"}, {"E31_collision_rock_55mph", 0, IMP_ROCK, 1.8f, "A collision: into a rock at ~55 mph"},
        {"E32_collision_tree_70mph", 0, IMP_TREE, 2.3f, "A collision: into a tree at ~70 mph"}, {"E33_landing_huge", 0, IMP_CRUNCH, 2.0f, "Landing a huge jump far too fast"}, {"E13_wiper", 0, IMP_WIPE, 1.f, "A wiper sweep"}, {"E14_wiper_after_deer", 0, IMP_WIPE_DEER, 1.f, "The slow wipe after a deer"},
    };
    for (const OS& o : SH) {
        begin(); carIdle(ENG_BOXER4T); g_stemMute = 1 | 2 | 4 | 16;
        // impacts play through the nature engine's voices: run it, with its ambience silent
        s_natOn = true; memset(&s_nat, 0, sizeof s_nat); s_nat.tod = 0.5f; s_nat.rainAgo = 999.f; s_nat.stillFor = 30.f;
        for (int f = 0; f < 30 * 3; f++) { s_nat.t = 200.f + f / 30.f; if (f == 6) { if (o.kind == 0) snd::natureImpact(o.a, o.s); else snd::engineEvent(o.a, o.s); } frame(); }
        s_natOn = false;
        save(o.nm, o.what);
    }
    for (int v = 0; v < 4; v++) {
        begin(); carIdle(vehicleSpec((uint8_t)v).engine); g_stemMute = 1 | 2 | 4 | 16;
        for (int f = 0; f < 30 * 2; f++) { if (f == 6) snd::engineEvent(snd::EE_HORN, 0); frame(); }
        char nm[64], what[100]; snprintf(nm, sizeof nm, "E%d_horn_%s", 15 + v, ENG_NAME[v]); snprintf(what, sizeof what, "Horn: %s", vehicleSpec((uint8_t)v).name); save(nm, what);
    }
    const char* UIN[7] = {"tick", "ok", "sold", "arrive_biome", "fork_decided", "warn", "mute"};
    for (int u = 0; u < 7; u++) {
        begin(); carIdle(ENG_BOXER4T); g_stemMute = 1 | 2 | 4 | 16;
        for (int f = 0; f < 30 * 2; f++) { if (f == 6) snd::engineEvent(snd::EE_UI, (float)u); frame(); }
        char nm[64], what[80]; snprintf(nm, sizeof nm, "E%d_ui_%s", 19 + u, UIN[u]); snprintf(what, sizeof what, "Interface: %s", UIN[u]); save(nm, what);
    }
    // ---- F. the world: weather and nature beds (no car) ----
    struct NS { const char* nm; float tod, rain, storm, wind, forest, lake, ocean, frog, snowy, open; float thunder; const char* what; };
    const NS NB[] = {
        {"F01_forest_morning_birds", 0.30f, 0, 0, 0.15f, 0.8f, 0, 0, 0, 0, 0.3f, 0, "Pine forest, morning: birds, wind in the trees"},
        {"F02_meadow_dusk_crickets", 0.82f, 0, 0, 0.1f, 0.2f, 0, 0, 0, 0, 1.f, 0, "Meadow at dusk: crickets"},
        {"F03_bayou_night_frogs", 0.90f, 0, 0, 0.1f, 0.4f, 0.6f, 0, 1.f, 0, 0.3f, 0, "Bayou at night: frogs"},
        {"F04_coast_surf", 0.50f, 0, 0, 0.3f, 0.2f, 0, 0.8f, 0, 0, 0.5f, 0, "Coast: surf"},
        {"F05_lake_evening", 0.74f, 0, 0, 0.1f, 0.4f, 0.8f, 0, 0, 0, 0.4f, 0, "Lake country, evening"},
        {"F06_rain_light", 0.45f, 0.3f, 0, 0.2f, 0.5f, 0, 0, 0, 0, 0.3f, 0, "Light rain"},
        {"F07_rain_heavy", 0.45f, 1.0f, 0.3f, 0.5f, 0.5f, 0, 0, 0, 0, 0.3f, 0, "Heavy rain"},
        {"F08_storm_thunder_near", 0.45f, 0.8f, 0.9f, 0.8f, 0.5f, 0, 0, 0, 0, 0.3f, 0.6f, "Storm with a close thunderclap"},
        {"F09_storm_thunder_far", 0.45f, 0.6f, 0.7f, 0.6f, 0.5f, 0, 0, 0, 0, 0.3f, 4.f, "Storm with distant rolling thunder"},
        {"F10_tundra_night_wind", 0.95f, 0, 0, 0.4f, 0.1f, 0, 0, 0, 1.f, 1.f, 0, "Tundra at night: wind on the snow"},
    };
    for (const NS& n : NB) {
        begin(); carIdle(ENG_BOXER4T); g_stemMute = 1 | 2 | 4 | 8 | 16; s_natOn = true;
        memset(&s_nat, 0, sizeof s_nat);
        s_nat.tod = n.tod; s_nat.rain = n.rain; s_nat.storm = n.storm; s_nat.gust = n.wind; s_nat.forestNear = n.forest; s_nat.lakeNear = n.lake;
        s_nat.oceanNear = n.ocean; s_nat.frogBoost = n.frog; s_nat.snowy = n.snowy; s_nat.tundra = n.snowy; s_nat.open = n.open;
        s_nat.cloud = n.rain > 0 ? 0.9f : 0.3f; s_nat.wet = n.rain > 0 ? 0.8f : 0.f; s_nat.rainAgo = n.rain > 0 ? 0.f : 999.f; s_nat.stillFor = 30.f;
        for (int f = 0; f < 30 * 10; f++) { s_nat.t = 100.f + f / 30.f; if (n.thunder > 0.f && f == 30) snd::natureThunder(n.thunder, 1.f, 1.f - n.thunder / 7.f); frame(); }
        s_natOn = false;
        save(n.nm, n.what);
    }
    // ---- G. the awe pad (sunsets, rainbows, aurora) ----
    for (int b = 0; b < 3; b++) {
        const uint8_t BB[3] = {B_LAKES, B_TUNDRA, B_CANYON};
        begin(); carIdle(ENG_BOXER4T); g_stemMute = 1 | 2 | 4 | 8; g_scene.biomeA = BB[b]; g_scene.awe = 1.f;
        for (int f = 0; f < 30 * 10; f++) frame();
        g_scene.awe = 0.f;
        char nm[64], what[100]; snprintf(nm, sizeof nm, "G%d_awe_pad_%s", b + 1, biome(BB[b]).sign); snprintf(what, sizeof what, "The soft chord that swells at spectacular moments: %s", biome(BB[b]).name); save(nm, what);
    }
    fclose(s_index);
    return 0;
}
