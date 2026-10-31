// Vehicle dynamics. Units: metres, seconds, kilograms, newtons, radians. +yaw turns right (clockwise from above).
#include "vehicle.h"
#include "terrain.h"
#include "journey.h"
#include <math.h>
#include <string.h>

namespace mantis {

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float sgn(float v) { return v < 0.f ? -1.f : 1.f; }
static constexpr float G = 9.81f;
static inline float wrapPiF(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

//                   tarmac gravel dirt  mud   snow  sand  grass rock
static const VehicleSpec VEHICLES[V_COUNT] = {
    { "MANTIS STI", "STI", "2013 HATCH. AWD. TURBO FLAT FOUR.", "PACE NOTES + LAUNCH CONTROL", false,
      1480.f, 2.62f, 0.60f, 0.50f, 1.53f, 1.05f,
      30.f, 26.f, 3.4f, 0.55f,
      ENG_BOXER4T, 410.f, 4000.f, 850.f, 6900.f, 3600.f, 0.45f,
      6, {3.64f, 2.24f, 1.52f, 1.14f, 0.97f, 0.76f}, 3.90f, 0.32f, 0.11f,
      DRIVE_AWD, 0.41f, 1.10f, 0.62f, true,
      {1.00f, 0.92f, 0.87f, 0.62f, 0.56f, 0.62f, 0.64f, 0.76f}, 1.55f, 1.28f, 0.115f, 1.0f,
      0.78f, 2.1f, 0.62f, 0.50f, 0.17f,
      3.0f, 0.35f, 1.18f, 0.0f, COCKPIT_RALLY,
      V_STI, 0.f, 0.80f, 0.85f, 0.35f },
    { "OLD JEEP 4.0", "JEEP", "1993 4.0 SIX. PART-TIME 4X4.", "LEAVE THE ROAD + LOW RANGE + WADE", false,
      1520.f, 2.37f, 0.54f, 0.78f, 1.48f, 1.10f,
      34.f, 17.f, 1.8f, 0.30f,
      ENG_I6, 310.f, 2800.f, 700.f, 5000.f, 2000.f, 0.f,
      5, {3.83f, 2.33f, 1.44f, 1.00f, 0.79f, 0.f}, 4.10f, 0.39f, 0.22f,
      DRIVE_4WD, 0.50f, 0.85f, 0.65f, false,
      {0.84f, 0.88f, 0.90f, 0.84f, 0.70f, 0.82f, 0.82f, 0.95f}, 1.40f, 1.18f, 0.16f, 0.45f,
      1.20f, 1.25f, 0.24f, 0.62f, 0.30f,
      45.f, 0.85f, 1.62f, 0.0f, COCKPIT_JEEP,
      V_STI, 20.f, 0.45f, 0.60f, 0.85f },
    { "STINGRAY BAJA", "VETTE", "LIFTED V8 ON ALL-TERRAINS. RWD.", "SHORTCUTS + DONUTS + 200 MPH", false,
      1450.f, 2.49f, 0.50f, 0.66f, 1.58f, 1.0f,
      32.f, 30.f, 3.9f, 0.30f,
      ENG_V8, 640.f, 4400.f, 750.f, 6400.f, 3000.f, 0.f,
      5, {2.64f, 1.75f, 1.34f, 1.00f, 0.78f, 0.f}, 3.55f, 0.40f, 0.14f,
      DRIVE_RWD, 0.0f, 1.15f, 0.60f, true,
      {1.08f, 0.80f, 0.78f, 0.62f, 0.48f, 0.66f, 0.68f, 0.76f}, 1.50f, 1.15f, 0.13f, 0.80f,
      0.86f, 1.75f, 0.45f, 0.60f, 0.24f,
      18.f, 0.45f, 1.38f, 0.0f, COCKPIT_VETTE,
      V_JEEP, 40.f, 1.0f, 0.95f, 0.60f },
    { "CR250 DIRT BIKE", "BIKE", "TWO-STROKE 250. KNOBBIES.", "GO ANYWHERE. NO ROADS. NO CLOCK.", true,
      190.f, 1.48f, 0.47f, 0.95f, 0.20f, 0.8f,
      28.f, 22.f, 4.5f, 0.35f,
      ENG_2STROKE, 42.f, 8000.f, 1600.f, 10000.f, 5000.f, 0.f,
      5, {2.20f, 1.68f, 1.36f, 1.13f, 0.96f, 0.f}, 11.8f, 0.33f, 0.06f,
      DRIVE_RWD, 0.0f, 1.05f, 0.65f, true,
      {0.92f, 0.95f, 1.00f, 0.86f, 0.70f, 0.88f, 0.94f, 0.92f}, 1.45f, 1.20f, 0.15f, 0.45f,
      0.55f, 1.35f, 0.42f, 0.55f, 0.32f,
      999.f, 0.60f, 1.52f, 0.25f, COCKPIT_BIKE,
      V_VETTE, 60.f, 0.85f, 0.80f, 1.0f },
};

const VehicleSpec& vehicleSpec(uint8_t id) { return VEHICLES[id % V_COUNT]; }

static float rollRes(uint8_t surf) {
    static const float RR[8] = {0.014f, 0.03f, 0.035f, 0.11f, 0.06f, 0.13f, 0.06f, 0.035f};
    return RR[surf & 7];
}

void vehicleReset(VehicleState& s, uint8_t id, float x, float z, float yaw) {
    const VehicleSpec& V = vehicleSpec(id);
    memset(&s, 0, sizeof(s));
    s.x = x; s.z = z; s.yaw = yaw;
    Ground g; terrainGround(x, z, g);
    float top = g.h + (g.mat == MAT_WATER ? g.water : 0.f);
    s.y = top + V.rideHeight; s.lastGY = top;
    s.gear = 1; s.rpm = V.idleRpm;
}

static float engineTorque(const VehicleSpec& V, float rpm) {
    float c;
    if (rpm < V.peakRpm) {
        float t = clampf((rpm - V.idleRpm) / (V.peakRpm - V.idleRpm), 0.f, 1.f);
        c = 0.52f + 0.48f * sqrtf(t);
    } else {
        float t = clampf((rpm - V.peakRpm) / (V.redline - V.peakRpm), 0.f, 1.2f);
        c = 1.f - 0.28f * t * t;
    }
    return V.torque * c;
}

// Pacejka-shaped lateral curve, normalised to 1 at the peak slip.
static inline float pac(float alpha, float C, float peak) {
    float B = tanf(1.5707963f / C) / peak;
    float a = fabsf(alpha);
    return sinf(C * atanf(B * a));
}

static void groundAt(float x, float z, float& h, Ground& g) {
    terrainGround(x, z, g);
    h = g.h + (g.mat == MAT_WATER ? g.water * 0.35f : 0.f);   // wading: you sit partly in it
}

static void substep(VehicleState& s, const VehicleSpec& V, uint8_t id, const VehicleInput& in, float dt) {
    const float m = V.mass, L = V.wheelbase;
    const float a = L * (1.f - V.frontWeight), b = L * V.frontWeight;   // CG to front / rear axle
    const float Iz = m * a * b * V.yawInertiaK;
    float cy = cosf(s.yaw), sy = sinf(s.yaw);
    // ---- ground under the four corners ----
    Ground gc, gf, gr, gl, grt;
    float hF, hR, hL, hRt, hC;
    groundAt(s.x + cy * a, s.z + sy * a, hF, gf);
    groundAt(s.x - cy * b, s.z - sy * b, hR, gr);
    float tw = V.bike ? 0.5f : V.track * 0.5f;
    groundAt(s.x + sy * tw, s.z - cy * tw, hL, gl);         // left = -right
    groundAt(s.x - sy * tw, s.z + cy * tw, hRt, grt);
    groundAt(s.x, s.z, hC, gc);
    float gY = (hF * b + hR * a) / L * 0.7f + hC * 0.3f;
    float slopeF = (hF - hR) / L;                    // + uphill
    float slopeR = (hRt - hL) / (2.f * tw);          // + right side higher
    s.groundPitch = atanf(slopeF);
    s.groundRoll = atanf(slopeR);
    s.surf = gc.surf; s.mat = gc.mat; s.across = gc.across; s.half = gc.half;
    s.water = gc.mat == MAT_WATER ? gc.water : 0.f;
    float gVy = (gY - s.lastGY) / dt;
    if (fabsf(gVy) > 40.f) gVy = 0.f;               // cache scroll / teleport, not a real ramp
    s.lastGY = gY;

    // ---- suspension: one spring under the CG ----
    float w0 = 6.2831853f * V.springHz;
    float kS = m * w0 * w0, cS = 2.f * V.damping * sqrtf(kS * m);
    float comp = (gY + V.rideHeight) - s.y;          // + compressed past the static ride height
    float sag = G / (w0 * w0);                       // the wheels hang this far before they leave the ground
    float N = m * G + kS * comp - cS * (s.vy - gVy);
    if (N < 0.f) N = 0.f;
    if (comp > V.travel) {                           // bottomed out: the chassis takes it
        float imp = gVy - s.vy;
        if (imp > 0.f) { s.evBottom = fmaxf(s.evBottom, imp); s.vy = gVy; }
        s.y = gY + V.rideHeight - V.travel;
        N = fmaxf(N, m * G * 1.5f);
        comp = V.travel;
    }
    s.comp = clampf((comp + sag) / (V.travel + sag), 0.f, 1.f);
    bool contact = N > 1.f;
    if (!contact) { s.airTime += dt; } 
    bool wasAir = s.air;
    s.air = s.airTime > 0.06f;
    if (contact) {
        if (wasAir) {
            float imp = fmaxf(0.f, gVy - s.vy);
            float mis = fabsf(wrapPiF(s.airPitch - s.groundPitch));
            s.evLand = fmaxf(s.evLand, imp + mis * 6.f);
            if (mis > 0.45f) { s.u *= 0.7f; s.v *= 0.5f; s.r *= 0.4f; }   // sketchy landing: scrubbed
            else if (mis > 0.25f) s.u *= 0.92f;
        }
        s.airTime = 0.f;
    }
    float load = N / (m * G);
    s.load = load;

    // ---- steering ----
    float spd = fabsf(s.u);
    float lock = V.lockDeg * 0.0174533f / sqrtf(1.f + (spd / V.steerRefSpeed) * (spd / V.steerRefSpeed));
    float beta = spd > 4.f ? atan2f(s.v, spd) : 0.f;
    float target = in.steer * lock + V.assist * beta * (1.f - 0.6f * fabsf(in.steer));
    if (s.reverse) target = in.steer * V.lockDeg * 0.0174533f;
    target = clampf(target, -V.lockDeg * 0.0174533f, V.lockDeg * 0.0174533f);
    float sr = V.steerRate * dt;
    s.steer += clampf(target - s.steer, -sr, sr);
    float d = s.steer;

    // ---- engine, clutch, box ----
    float throttle = in.throttle, brake = in.brake;
    if (s.reverse) { float t = throttle; throttle = brake; brake = t; }
    if (s.shiftT > 0.f) s.shiftT -= dt;
    float ratio = s.reverse ? -V.ratio[0] * 0.9f : V.ratio[s.gear - 1];
    float over = V.finalDrive * fabsf(ratio);
    float wheelRpm = spd / V.wheelRadius * over * 9.549f;
    float lowRange = 1.f;
    if (id == V_JEEP && spd < 7.f && (s.groundPitch > 0.09f || s.water > 0.2f) && throttle > 0.2f) lowRange = 2.6f;   // low range
    float clutchRpm = V.idleRpm + throttle * (V.launchRpm - V.idleRpm);
    float eRpm = wheelRpm < clutchRpm && s.gear == 1 ? fmaxf(wheelRpm, clutchRpm) : fmaxf(wheelRpm, V.idleRpm);
    eRpm += s.spin * (V.redline - eRpm) * 0.85f;
    if (s.air) eRpm = s.rpm + (throttle > 0.1f ? (V.redline - s.rpm) * dt * 6.f : -(s.rpm - V.idleRpm) * dt * 2.f);
    s.rpm += (eRpm - s.rpm) * clampf(dt * 18.f, 0.f, 1.f);
    if (s.rpm > V.redline * 1.03f) s.limiterT = 0.07f;
    if (s.limiterT > 0.f) s.limiterT -= dt;
    if (V.turbo > 0.f) {
        float bt = throttle * clampf((s.rpm - 2300.f) / 1700.f, 0.f, 1.f);
        s.boost += (bt - s.boost) * clampf(dt / (bt > s.boost ? V.turbo : 0.12f), 0.f, 1.f);
    }
    float T = engineTorque(V, s.rpm) * throttle * (V.turbo > 0.f ? 0.6f + 0.4f * s.boost : 1.f);
    if (throttle < 0.05f) T = -V.torque * 0.10f * clampf(s.rpm / V.redline, 0.f, 1.f) * (spd > 1.f ? 1.f : 0.f);
    if (s.limiterT > 0.f || s.shiftT > 0.f) T = fminf(T, 0.f);
    float Fdrive = T * over * 0.88f * lowRange / V.wheelRadius * (s.reverse ? -1.f : 1.f);
    if (s.reverse && fabsf(s.u) > 7.f && Fdrive < 0.f) Fdrive = 0.f;
    // auto box
    if (!s.reverse && !s.air && s.shiftT <= 0.f && s.spin < 0.4f) {
        float up = V.idleRpm + (V.redline - V.idleRpm) * (0.62f + 0.36f * throttle);
        float down = V.idleRpm + (V.redline - V.idleRpm) * (0.22f + 0.30f * brake + 0.1f * throttle);
        if (s.rpm > up && s.gear < V.gears) { s.gear++; s.shiftT = V.shiftTime; s.evShift = 1.f; }
        else if (s.gear > 1) {
            float lowerRpm = spd / V.wheelRadius * V.finalDrive * V.ratio[s.gear - 2] * 9.549f;
            if (s.rpm < down && lowerRpm < V.redline * 0.9f) { s.gear--; s.shiftT = V.shiftTime * 0.6f; s.evShift = -1.f; }
        }
    }
    // reverse: hold the brake at a standstill; throttle to go forward again
    if (!s.reverse && in.brake > 0.35f && spd < 0.6f && in.throttle < 0.05f) { s.revHold += dt; if (s.revHold > 0.45f) { s.reverse = true; s.revHold = 0.f; } }
    else if (s.reverse && in.throttle > 0.2f && s.u > -0.6f) { s.reverse = false; s.gear = 1; }
    else s.revHold = 0.f;

    // ---- loads (longitudinal transfer) ----
    float Fz = N > 0.f ? N : 0.f;
    float tr = m * s.longG * G * V.cgHeight / L;
    float Fzf = Fz * V.frontWeight - tr;
    float Fzr = Fz * (1.f - V.frontWeight) + tr;
    float wheelieT = 0.f;
    if (V.bike && Fzf < 0.08f * Fz && throttle > 0.3f && !s.air) wheelieT = clampf((0.08f * Fz - Fzf) / (Fz + 1.f) * 4.f, 0.f, 1.f);
    s.wheelie += (wheelieT * 0.75f - s.wheelie) * clampf(dt * (wheelieT > s.wheelie ? 3.f : 5.f), 0.f, 1.f);
    if (Fzf < 0.f) Fzf = 0.f;
    if (Fzr < 0.f) Fzr = 0.f;

    // ---- tyres ----
    bool loose = s.surf != SURF_TARMAC;
    float mu = V.mu[s.surf & 7] * (gc.mat == MAT_WATER ? 0.75f : 1.f);
    float C = loose ? V.looseC : V.tarmacC;
    float peak = V.peakSlip * (loose ? 1.6f : 1.f);
    float u0 = fmaxf(spd, 2.2f);
    float alphaF = atan2f(s.v + a * s.r, u0) - d * sgn(s.u + 0.01f);
    float alphaR = atan2f(s.v - b * s.r, u0);
    float FmaxF = mu * Fzf, FmaxR = mu * Fzr;
    if (V.bike) FmaxF *= 1.f - clampf(s.wheelie / 0.25f, 0.f, 1.f);
    // drive split and brakes
    float split = V.drive == DRIVE_RWD ? 0.f : V.frontSplit;
    float FxF = Fdrive * split, FxR = Fdrive * (1.f - split);
    float Fb = brake * V.brakeG * m * G;
    float dirU = s.u >= 0.f ? -1.f : 1.f;
    if (spd > 0.3f) { FxF += dirU * Fb * V.brakeBias; FxR += dirU * Fb * (1.f - V.brakeBias); }
    else if (brake > 0.05f && fabsf(Fdrive) < 1.f) { s.u *= 0.8f; }
    float latKeepF = 1.f, latKeepR = 1.f, spin = 0.f;
    if (in.handbrake && !V.bike) { FxR = (spd > 0.3f ? dirU : 0.f) * FmaxR * 0.8f; latKeepR = 0.28f; }
    else if (in.handbrake && V.bike) { FxR += (spd > 0.3f ? dirU : 0.f) * FmaxR * 0.7f; latKeepR = 0.35f; }
    if (fabsf(FxF) > FmaxF) {
        bool driving = FxF * s.u > 0.f || spd < 1.f;
        if (driving) spin = fmaxf(spin, clampf(fabsf(FxF) / (FmaxF + 1.f) - 1.f, 0.f, 1.f));
        if (!driving && V.abs) FxF = sgn(FxF) * FmaxF * 0.97f;
        else { FxF = sgn(FxF) * FmaxF * 0.82f; latKeepF = driving ? 0.45f : 0.30f; }
    }
    if (fabsf(FxR) > FmaxR && !in.handbrake) {
        bool driving = FxR * s.u > 0.f || spd < 1.f;
        if (driving) spin = fmaxf(spin, clampf(fabsf(FxR) / (FmaxR + 1.f) - 1.f, 0.f, 1.f));
        if (!driving && V.abs) FxR = sgn(FxR) * FmaxR * 0.97f;
        else { FxR = sgn(FxR) * FmaxR * 0.80f; latKeepR = driving ? 0.38f : 0.30f; }
    }
    s.spin += (clampf(spin * 2.5f, 0.f, 1.f) - s.spin) * clampf(dt * 10.f, 0.f, 1.f);
    float capF = sqrtf(fmaxf(0.f, FmaxF * FmaxF - FxF * FxF)) * latKeepF;
    float capR = sqrtf(fmaxf(0.f, FmaxR * FmaxR - FxR * FxR)) * latKeepR;
    if (latKeepF < 1.f) capF = fmaxf(capF, FmaxF * latKeepF);
    if (latKeepR < 1.f) capR = fmaxf(capR, FmaxR * latKeepR);
    float FyF = -sgn(alphaF) * pac(alphaF, C, peak) * FmaxF;
    float FyR = -sgn(alphaR) * pac(alphaR, C, peak) * FmaxR;
    float ramp = clampf(spd / 1.2f, 0.f, 1.f);       // no lateral jitter at a standstill
    FyF = clampf(FyF, -capF, capF) * ramp;
    FyR = clampf(FyR, -capR, capR) * ramp;
    s.slipF = clampf(fabsf(alphaF) / peak, 0.f, 2.f);
    s.slipR = clampf(fabsf(alphaR) / peak, 0.f, 2.f);
    s.gripNow = mu;
    if (s.air) { FxF = FxR = FyF = FyR = 0.f; }

    // ---- resistances ----
    float vel2 = s.u * s.u + s.v * s.v;
    float vmag = sqrtf(vel2);
    float drag = 0.6f * V.cdA * vmag;                                  // 0.5 rho CdA |v| (rho 1.2)
    float rr = rollRes(s.surf) * (loose ? V.rollMul : 1.f) * Fz;
    if (s.water > 0.05f) { drag += s.water * 260.f * vmag * 0.05f + s.water * 900.f / fmaxf(vmag, 1.f); }
    float Fx = FxF * cosf(d) - FyF * sinf(d) + FxR - drag * s.u - (spd > 0.05f ? rr * sgn(s.u) : 0.f);
    float Fy = FxF * sinf(d) + FyF * cosf(d) + FyR - drag * s.v;
    if (!s.air) {
        Fx -= m * G * sinf(s.groundPitch) * 0.95f;
        Fy -= m * G * sinf(s.groundRoll) * 0.9f;
    }
    float Mz = a * (FxF * sinf(d) + FyF * cosf(d)) - b * FyR;
    float du = Fx / m + s.v * s.r;
    float dv = Fy / m - s.u * s.r;
    float dr = Mz / Iz;
    s.longG = s.longG + ((Fx / m) / G - s.longG) * clampf(dt * 8.f, 0.f, 1.f);
    s.latG = s.latG + ((s.u * s.r + dv) / G - s.latG) * clampf(dt * 8.f, 0.f, 1.f);
    s.u += du * dt; s.v += dv * dt; s.r += dr * dt;
    if (s.air) s.r *= 1.f - dt * 0.3f;
    // parking-speed kinematics (but never while the rear is lit up: donuts are allowed)
    if (!s.air && spd < 3.f && s.spin < 0.25f && !in.handbrake) {
        float w = (1.f - spd / 3.f) * clampf(dt * 12.f, 0.f, 1.f);
        float rk = s.u * tanf(d) / L;
        s.r += (rk - s.r) * w;
        s.v += (rk * b - s.v) * w;
    }
    if (!s.air && spd < 0.15f && throttle < 0.05f && fabsf(Fx) < m * 0.6f) { s.u *= 0.9f; s.v *= 0.9f; }

    // ---- vertical ----
    float ay = N / m - G;
    s.vy += ay * dt;
    s.y += s.vy * dt;
    // ---- position ----
    float wx = cy * s.u - sy * s.v, wz = sy * s.u + cy * s.v;
    s.x += wx * dt; s.z += wz * dt;
    s.yaw += s.r * dt;
    if (s.yaw > 3.14159265f) s.yaw -= 6.2831853f;
    if (s.yaw < -3.14159265f) s.yaw += 6.2831853f;

    // ---- air attitude ----
    if (s.air) {
        if (V.bike) s.airPitchV += (in.airPitch * 3.2f - s.airPitchV * 1.5f) * dt;
        else s.airPitchV += (-0.35f - s.airPitchV * 0.8f) * dt;          // nose slowly drops
        s.airPitch += s.airPitchV * dt;
    } else {
        s.airPitch = s.groundPitch;
        s.airPitchV = (s.groundPitch - s.airPitch) / dt * 0.f + s.vy * 0.f;
    }
    // ---- body on its springs (what the camera feels) ----
    float wB = 6.2831853f * V.springHz * 1.1f, zB = V.damping;
    float pT = s.air ? s.airPitch : s.groundPitch - s.longG * 0.045f * (V.bike ? 0.5f : 1.f) + s.wheelie;
    // visual roll, right side down +: the ground's camber, and a car's body rolling to the outside of the turn
    float rT = s.air ? s.roll * 0.98f : -s.groundRoll - s.latG * (V.bike ? 0.f : 0.05f) * (V.cgHeight / 0.6f);
    s.pitchV += (-(wB * wB) * (s.pitch - pT) - 2.f * zB * wB * s.pitchV) * dt;
    s.rollV += (-(wB * wB) * (s.roll - rT) - 2.f * zB * wB * s.rollV) * dt;
    s.pitch += s.pitchV * dt;
    s.roll += s.rollV * dt;
    if (V.bike) {
        // a bike leans INTO the turn (right turn: lean right, +), as far as the cornering force asks
        // from the cornering force actually felt (a slide doesn't lay the bike down); upright at walking pace
        float leanT = s.air ? s.lean * 0.97f : (atanf(s.latG) + s.steer * 0.12f) * clampf((spd - 1.5f) / 5.f, 0.f, 1.f);
        leanT = clampf(leanT, -0.75f, 0.75f);
        s.lean += (leanT - s.lean) * clampf(dt * 7.f, 0.f, 1.f);
    }
    s.wheelRot += s.u * dt / V.wheelRadius;
    if (s.water > 0.25f && spd > 3.f) s.evSplash = fmaxf(s.evSplash, clampf(spd / 15.f, 0.f, 1.f));
    if (!s.air && fabsf(gVy - s.vy) > 2.5f) s.evBump = fmaxf(s.evBump, fabsf(gVy - s.vy) * 0.1f);
}

void vehicleStep(VehicleState& s, uint8_t id, const VehicleInput& in, float dt) {
    const VehicleSpec& V = vehicleSpec(id);
    int n = (int)ceilf(dt / (1.f / 120.f));
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    float h = dt / n;
    for (int i = 0; i < n; i++) substep(s, V, id, in, h);
    s.speed = sqrtf(s.u * s.u + s.v * s.v);
}

float vehicleHitPost(VehicleState& s, uint8_t id, float ox, float oz, float rad) {
    const VehicleSpec& V = vehicleSpec(id);
    float body = V.bike ? 0.45f : V.track * 0.6f;
    float dx = s.x - ox, dz = s.z - oz;
    float d = sqrtf(dx * dx + dz * dz);
    float minD = rad + body;
    if (d >= minD || d < 1e-4f) return 0.f;
    float nx = dx / d, nz = dz / d;
    s.x = ox + nx * minD; s.z = oz + nz * minD;
    float cy = cosf(s.yaw), sy = sinf(s.yaw);
    float wx = cy * s.u - sy * s.v, wz = sy * s.u + cy * s.v;
    float vn = wx * nx + wz * nz;
    if (vn >= 0.f) return 0.f;
    float rest = 0.25f;
    wx -= (1.f + rest) * vn * nx; wz -= (1.f + rest) * vn * nz;
    wx *= 0.55f; wz *= 0.55f;                      // trees don't give
    s.u = cy * wx + sy * wz; s.v = -sy * wx + cy * wz;
    s.r += (nx * sy - nz * cy) * vn * 0.12f;      // a glancing hit spins you a little
    (void)id;
    return -vn;
}

}
