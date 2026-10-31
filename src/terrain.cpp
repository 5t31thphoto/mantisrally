// The land and the road (see terrain.h). Dreamt ahead, forgotten behind: coherent, never stored.
#include "terrain.h"
#include "universe.h"
#include "mem.h"
#include <math.h>
#include <string.h>

namespace mantis {

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float smooth01(float t) { t = clampf(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }
static inline float sq(float v) { return v * v; }

static uint32_t s_seed = 1;

// =====================================================================================================
//  Natural land
// =====================================================================================================
float terrainWaterLevel(float x, float z) {
    const Land& L = journeyLand();
    float thresh = 0.20f + 0.32f * L.water;                         // more water: more of the low ground floods
    return journeyBase(x, z) + (thresh - 0.45f) * L.hills * 2.2f - 0.6f;
}

float terrainNatural(float x, float z) {
    const Land& L = journeyLand();
    float h = journeyBase(x, z);
    float n1 = fbm2(x * 0.0035f, z * 0.0035f, 4, s_seed + 0x11);
    h += (n1 - 0.45f) * L.hills * 2.2f;
    if (L.ridge > 0.3f) {                                            // sharp crests, Seadiver style
        float r = fbm2(x * 0.0062f, z * 0.0062f, 3, s_seed + 0x41);
        r = 1.f - fabsf(2.f * r - 1.f);
        h += (r * r * r - 0.3f) * L.ridge * 1.5f;
    }
    if (L.cliff > 0.3f) {                                            // terraces, mesas, rock steps
        float c = fbm2(x * 0.0042f, z * 0.0042f, 2, s_seed + 0xC1) * 4.f;
        float fl = floorf(c);
        float st = fl + smooth01((c - fl - 0.72f) * 5.f);
        h += (st * 0.25f - 0.45f) * L.cliff * 1.6f;
    }
    // medium bumps (the stuff you get air off) and fine roughness
    h += (vnoise2(x * 0.045f, z * 0.045f, s_seed + 0x70) - 0.5f) * L.rough * 3.2f;
    h += (vnoise2(x * 0.17f, z * 0.17f, s_seed + 0x71) - 0.5f) * L.rough * 0.55f;
    return h;
}

// =====================================================================================================
//  Road deque
// =====================================================================================================
static constexpr int RN = 512, RM = RN - 1, ARM_MAX = 200;
static RoadNode* s_rn = nullptr;
static int s_base = 0, s_cnt = 0;
static RoadNode* s_arm = nullptr;
static int s_armN = 0;
static inline RoadNode& nodeAt(int i) { return s_rn[(s_base + i) & RM]; }

enum : uint8_t { SEG_STRAIGHT, SEG_SWEEP, SEG_TIGHT, SEG_HAIRPIN };
struct Gen {
    float curv, tCurv, segLeft;
    uint8_t seg; int8_t sign; uint8_t pending;
    float grade, crestLeft, crestLen, crestAmp;
    float baseYaw;
    float sinceJunc, nextJunc, sinceStore, nextStore;
    uint32_t rng;
    int lockGrade;                       // nodes left on a fixed grade (the two arms of a fork stay level together)
};
static Gen s_gF, s_gB, s_gA;

static void rebakeAround(float x, float z, float r);
static void rebakeLater(float x, float z, float r);
static void rebakeCancel();
static void avoidSelf(struct Gen& g, const RoadNode& n, int dir);
static Junction s_junc;
static MotorSign s_mmSign;
static bool s_storePending = false;
static RoadSign s_sign;
static Store s_store;
static bool s_storeReq = false;
static bool s_juncEvent = false, s_juncTookArm = false;
static int s_carIdx = 0, s_carDir = 1;
static float s_carRoadDist = 0.f;

static void resetGen(Gen& g, float yaw, uint32_t salt) {
    memset(&g, 0, sizeof(g));
    g.rng = hash32(s_seed ^ salt);
    g.baseYaw = yaw;
    g.nextJunc = MILE * (1.5f + 1.5f * hashf(g.rng + 1));      // the first fork comes early; then every 3-7 miles
    g.nextStore = MILE * (1.2f + 1.0f * hashf(g.rng + 2));
}

static void newSegment(Gen& g, float h) {
    const Land& L = journeyLand();
    uint32_t r = g.rng = hash32(g.rng + 0x9E3779B9u);
    float u = hashf(r), u2 = hashf(r >> 7), u3 = hashf(r >> 13);
    float dev = wrapPi(h - g.baseYaw);
    int sgn = (r & 1) ? 1 : -1;
    if (fabsf(dev) > 0.8f) sgn = dev > 0.f ? -1 : 1;                 // wandered off the general bearing: come back
    if (g.pending == 1) {                                              // the short leg between switchbacks
        g.seg = SEG_STRAIGHT; g.tCurv = 0.f; g.segLeft = 16.f + 30.f * u; g.pending = 2; return;
    }
    if (g.pending == 2) {                                              // the matching hairpin back
        g.seg = SEG_HAIRPIN; g.sign = (int8_t)-g.sign;
        float R = 9.5f + 4.f * u2;
        g.tCurv = g.sign / R; g.segLeft = R * (2.6f + 0.4f * u3); g.pending = 0; return;
    }
    float wS = L.straight, wW = L.sweep, wT = L.tight, wH = L.hairpin;
    float tot = wS + wW + wT + wH;
    float pick = u * tot;
    g.baseYaw += (u3 - 0.5f) * 0.35f;
    if (pick < wS) {
        g.seg = SEG_STRAIGHT; g.tCurv = (u2 - 0.5f) * 0.0012f; g.segLeft = 40.f + 220.f * u2 * u3 + 40.f * wS;
    } else if (pick < wS + wW) {
        g.seg = SEG_SWEEP; g.sign = (int8_t)sgn;
        float R = 120.f + 260.f * u2;
        g.tCurv = sgn / R; g.segLeft = R * (0.3f + 0.6f * u3);
    } else if (pick < wS + wW + wT) {
        g.seg = SEG_TIGHT; g.sign = (int8_t)sgn;
        float R = 26.f + 50.f * u2;
        g.tCurv = sgn / R; g.segLeft = R * (0.5f + 0.9f * u3);
    } else {
        g.seg = SEG_HAIRPIN; g.sign = (int8_t)sgn;
        float R = 9.5f + 4.f * u2;
        g.tCurv = sgn / R; g.segLeft = R * (2.6f + 0.4f * u3); g.pending = 1;
    }
}

// One node outward from `from`. outward heading = from.yaw (front) or from.yaw + pi (back).
static RoadNode genStep(Gen& g, const RoadNode& from, int dir) {
    const Land& L = journeyLand();
    float h = dir > 0 ? from.yaw : from.yaw + PI_F;
    if (g.segLeft <= 0.f) newSegment(g, h);
    float kRate = 0.0065f * ROAD_DS;
    float dk = clampf(g.tCurv - g.curv, -kRate, kRate);
    g.curv += dk;
    float hMid = h + g.curv * ROAD_DS * 0.5f;
    RoadNode n;
    n.x = from.x + cosf(hMid) * ROAD_DS;
    n.z = from.z + sinf(hMid) * ROAD_DS;
    float hNew = h + g.curv * ROAD_DS;
    n.yaw = dir > 0 ? hNew : hNew - PI_F;
    n.s = from.s + dir * ROAD_DS;
    g.segLeft -= ROAD_DS;
    // ---- elevation: follow the land ahead, within the biome's grade, with crests and dips ----
    const float ahead = 45.f;
    float ax = n.x + cosf(hNew) * ahead, az = n.z + sinf(hNew) * ahead;
    float target = fmaxf(terrainNatural(ax, az), terrainWaterLevel(ax, az) + 1.0f);
    float here = fmaxf(terrainNatural(n.x, n.z), terrainWaterLevel(n.x, n.z) + 1.0f);
    target = target * 0.65f + here * 0.35f;
    float want = (target - from.y) / ahead;
    float vR = fmaxf(240.f, L.vRadius * 3.f);
    if (g.crestLeft > 0.f) {
        float ph = 1.f - g.crestLeft / g.crestLen;
        want += (ph < 0.5f ? 1.f : -1.f) * g.crestAmp;
        vR = L.vRadius;
        g.crestLeft -= ROAD_DS;
    } else if (hashf(g.rng ^ (uint32_t)(n.s * 7.f)) < L.crest * ROAD_DS / 110.f && fabsf(g.curv) < 0.03f) {
        g.crestLen = 24.f + 40.f * hashf(g.rng + 5);
        g.crestAmp = (0.05f + 0.07f * hashf(g.rng + 9)) * ((g.rng & 8) ? 1.f : -0.7f);
        g.crestLeft = g.crestLen;
    }
    want = clampf(want, -L.grade, L.grade);
    float gStep = ROAD_DS / vR;
    if (g.lockGrade > 0) { g.lockGrade--; g.crestLeft = 0.f; }
    else g.grade += clampf(want - g.grade, -gStep, gStep);
    n.y = from.y + g.grade * ROAD_DS;
    n.surf = L.surface;
    n.half20 = (uint8_t)clampf(L.roadHalf * 20.f, 40.f, 120.f);
    n.flags = g.crestLeft > 0.f && g.crestAmp > 0.f ? RN_CREST : 0;
    n.side = 0;
    g.sinceJunc += ROAD_DS;
    g.sinceStore += ROAD_DS;
    return n;
}

static void startRoad(float x, float z, float yaw) {
    s_base = 0; s_cnt = 1;
    RoadNode& n = nodeAt(0);
    n.x = x; n.z = z; n.yaw = yaw; n.s = 0.f;
    n.y = fmaxf(terrainNatural(x, z), terrainWaterLevel(x, z) + 1.0f);
    n.surf = journeyLand().surface; n.half20 = (uint8_t)(journeyLand().roadHalf * 20.f); n.flags = 0; n.side = 0;
    resetGen(s_gF, yaw, 0xF00Du + (uint32_t)(x * 3.f));
    resetGen(s_gB, yaw + PI_F, 0xBAC4u + (uint32_t)(z * 5.f));
    s_gF.segLeft = s_gB.segLeft = 60.f;
    s_carIdx = 0;
    s_junc.active = false; s_sign.active = false; s_armN = 0;
    s_mmSign.active = false; s_storePending = false; rebakeCancel();
}

int roadCount() { return s_cnt; }
const RoadNode& roadNode(int i) { return nodeAt(i); }
int roadArmCount() { return s_armN; }
const RoadNode& roadArmNode(int i) { return s_arm[i]; }
const Junction* roadJunction() { return &s_junc; }
const RoadSign* roadSign() { return &s_sign; }
const Store* roadStore() { return &s_store; }
void roadRequestStore() { s_storeReq = true; }
bool roadJunctionEvent(bool& tookArm) { bool e = s_juncEvent; s_juncEvent = false; tookArm = s_juncTookArm; return e; }

// ---- nearest point on a polyline ----
struct Poly { bool ring; int n; };
static inline const RoadNode& pnode(bool arm, int i) { return arm ? s_arm[i] : nodeAt(i); }

static float segProject(const RoadNode& a, const RoadNode& b, float x, float z, float& t) {
    float dx = b.x - a.x, dz = b.z - a.z;
    float L2 = dx * dx + dz * dz;
    t = L2 > 1e-6f ? clampf(((x - a.x) * dx + (z - a.z) * dz) / L2, 0.f, 1.f) : 0.f;
    float px = a.x + dx * t - x, pz = a.z + dz * t - z;
    return px * px + pz * pz;
}

static bool nearestOn(bool arm, int n, float x, float z, RoadHit& out, float maxD2) {
    if (n < 2) return false;
    // coarse pass every 4th node; keep the 3 best local minima (switchback legs are separate minima),
    // then refine each exactly against its segments
    const int STR = 4;
    float cd[RN / STR + 2];
    int m = 0;
    for (int i = 0; i < n; i += STR, m++) { const RoadNode& p = pnode(arm, i); cd[m] = sq(p.x - x) + sq(p.z - z); }
    int cand[3] = {-1, -1, -1}; float cv[3] = {1e30f, 1e30f, 1e30f};
    for (int k = 0; k < m; k++) {
        float d = cd[k];
        if ((k > 0 && cd[k - 1] < d) || (k + 1 < m && cd[k + 1] < d)) continue;     // not a local minimum
        for (int q = 0; q < 3; q++) if (d < cv[q]) {
            for (int r = 2; r > q; r--) { cv[r] = cv[r - 1]; cand[r] = cand[r - 1]; }
            cv[q] = d; cand[q] = k * STR; break;
        }
    }
    if (cand[0] < 0) return false;
    float lim = sqrtf(maxD2) + STR * ROAD_DS * 2.f;
    if (cv[0] > lim * lim) return false;
    float best = 1e30f; int bi = -1; float bt = 0.f;
    for (int c = 0; c < 3; c++) {
        if (cand[c] < 0 || cv[c] > sq(sqrtf(cv[0]) + 40.f)) continue;
        int lo = cand[c] - STR - 1, hi = cand[c] + STR + 1;
        if (lo < 0) lo = 0;
        if (hi > n - 2) hi = n - 2;
        for (int i = lo; i <= hi; i++) {
            float t; float d = segProject(pnode(arm, i), pnode(arm, i + 1), x, z, t);
            if (d < best) { best = d; bi = i; bt = t; }
        }
    }
    if (bi < 0 || best > maxD2) return false;
    const RoadNode& a = pnode(arm, bi);
    const RoadNode& b = pnode(arm, bi + 1);
    float px = a.x + (b.x - a.x) * bt, pz = a.z + (b.z - a.z) * bt;
    float yaw = atan2f(b.z - a.z, b.x - a.x);
    if (!arm || true) {
        // keep the front-direction convention (arm nodes are stored that way too)
        float ny = a.yaw + wrapPi(b.yaw - a.yaw) * bt;
        if (cosf(ny - yaw) < 0.f) yaw += PI_F;
    }
    out.across = -(x - px) * sinf(yaw) + (z - pz) * cosf(yaw);
    out.along = a.s + (b.s - a.s) * bt;
    out.y = a.y + (b.y - a.y) * bt;
    out.yaw = yaw;
    out.half = (a.half20 + (b.half20 - a.half20) * bt) * 0.05f;
    out.surf = a.surf;
    out.dist = sqrtf(best);
    out.arm = arm;
    out.idx = bi;
    return true;
}

bool roadNearest(float x, float z, RoadHit& h, float maxDist) {
    float m2 = maxDist * maxDist;
    RoadHit a, b;
    bool ha = nearestOn(false, s_cnt, x, z, a, m2);
    bool hb = s_armN > 1 && nearestOn(true, s_armN, x, z, b, m2);
    if (ha && (!hb || a.dist <= b.dist)) { h = a; return true; }
    if (hb) { h = b; return true; }
    return false;
}

float roadDistanceTo(float x, float z) {
    RoadHit h;
    return roadNearest(x, z, h, 4000.f) ? fmaxf(0.f, h.dist - h.half) : 1e6f;
}

bool roadRecover(float& x, float& z, float& yaw, float edge) {
    RoadHit h;
    if (!roadNearest(x, z, h, 5000.f)) return false;
    float target = clampf(h.across, -edge, edge);
    float shift = target - h.across;
    x += -sinf(h.yaw) * shift;
    z += cosf(h.yaw) * shift;
    float fwd = h.yaw;
    if (cosf(yaw - fwd) < 0.f) fwd += PI_F;
    yaw = fwd;
    return true;
}

// ---- junctions, signs, stores ----
static void spawnJunction(int dir) {
    int ji = dir > 0 ? s_cnt - 1 : 0;
    RoadNode J = nodeAt(ji);
    Gen& main = dir > 0 ? s_gF : s_gB;
    s_gA = main;
    s_gA.rng = hash32(main.rng ^ 0xA2A2u);
    int side = (s_gA.rng & 2) ? 1 : -1;
    float hOut = dir > 0 ? J.yaw : J.yaw + PI_F;
    // the arm peels away hard, the main road eases the other way: a proper Y
    s_gA.seg = SEG_TIGHT; s_gA.sign = (int8_t)side; s_gA.pending = 0;
    float R = 45.f + 30.f * hashf(s_gA.rng);
    s_gA.tCurv = side / R; s_gA.segLeft = R * (0.55f + 0.3f * hashf(s_gA.rng + 3));
    s_gA.baseYaw = hOut + side * 0.9f;
    s_gA.curv = 0.f;
    main.seg = SEG_SWEEP; main.sign = (int8_t)-side; main.pending = 0;
    main.tCurv = -side / 220.f; main.segLeft = 60.f;
    main.sinceJunc = 0.f;
    main.nextJunc = MILE * (3.f + 4.f * hashf(main.rng + 77));
    s_gA.sinceJunc = 0.f; s_gA.nextJunc = MILE * (3.f + 4.f * hashf(s_gA.rng + 78));
    main.lockGrade = s_gA.lockGrade = 22;          // both branches leave the split at the same grade
    main.crestLeft = s_gA.crestLeft = 0.f;
    // arm polyline, stored in the deque's front-direction convention
    s_armN = 0;
    s_arm[s_armN++] = J;
    RoadNode prev = J;
    for (int k = 1; k < ARM_MAX; k++) {
        RoadNode n = genStep(s_gA, prev, dir);
        if (k > 12) avoidSelf(s_gA, n, dir);
        s_arm[s_armN++] = n;
        rebakeAround(n.x, n.z, n.half20 * 0.05f + 10.f);
        prev = n;
    }
    s_junc.active = true; s_junc.decided = false;
    s_junc.x = J.x; s_junc.z = J.z; s_junc.yaw = hOut; s_junc.dir = dir; s_junc.armSide = side; s_junc.s = J.s;
    s_junc.destMain = journeyDestination();
    s_junc.destArm = journeyPickDestination(s_gA.rng, s_junc.destMain);
    s_junc.milesMain = journeyMilesTo(s_junc.destMain) ;
    s_junc.milesArm = journeyMilesTo(s_junc.destArm);
    // the sign stands ~60 m before the split on the approach's right
    int si = ji - dir * 20;
    if (si >= 0 && si < s_cnt) {
        const RoadNode& sn = nodeAt(si);
        float half = sn.half20 * 0.05f;
        float rx = -sinf(sn.yaw), rz = cosf(sn.yaw);
        float off = (half + 3.0f) * dir;            // right of travel
        s_sign.active = true; s_sign.x = sn.x + rx * off; s_sign.z = sn.z + rz * off;
        s_sign.yaw = dir > 0 ? sn.yaw : sn.yaw + PI_F; s_sign.side = (int8_t)dir;
    }
}

// The MANTIS MOTORS arrow sign goes up first; the store follows a quarter mile further on, on the same side.
static float s_storeAtS = 0.f;
static int s_storeSide = 1, s_storeDir = 1;                // side in the road's front frame; the end it grows at
const MotorSign* roadMotorSign() { return &s_mmSign; }
static void spawnStoreSign(int dir) {
    int si = dir > 0 ? s_cnt - 1 : 0;
    const RoadNode& n = nodeAt(si);
    Gen& g = dir > 0 ? s_gF : s_gB;
    s_storeSide = (g.rng & 4) ? 1 : -1;
    s_storeDir = dir;
    float half = n.half20 * 0.05f;
    float rx = -sinf(n.yaw), rz = cosf(n.yaw);
    s_mmSign.active = true;
    s_mmSign.x = n.x + rx * s_storeSide * (half + 2.8f);
    s_mmSign.z = n.z + rz * s_storeSide * (half + 2.8f);
    s_mmSign.travelSide = (int8_t)(s_storeSide * dir);      // + : the store is on your right as you drive toward it
    s_storePending = true;
    s_storeAtS = n.s + dir * 402.f;                         // a quarter mile on
    g.sinceStore = 0.f; g.nextStore = MILE * (3.5f + 2.5f * hashf(g.rng + 31));
    s_storeReq = false;
    rebakeAround(s_mmSign.x, s_mmSign.z, 6.f);
}

static void spawnStore(int dir) {
    int si = dir > 0 ? s_cnt - 1 : 0;
    RoadNode& n = nodeAt(si);
    Gen& g = dir > 0 ? s_gF : s_gB;
    int side = s_storeSide;
    s_storePending = false;
    n.flags |= RN_STORE; n.side = (uint8_t)(side > 0 ? 1 : 2);
    float half = n.half20 * 0.05f;
    float rx = -sinf(n.yaw), rz = cosf(n.yaw);
    s_store.active = true; s_store.s = n.s;
    s_store.x = n.x + rx * side * (half + 12.f);           // right beside the road
    s_store.z = n.z + rz * side * (half + 12.f);
    s_store.yaw = n.yaw;
    (void)g;
    rebakeLater(s_store.x, s_store.z, 48.f);                // its gravel lot and a clear frontage (a slice per frame)
}

// The road never crosses itself: if a new node comes near the road's own body, it bends away.
static void avoidSelf(Gen& g, const RoadNode& n, int dir) {
    // far body: stay 55 m clear.  near body (switchback legs): never closer than 18 m.
    float best = 1e9f; int bi = -1; bool close = false;
    for (int i = 0; i < s_cnt; i += 2) {
        int fromEnd = dir > 0 ? s_cnt - 1 - i : i;
        if (fromEnd < 20) continue;
        const RoadNode& p = nodeAt(i);
        float d = sq(p.x - n.x) + sq(p.z - n.z);
        float lim = fromEnd < 40 ? 18.f : 55.f;
        if (d < lim * lim && d < best) { best = d; bi = i; close = fromEnd < 40; }
    }
    if (bi < 0) return;
    const RoadNode& p = nodeAt(bi);
    float h = dir > 0 ? n.yaw : n.yaw + PI_F;
    float side = cosf(h) * (p.z - n.z) - sinf(h) * (p.x - n.x);   // + : the old road is on our right
    int away = side > 0.f ? -1 : 1;
    float R = close ? 18.f : (best < 40.f * 40.f ? 22.f : 35.f);
    g.seg = SEG_TIGHT; g.sign = (int8_t)away; g.pending = 0;
    g.tCurv = away / R; g.segLeft = close ? 20.f : 40.f;
    g.curv = g.curv * 0.5f + g.tCurv * 0.5f;
    g.baseYaw = h + away * 1.2f;
}

static void extend(int dir, bool facing) {
    Gen& g = dir > 0 ? s_gF : s_gB;
    if (s_cnt >= RN - 1) return;
    RoadNode from = dir > 0 ? nodeAt(s_cnt - 1) : nodeAt(0);
    RoadNode n = genStep(g, from, dir);
    avoidSelf(g, n, dir);
    if (dir > 0) { nodeAt(s_cnt) = n; s_cnt++; }
    else { s_base = (s_base - 1) & RM; nodeAt(0) = n; s_cnt++; s_carIdx++; }
    rebakeAround(n.x, n.z, n.half20 * 0.05f + 10.f);           // the road may wind back into land already dreamt
    if (!facing) return;
    bool calm = fabsf(g.curv) < 1.f / 140.f && g.pending == 0 && g.seg != SEG_HAIRPIN;
    if (s_storePending) {                                   // the sign is up: the store comes a quarter mile on
        if (dir == s_storeDir && (n.s - s_storeAtS) * dir >= 0.f && calm) spawnStore(dir);
        return;                                             // (and no fork between a sign and its store)
    }
    if (!s_junc.active && g.sinceJunc > g.nextJunc && calm) spawnJunction(dir);
    // the store only goes on the road you are actually on: never while a fork ahead is undecided
    else if (!s_store.active && !s_mmSign.active && calm && g.sinceStore > g.nextStore * (s_storeReq ? 0.35f : 1.f)
             && (!s_junc.active || (s_junc.decided && fabsf(n.s - s_junc.s) > 400.f))) spawnStoreSign(dir);
}

static void dropBack() { s_base = (s_base + 1) & RM; s_cnt--; s_carIdx--; }
static void dropFront() { s_cnt--; }

// Take the arm: the old continuation becomes the (still drawn) side road.
static void takeArm() {
    int dir = s_junc.dir;
    int ji = (int)lroundf((s_junc.s - nodeAt(0).s) / ROAD_DS);
    if (ji < 0 || ji >= s_cnt) return;
    static RoadNode ghost[ARM_MAX];
    int gn = 0;
    ghost[gn++] = nodeAt(ji);
    if (dir > 0) {
        for (int i = ji + 1; i < s_cnt && gn < ARM_MAX; i++) ghost[gn++] = nodeAt(i);
        s_cnt = ji + 1;
        for (int k = 1; k < s_armN && s_cnt < RN - 1; k++) { nodeAt(s_cnt) = s_arm[k]; s_cnt++; }
        s_gF = s_gA;
    } else {
        for (int i = ji - 1; i >= 0 && gn < ARM_MAX; i--) ghost[gn++] = nodeAt(i);
        int drop = ji;                                   // nodes before the split go
        s_base = (s_base + drop) & RM; s_cnt -= drop; s_carIdx -= drop;
        for (int k = 1; k < s_armN && s_cnt < RN - 1; k++) { s_base = (s_base - 1) & RM; nodeAt(0) = s_arm[k]; s_cnt++; s_carIdx++; }
        s_gB = s_gA;
    }
    memcpy(s_arm, ghost, sizeof(RoadNode) * gn);
    s_armN = gn;
}

// Track where the car is along the road.
static void trackCar(float x, float z, float yaw) {
    if (s_cnt < 2) return;
    if (s_carIdx < 0) s_carIdx = 0;
    if (s_carIdx > s_cnt - 1) s_carIdx = s_cnt - 1;
    int lo = s_carIdx - 12, hi = s_carIdx + 12;
    if (lo < 0) lo = 0;
    if (hi > s_cnt - 1) hi = s_cnt - 1;
    float best = 1e30f; int bi = s_carIdx;
    for (int i = lo; i <= hi; i++) { const RoadNode& p = nodeAt(i); float d = sq(p.x - x) + sq(p.z - z); if (d < best) { best = d; bi = i; } }
    if (best > 60.f * 60.f) {                                    // lost it: full search
        for (int i = 0; i < s_cnt; i += 2) { const RoadNode& p = nodeAt(i); float d = sq(p.x - x) + sq(p.z - z); if (d < best) { best = d; bi = i; } }
    }
    s_carIdx = bi;
    s_carRoadDist = sqrtf(best);
    float c = cosf(yaw - nodeAt(bi).yaw);
    if (c > 0.25f) s_carDir = 1; else if (c < -0.25f) s_carDir = -1;
}

// =====================================================================================================
//  Caches
// =====================================================================================================
static Grid s_near, s_far;
static float s_lagYaw = 0.f;
static int s_frame = 0;

const Grid& terrainNear() { return s_near; }
const Grid& terrainFar() { return s_far; }

static inline uint8_t encN(float nx, float nz) {
    int a = (int)lroundf(clampf(nx, -1.f, 1.f) * 7.f) + 8, b = (int)lroundf(clampf(nz, -1.f, 1.f) * 7.f) + 8;
    return (uint8_t)((a << 4) | b);
}

static float cutH(float nat, float a, float half, float roadY, bool paved, float steep) {
    float flatW = half + 1.3f;
    if (a <= flatW) {
        float crown = a < half ? (paved ? 0.07f : 0.12f) * (1.f - sq(a / half)) : -(a - half) * 0.10f;
        return roadY + crown;
    }
    float bw = 4.f + 9.f * (1.f - steep);
    float t = (a - flatW) / bw;
    if (t >= 1.f) return nat;
    float ts = smooth01(t);
    float ditch = (!paved && t < 0.6f) ? -0.45f * sinf(t * (PI_F / 0.6f)) * (1.f - t) : 0.f;
    return roadY - 0.13f + (nat - roadY + 0.13f) * ts + ditch;
}

static uint8_t pickVeg(float x, float z, int ci, int cj, uint8_t mat, float aRoad, float half) {
    const Land& L = journeyLand();
    if (mat == MAT_ROAD || mat == MAT_SHOULDER || mat == MAT_LOT || mat == MAT_WATER) return VEG_NONE;
    if (aRoad < half + 3.2f) return VEG_NONE;
    if (s_store.active && sq(x - s_store.x) + sq(z - s_store.z) < 46.f * 46.f) return VEG_NONE;   // nothing hides the dealership
    uint32_t h = hash32((uint32_t)ci * 73856093u ^ (uint32_t)cj * 19349663u ^ s_seed);
    float cl = vnoise2(x * 0.011f, z * 0.011f, s_seed + 0x5E);            // groves and clearings
    float grove = smooth01((cl - 0.22f) * 2.2f) * 1.25f;
    float k = 0.17f;
    float r = hashf(h);
    float acc = 0.f;
    acc += L.pine * grove * k;           if (r < acc) return VEG_PINE;
    acc += L.broad * grove * k;          if (r < acc) return VEG_BROAD;
    acc += L.redwood * grove * k * 0.6f; if (r < acc) return VEG_REDWOOD;
    acc += L.snag * k * 0.4f;            if (r < acc) return VEG_SNAG;
    acc += L.cactus * k * 0.35f;         if (r < acc) return VEG_CACTUS;
    acc += L.boulder * k * 0.3f;         if (r < acc) return VEG_BOULDER;
    acc += L.bush * k * 0.6f;            if (r < acc) return mat == MAT_SNOW ? VEG_NONE : VEG_BUSH;
    acc += L.flowers * k * 0.9f;         if (r < acc) return (mat == MAT_GRASS) ? VEG_FLOWERS : VEG_NONE;
    return VEG_NONE;
}

#ifdef DEBUG_BAKE
int g_dbgFrame = 0; float g_dbgRoadY[96*96]; int g_dbgWhen[96*96]; int g_dbgHit[96*96];
#endif
static void bakeCell(Grid& g, int ci, int cj) {
    float x = ci * g.cell, z = cj * g.cell;
    const Land& L = journeyLand();
    float e = g.cell * 0.5f;
    float n0 = terrainNatural(x, z), nX = terrainNatural(x + e, z), nZ = terrainNatural(x, z + e);
    float wl = terrainWaterLevel(x, z);
    float h = n0, hX = nX, hZ = nZ;
    float across = 600.f, along = 0.f, half = 3.4f;
    uint8_t surf = SURF_GRASS, mat = L.ground;
    RoadHit rh;
    bool road = roadNearest(x, z, rh, 40.f);
    float steep = clampf((L.cliff + L.ridge) / 50.f, 0.f, 1.f);
    bool lot = false;
    if (road) {
        across = rh.across; along = rh.along; half = rh.half;
        bool paved = rh.surf == SURF_TARMAC;
        float sX = -sinf(rh.yaw), sZ = cosf(rh.yaw);
        float gx = cosf(rh.yaw), gz = sinf(rh.yaw);
        float gr = 0.f;
        h = cutH(n0, fabsf(across), half, rh.y, paved, steep);
        hX = cutH(nX, fabsf(across + e * sX), half, rh.y + gr * e * gx, paved, steep);
        hZ = cutH(nZ, fabsf(across + e * sZ), half, rh.y + gr * e * gz, paved, steep);
        if (fabsf(across) <= half) { mat = MAT_ROAD; surf = rh.surf; }
        else if (fabsf(across) <= half + 1.3f) { mat = MAT_SHOULDER; surf = rh.surf == SURF_TARMAC ? SURF_GRAVEL : rh.surf; }
    }
    if (s_store.active && mat != MAT_ROAD) {                      // the dealership lot: flat gravel
        float dx = x - s_store.x, dz = z - s_store.z;
        float fa = dx * cosf(s_store.yaw) + dz * sinf(s_store.yaw);
        float fl = -dx * sinf(s_store.yaw) + dz * cosf(s_store.yaw);
        float ra = fabsf(fa), rl = fabsf(fl);
        if (ra < 30.f && rl < 26.f) {
            RoadHit sh; float ly = road ? rh.y : h;
            if (roadNearest(s_store.x, s_store.z, sh, 80.f)) ly = sh.y;
            // next to the road the lot meets the road at the road's own height (a sloping road leaves no step)
            if (road) { float e = clampf((fabsf(across) - half) / 10.f, 0.f, 1.f); ly = rh.y + (ly - rh.y) * e; }
            float t = smooth01(fmaxf((ra - 18.f) / 12.f, (rl - 15.f) / 11.f));
            h = ly - 0.05f + (h - ly) * t; hX = ly + (hX - ly) * t; hZ = ly + (hZ - ly) * t;
            if (t < 0.05f) { mat = MAT_LOT; surf = SURF_GRAVEL; lot = true; }
        }
    }
    float depth = 0.f;
    if (mat != MAT_ROAD && mat != MAT_SHOULDER && !lot && h < wl) {
        depth = wl - h; mat = MAT_WATER; surf = SURF_MUD;
    }
    float nx = -(hX - h) / e, nz = -(hZ - h) / e;
    float inv = 1.f / sqrtf(nx * nx + 1.f + nz * nz);
    nx *= inv; nz *= inv; float ny = inv;
    if (mat != MAT_ROAD && mat != MAT_SHOULDER && mat != MAT_WATER && !lot) {
        float p = vnoise2(x * 0.06f, z * 0.06f, s_seed + 0xD1);
        float base = journeyBase(x, z);
        if (h - base > L.snowLine + (p - 0.5f) * 14.f) { mat = MAT_SNOW; }
        else if (ny < 0.72f + 0.08f * p) { mat = MAT_ROCK; }
        else if (L.ground == MAT_SAND) { mat = p > 0.66f ? MAT_SOIL : MAT_SAND; }
        else if (L.ground == MAT_SNOW) { mat = p > 0.8f ? MAT_ROCK : MAT_SNOW; }
        else { mat = p > 0.70f ? (L.water > 0.7f ? MAT_MUD : MAT_SOIL) : MAT_GRASS; }
        static const uint8_t TYRE[] = {SURF_GRASS, SURF_DIRT, SURF_ROCK, SURF_SNOW, SURF_SAND, SURF_MUD};
        surf = TYRE[mat];
    }
    Cell& c = g.c[gridWrap(cj, g.N) * g.N + gridWrap(ci, g.N)];
    float hs = mat == MAT_WATER ? wl : h;
    c.h = (int16_t)clampf(hs * 10.f, -32000.f, 32000.f);
    c.across = (int16_t)clampf(across * 50.f, -30000.f, 30000.f);
    float al = along * 20.f; al -= 256.f * floorf(al / 256.f);
    c.along = (uint8_t)al;
    c.mat = (uint8_t)((surf << 4) | mat);
    c.nrm = mat == MAT_WATER ? encN(0.f, 0.f) : encN(nx, nz);
    if (mat == MAT_ROAD || mat == MAT_SHOULDER) c.extra = (uint8_t)clampf(half * 20.f, 0.f, 255.f);
    else if (mat == MAT_WATER) c.extra = (uint8_t)clampf(depth * 20.f, 0.f, 255.f);
    else if (&g == &s_near) c.extra = pickVeg(x, z, ci, cj, mat, fabsf(across), half);
    else {                                                        // far: forest canopy height in metres
        float cl = vnoise2(x * 0.011f, z * 0.011f, s_seed + 0x5E);
        float dens = (L.pine + L.broad + L.redwood * 1.5f) * smooth01((cl - 0.22f) * 2.2f);
        bool nearStore = s_store.active && sq(x - s_store.x) + sq(z - s_store.z) < 60.f * 60.f;
        c.extra = (!nearStore && dens > 0.35f && mat != MAT_ROCK && fabsf(across) > half + 6.f) ? (uint8_t)clampf(5.f + dens * 6.f + L.redwood * 12.f, 0.f, 22.f) : 0;
    }
#ifdef DEBUG_BAKE
    if (&g == &s_near) { int k = gridWrap(cj, g.N) * g.N + gridWrap(ci, g.N); g_dbgRoadY[k] = road ? rh.y : -999.f; g_dbgWhen[k] = g_dbgFrame; g_dbgHit[k] = road ? (rh.arm ? 1000 + rh.idx : rh.idx) : -1; }
#endif
    if (hs > g.maxH) g.maxH = hs;
}

static void gridInit(Grid& g, int N, float cell) {
    g.N = N; g.cell = cell; g.inv = 1.f / cell;
    if (!g.c) g.c = (Cell*)(N == 96 ? memFast(sizeof(Cell) * N * N) : memSlow(sizeof(Cell) * N * N));
    g.jobAxis = -1; g.jobPos = N; g.rebake = -1; g.maxH = -1e9f;
}

static void gridFill(Grid& g, float cx, float cz) {
    if (!g.c) return;
    g.ox = (int)floorf(cx * g.inv) - g.N / 2;
    g.oz = (int)floorf(cz * g.inv) - g.N / 2;
    g.maxH = -1e9f;
    for (int j = 0; j < g.N; j++) for (int i = 0; i < g.N; i++) bakeCell(g, g.ox + i, g.oz + j);
    g.jobPos = g.N; g.rebake = -1;
}

static void gridScroll(Grid& g, float cx, float cz, int budget) {
    if (!g.c) return;
    int wx = (int)floorf(cx * g.inv) - g.N / 2;
    int wz = (int)floorf(cz * g.inv) - g.N / 2;
    if (abs(wx - g.ox) > g.N / 2 || abs(wz - g.oz) > g.N / 2) { gridFill(g, cx, cz); return; }
    while (budget > 0) {
        if (g.jobPos >= g.N) {
            if (g.ox < wx) { g.ox++; g.jobAxis = 0; g.jobLine = g.ox + g.N - 1; }
            else if (g.ox > wx) { g.ox--; g.jobAxis = 0; g.jobLine = g.ox; }
            else if (g.oz < wz) { g.oz++; g.jobAxis = 1; g.jobLine = g.oz + g.N - 1; }
            else if (g.oz > wz) { g.oz--; g.jobAxis = 1; g.jobLine = g.oz; }
            else break;
            g.jobPos = 0;
        }
        int n = g.N - g.jobPos < budget ? g.N - g.jobPos : budget;
        for (int k = 0; k < n; k++, g.jobPos++) {
            if (g.jobAxis == 0) bakeCell(g, g.jobLine, g.oz + g.jobPos);
            else bakeCell(g, g.ox + g.jobPos, g.jobLine);
        }
        budget -= n;
    }
    if (budget > 0 && g.rebake >= 0) {
        int total = g.N * g.N;
        while (budget-- > 0 && g.rebake < total) {
            int k = g.rebake++;
            bakeCell(g, g.ox + k % g.N, g.oz + k / g.N);
        }
        if (g.rebake >= total) g.rebake = -1;
    }
}

// re-dream the cells around a point if they are in a cache window (a road arrived after they were dreamt)
static void rebakeIn(Grid& g, float x, float z, float r) {
    if (!g.c) return;
    int i0 = (int)floorf((x - r) * g.inv), i1 = (int)floorf((x + r) * g.inv);
    int j0 = (int)floorf((z - r) * g.inv), j1 = (int)floorf((z + r) * g.inv);
    if (i1 < g.ox || j1 < g.oz || i0 >= g.ox + g.N || j0 >= g.oz + g.N) return;
    if (i0 < g.ox) i0 = g.ox;
    if (j0 < g.oz) j0 = g.oz;
    if (i1 > g.ox + g.N - 1) i1 = g.ox + g.N - 1;
    if (j1 > g.oz + g.N - 1) j1 = g.oz + g.N - 1;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) bakeCell(g, i, j);
}
static void rebakeAround(float x, float z, float r) { rebakeIn(s_near, x, z, r); rebakeIn(s_far, x, z, r); }
// a big region (the dealership's lot) is rebuilt a slice per frame instead of all at once: no hitch
struct Pending { bool on; float x, z, r; int row; };
static Pending s_pend = {false, 0.f, 0.f, 0.f, 0};
static void rebakeCancel() { s_pend.on = false; }
static void rebakeLater(float x, float z, float r) { s_pend.on = true; s_pend.x = x; s_pend.z = z; s_pend.r = r; s_pend.row = 0; }
static void rebakeStep() {
    if (!s_pend.on) return;
    const float strip = 6.f;                                  // metres of the region per frame
    float z0 = s_pend.z - s_pend.r + s_pend.row * strip;
    if (z0 > s_pend.z + s_pend.r) { s_pend.on = false; return; }
    s_pend.row++;
    Grid* gs[2] = {&s_near, &s_far};
    for (Grid* gp : gs) {
        Grid& g = *gp;
        if (!g.c) continue;
        int i0 = (int)floorf((s_pend.x - s_pend.r) * g.inv), i1 = (int)floorf((s_pend.x + s_pend.r) * g.inv);
        int j0 = (int)floorf(z0 * g.inv), j1 = (int)floorf((z0 + strip) * g.inv) - 1;
        if (j1 < j0) j1 = j0;
        for (int j = j0; j <= j1; j++) {
            if (j < g.oz || j >= g.oz + g.N) continue;
            for (int i = i0; i <= i1; i++) if (i >= g.ox && i < g.ox + g.N) bakeCell(g, i, j);
        }
    }
}

static void gridMax(Grid& g) {
    if (!g.c) return;
    float m = -1e9f;
    for (int k = 0; k < g.N * g.N; k++) { float h = g.c[k].h * 0.1f + (g.c[k].mat & 15) * 0.f; if (h > m) m = h; }
    g.maxH = m + 40.f;                            // + the tallest tree / canopy
}

static void windowCentres(float x, float z, float& nx, float& nz, float& fx, float& fz) {
    float c = cosf(s_lagYaw), s = sinf(s_lagYaw);
    nx = x + c * 45.f; nz = z + s * 45.f;
    fx = x + c * 150.f; fz = z + s * 150.f;
}

void terrainRebuild() {
    float nx, nz, fx, fz;
    float x = 0.f, z = 0.f;
    if (s_cnt > 0) { x = nodeAt(s_carIdx).x; z = nodeAt(s_carIdx).z; }
    windowCentres(x, z, nx, nz, fx, fz);
    gridFill(s_near, nx, nz);
    gridFill(s_far, fx, fz);
    gridMax(s_near); gridMax(s_far);
}

void terrainInit(uint32_t seed) {
    s_seed = seed ? seed : 1;
    if (!s_rn) s_rn = (RoadNode*)memFast(sizeof(RoadNode) * RN);
    if (!s_arm) s_arm = (RoadNode*)memSlow(sizeof(RoadNode) * ARM_MAX);
    memset(&s_junc, 0, sizeof(s_junc)); memset(&s_sign, 0, sizeof(s_sign)); memset(&s_store, 0, sizeof(s_store));
    memset(&s_mmSign, 0, sizeof(s_mmSign)); s_storePending = false;
    gridInit(s_near, 96, 2.5f);
    gridInit(s_far, 64, 8.f);
    s_lagYaw = 0.f;
    startRoad(0.f, 0.f, 0.f);
    for (int k = 0; k < 170; k++) { extend(1, false); extend(-1, false); }
    s_carIdx = 0;
    for (int i = 0; i < s_cnt; i++) if (fabsf(nodeAt(i).s) < 0.1f) s_carIdx = i;
    terrainRebuild();
}

void roadSpawnCrossing(float cx, float cz, float yaw) {
    uint32_t h = hash32(s_seed ^ (uint32_t)(cx * 3.f) ^ (uint32_t)(cz * 7.f));
    float d = 520.f;
    float px = cx + cosf(yaw) * d, pz = cz + sinf(yaw) * d;
    float ry = yaw + PI_F * 0.5f + (hashf(h) - 0.5f) * 1.2f;
    startRoad(px, pz, ry);
    for (int k = 0; k < 160; k++) { extend(1, false); extend(-1, false); }
    s_carIdx = 0;
    for (int i = 0; i < s_cnt; i++) if (fabsf(nodeAt(i).s) < 0.1f) s_carIdx = i;
    s_near.rebake = 0; s_far.rebake = 0;
}

void terrainUpdate(float carX, float carZ, float carYaw, float speed, float dt) {
    s_frame++;
    // the cache windows look ahead along a lagged heading (a donut doesn't thrash them)
    float k = clampf(dt * (0.25f + fabsf(speed) * 0.03f), 0.f, 1.f);
    s_lagYaw += wrapPi(carYaw - s_lagYaw) * k;

    if (s_cnt >= 2) {
        trackCar(carX, carZ, carYaw);
        bool nearRoad = s_carRoadDist < 160.f;
        // keep ~450 m of road dreamt ahead of the car at both ends, forget beyond ~700 m
        const int AHEAD = 150, KEEP = 233;
        int ext = 0;
        while (nearRoad && ext < 6 && s_cnt - 1 - s_carIdx < AHEAD && s_cnt < RN - 2) { extend(1, s_carDir > 0); ext++; }
        while (nearRoad && ext < 12 && s_carIdx < AHEAD && s_cnt < RN - 2) { extend(-1, s_carDir < 0); ext++; }
        while (s_carIdx > KEEP && s_cnt > 2) dropBack();
        while (s_cnt - 1 - s_carIdx > KEEP && s_cnt > 2) dropFront();
        // a free roamer far from any road: drop it and dream a new one crossing ahead
        if (s_carRoadDist > 750.f) roadSpawnCrossing(carX, carZ, carYaw);
    }
    // ---- the fork ----
    if (s_junc.active) {
        float dx = carX - s_junc.x, dz = carZ - s_junc.z;
        float past = (dx * cosf(s_junc.yaw) + dz * sinf(s_junc.yaw));
        float dist = sqrtf(dx * dx + dz * dz);
        if (!s_junc.decided && past > 22.f && dist < 120.f) {
            RoadHit m, a;
            bool hm = nearestOn(false, s_cnt, carX, carZ, m, 1e9f);
            bool ha = s_armN > 1 && nearestOn(true, s_armN, carX, carZ, a, 1e9f);
            bool arm = ha && (!hm || a.dist < m.dist);
            if (arm) takeArm();
            s_junc.decided = true;
            // a store left on the road not taken is gone; the next one comes soon on this road
            if (s_store.active) {
                RoadHit sh;
                if (!roadNearest(s_store.x, s_store.z, sh, 60.f) || sh.arm) {
                    s_store.active = false;
                    Gen& g = s_junc.dir > 0 ? s_gF : s_gB;
                    g.sinceStore = g.nextStore;
                }
            }
            s_juncEvent = true; s_juncTookArm = arm;
            journeyFollowRoad(arm ? s_junc.destArm : s_junc.destMain);
        }
        if (dist > 650.f) { s_junc.active = false; s_armN = 0; s_sign.active = false; }
    }
    if (s_store.active) {
        float dx = carX - s_store.x, dz = carZ - s_store.z;
        if (dx * dx + dz * dz > 750.f * 750.f) s_store.active = false;
    }
    if (s_mmSign.active) {
        float dx = carX - s_mmSign.x, dz = carZ - s_mmSign.z;
        if (dx * dx + dz * dz > 900.f * 900.f) { s_mmSign.active = false; s_storePending = false; }
    }
    // ---- caches ----
    float nx, nz, fx, fz;
    windowCentres(carX, carZ, nx, nz, fx, fz);
    rebakeStep();
    gridScroll(s_near, nx, nz, 200);
    gridScroll(s_far, fx, fz, 72);
    if ((s_frame & 31) == 0) gridMax(s_near);
    if ((s_frame & 31) == 16) gridMax(s_far);
}

// =====================================================================================================
//  Queries
// =====================================================================================================
static bool gridInside(const Grid& g, float gx, float gz) {
    int ix = (int)floorf(gx), iz = (int)floorf(gz);
    return g.c && ix >= g.ox + 1 && iz >= g.oz + 1 && ix + 2 < g.ox + g.N && iz + 2 < g.oz + g.N;
}

void terrainGround(float x, float z, Ground& o) {
    const Grid* gp = nullptr;
    if (gridInside(s_near, x * s_near.inv, z * s_near.inv)) gp = &s_near;
    else if (gridInside(s_far, x * s_far.inv, z * s_far.inv)) gp = &s_far;
    if (gp) {
        const Grid& g = *gp;
        float gx = x * g.inv, gz = z * g.inv;
        int ix = (int)floorf(gx), iz = (int)floorf(gz);
        float fx = gx - ix, fz = gz - iz;
        int N = g.N;
        int x0 = gridWrap(ix, N), x1 = gridWrap(ix + 1, N), z0 = gridWrap(iz, N) * N, z1 = gridWrap(iz + 1, N) * N;
        const Cell &a = g.c[z0 + x0], &b = g.c[z0 + x1], &c = g.c[z1 + x0], &d = g.c[z1 + x1];
        float ha = a.h * 0.1f, hb = b.h * 0.1f, hc = c.h * 0.1f, hd = d.h * 0.1f;
        float h0 = ha + (hb - ha) * fx, h1 = hc + (hd - hc) * fx;
        o.h = h0 + (h1 - h0) * fz;
        float dhx = ((hb - ha) * (1.f - fz) + (hd - hc) * fz) * g.inv;
        float dhz = ((hc - ha) * (1.f - fx) + (hd - hb) * fx) * g.inv;
        float inv = 1.f / sqrtf(dhx * dhx + 1.f + dhz * dhz);
        o.nx = -dhx * inv; o.ny = inv; o.nz = -dhz * inv;
        float aa = a.across * 0.02f, ab = b.across * 0.02f, ac = c.across * 0.02f, ad = d.across * 0.02f;
        float a0 = aa + (ab - aa) * fx, a1 = ac + (ad - ac) * fx;
        o.across = a0 + (a1 - a0) * fz;
        const Cell& n = (fx < 0.5f) ? (fz < 0.5f ? a : c) : (fz < 0.5f ? b : d);
        o.mat = n.mat & 15; o.surf = n.mat >> 4;
        o.half = (o.mat == MAT_ROAD || o.mat == MAT_SHOULDER) ? n.extra * 0.05f : 3.4f;
        o.water = o.mat == MAT_WATER ? n.extra * 0.05f : 0.f;
        o.along = 0.f;
        o.cached = true;
        if (o.mat == MAT_WATER) o.h -= o.water;           // the bed, not the surface
        return;
    }
    // outside both caches: compute directly (rare: teleports, tests)
    float n0 = terrainNatural(x, z);
    o.h = n0; o.nx = 0.f; o.ny = 1.f; o.nz = 0.f; o.across = 600.f; o.along = 0.f; o.half = 3.4f;
    o.mat = journeyLand().ground; o.surf = SURF_GRASS; o.water = 0.f; o.cached = false;
    RoadHit rh;
    if (roadNearest(x, z, rh, 40.f)) {
        o.across = rh.across; o.along = rh.along; o.half = rh.half;
        o.h = cutH(n0, fabsf(rh.across), rh.half, rh.y, rh.surf == SURF_TARMAC, 0.5f);
        if (fabsf(rh.across) <= rh.half) { o.mat = MAT_ROAD; o.surf = rh.surf; }
    }
    float wl = terrainWaterLevel(x, z);
    if (o.mat != MAT_ROAD && o.h < wl) { o.mat = MAT_WATER; o.water = wl - o.h; }
}

float terrainHeight(float x, float z) { Ground g; terrainGround(x, z, g); return g.mat == MAT_WATER ? g.h + g.water : g.h; }

bool vegAt(int ci, int cj, Veg& v) {
    const Grid& g = s_near;
    if (!g.c || ci < g.ox + 1 || cj < g.oz + 1 || ci >= g.ox + g.N - 1 || cj >= g.oz + g.N - 1) return false;
    const Cell& c = g.c[gridWrap(cj, g.N) * g.N + gridWrap(ci, g.N)];
    uint8_t mat = c.mat & 15;
    if (mat == MAT_ROAD || mat == MAT_SHOULDER || mat == MAT_WATER || mat == MAT_LOT) return false;
    uint8_t t = c.extra & 15;
    if (t == VEG_NONE || t >= VEG_N) return false;
    uint32_t h = hash32((uint32_t)ci * 2654435761u ^ (uint32_t)cj * 40503u ^ s_seed);
    v.type = t; v.var = (uint8_t)(h >> 24);
    v.x = (ci + 0.2f + 0.6f * ((h & 255) / 255.f)) * g.cell;
    v.z = (cj + 0.2f + 0.6f * (((h >> 8) & 255) / 255.f)) * g.cell;
    float s = ((h >> 16) & 255) / 255.f;
    switch (t) {
        case VEG_PINE:    v.size = 8.f + 9.f * s;  v.radius = 0.35f; break;
        case VEG_BROAD:   v.size = 6.f + 5.f * s;  v.radius = 0.4f; break;
        case VEG_REDWOOD: v.size = 26.f + 16.f * s; v.radius = 1.1f; break;
        case VEG_BUSH:    v.size = 0.8f + 1.0f * s; v.radius = 0.f; break;
        case VEG_CACTUS:  v.size = 2.5f + 2.5f * s; v.radius = 0.3f; break;
        case VEG_SNAG:    v.size = 5.f + 5.f * s;  v.radius = 0.3f; break;
        case VEG_BOULDER: v.size = 0.9f + 2.4f * s; v.radius = v.size * 0.45f; break;
        default:          v.size = 0.4f; v.radius = 0.f; break;
    }
    v.y = c.h * 0.1f;
    return true;
}

// =====================================================================================================
//  Pace notes
// =====================================================================================================
PaceNote roadPaceNote(float carX, float carZ, float carYaw, float speed) {
    PaceNote p; p.valid = false; p.dir = 0; p.sev = 6; p.dist = 0.f; p.crest = false;
    if (s_cnt < 8 || s_carRoadDist > 25.f) return p;
    int dir = s_carDir;
    int start = 6 + (int)(fabsf(speed) * 0.8f / ROAD_DS);           // look further at speed
    for (int m = start; m < start + 50; m++) {
        int k = s_carIdx + dir * m;
        int k2 = k + dir * 4;
        if (k < 0 || k2 < 0 || k >= s_cnt || k2 >= s_cnt) break;
        const RoadNode& a = nodeAt(k);
        const RoadNode& b = nodeAt(k2);
        float turn = wrapPi(b.yaw - a.yaw) * (dir > 0 ? 1.f : -1.f) * 1.f;
        float kap = turn / (4.f * ROAD_DS);
        if (a.flags & RN_CREST) { p.crest = true; }
        if (fabsf(kap) > 1.f / 230.f) {
            float maxK = 0.f; int sgn = kap > 0 ? 1 : -1;
            for (int q = 0; q < 14; q++) {
                int kk = k + dir * q, kk2 = kk + dir * 2;
                if (kk < 0 || kk2 < 0 || kk >= s_cnt || kk2 >= s_cnt) break;
                float t2 = wrapPi(nodeAt(kk2).yaw - nodeAt(kk).yaw) * (dir > 0 ? 1.f : -1.f) / (2.f * ROAD_DS);
                if (t2 * sgn > maxK) maxK = t2 * sgn;
            }
            float R = maxK > 1e-4f ? 1.f / maxK : 999.f;
            int sev = R > 200.f ? 6 : R > 120.f ? 5 : R > 70.f ? 4 : R > 40.f ? 3 : R > 20.f ? 2 : 1;
            p.dir = sgn; p.sev = sev; p.dist = (float)m * ROAD_DS; p.valid = true;
            return p;
        }
        if (p.crest) { p.dist = (float)m * ROAD_DS; p.valid = true; p.dir = 0; return p; }
    }
    (void)carX; (void)carZ; (void)carYaw;
    return p;
}

}
