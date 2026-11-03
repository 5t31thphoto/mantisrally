// Unpacks the art into PSRAM at boot. The decoder is a small, self-contained inflate (RFC 1950/1951: stored,
// fixed and dynamic Huffman blocks), so the build doesn't depend on which ROM helpers an ESP-IDF version exposes.
#include "art.h"
#include "art_packed.h"
#include "mem.h"
#include <string.h>

namespace mantis {
namespace art {

VehArt  VEH_ART[4];
CockArt COCK_ART[4];
SideArt SIDE_ART[4];
SideArt REAR_ART[4];
const uint8_t* MM_SIGN = nullptr;
const uint8_t* MM_SIGN_L = nullptr;
uint8_t MM_SIGN_W = 0, MM_SIGN_H = 0, MM_SIGN_AX = 0, MM_SIGN_AXL = 0;
static uint8_t* s_data = nullptr;

// ---------------------------------------------------------------- inflate
namespace {
struct In {
    const uint8_t* p; const uint8_t* end;
    uint32_t bitbuf; int bitcnt;
    uint8_t* out; uint32_t outN, outPos;
    bool bad;
};
inline int bits(In& s, int need) {
    uint32_t v = s.bitbuf;
    while (s.bitcnt < need) {
        if (s.p >= s.end) { s.bad = true; return 0; }
        v |= (uint32_t)(*s.p++) << s.bitcnt;
        s.bitcnt += 8;
    }
    s.bitbuf = v >> need;
    s.bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}
struct Huff { int16_t count[16]; int16_t symbol[320]; };
int decode(In& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    s.bad = true;
    return -1;
}
void build(Huff& h, const uint8_t* length, int n) {
    int16_t offs[16];
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; i++) h.count[length[i]]++;
    h.count[0] = 0;
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (int16_t)(offs[len] + h.count[len]);
    for (int i = 0; i < n; i++) if (length[i]) h.symbol[offs[length[i]]++] = (int16_t)i;
}
const int16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int8_t  LEXT[29]  = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int8_t  DEXT[30]  = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(In& s, const Huff& lit, const Huff& dist) {
    for (;;) {
        int sym = decode(s, lit);
        if (s.bad || sym < 0) return false;
        if (sym < 256) { if (s.outPos >= s.outN) return false; s.out[s.outPos++] = (uint8_t)sym; continue; }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = LBASE[sym] + bits(s, LEXT[sym]);
        int ds = decode(s, dist);
        if (s.bad || ds < 0 || ds >= 30) return false;
        uint32_t d = (uint32_t)(DBASE[ds] + bits(s, DEXT[ds]));
        if (d > s.outPos || s.outPos + (uint32_t)len > s.outN) return false;
        for (int k = 0; k < len; k++, s.outPos++) s.out[s.outPos] = s.out[s.outPos - d];
    }
}
bool stored(In& s) {
    s.bitbuf = 0; s.bitcnt = 0;                          // to the byte boundary
    if (s.end - s.p < 4) return false;
    uint32_t len = (uint32_t)s.p[0] | ((uint32_t)s.p[1] << 8);
    uint32_t nlen = (uint32_t)s.p[2] | ((uint32_t)s.p[3] << 8);
    s.p += 4;
    if (len != (~nlen & 0xFFFFu) || (uint32_t)(s.end - s.p) < len || s.outPos + len > s.outN) return false;
    memcpy(s.out + s.outPos, s.p, len); s.p += len; s.outPos += len;
    return true;
}
bool fixedBlock(In& s) {
    static Huff lit, dist; static bool made = false;
    if (!made) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        build(dist, l, 30);
        made = true;
    }
    return codes(s, lit, dist);
}
bool dynamicBlock(In& s) {
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return false;
    uint8_t lengths[320];
    memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i < ncode; i++) lengths[ORDER[i]] = (uint8_t)bits(s, 3);
    static Huff lencode, lit, dist;
    build(lencode, lengths, 19);
    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(s, lencode);
        if (s.bad || sym < 0) return false;
        if (sym < 16) { lengths[idx++] = (uint8_t)sym; continue; }
        int len = 0, rep;
        if (sym == 16) { if (idx == 0) return false; len = lengths[idx - 1]; rep = 3 + bits(s, 2); }
        else if (sym == 17) rep = 3 + bits(s, 3);
        else rep = 11 + bits(s, 7);
        if (idx + rep > nlen + ndist) return false;
        while (rep--) lengths[idx++] = (uint8_t)len;
    }
    build(lit, lengths, nlen);
    build(dist, lengths + nlen, ndist);
    return codes(s, lit, dist);
}
}  // namespace

bool artInflate(const uint8_t* z, uint32_t zn, uint8_t* out, uint32_t outN) {
    if (zn < 2 || (z[0] & 0x0F) != 8 || ((z[0] << 8) | z[1]) % 31 != 0) return false;   // a zlib header: deflate
    In s; s.p = z + 2; s.end = z + zn; s.bitbuf = 0; s.bitcnt = 0; s.out = out; s.outN = outN; s.outPos = 0; s.bad = false;
    int last;
    do {
        last = bits(s, 1);
        int type = bits(s, 2);
        bool ok = type == 0 ? stored(s) : type == 1 ? fixedBlock(s) : type == 2 ? dynamicBlock(s) : false;
        if (!ok || s.bad) return false;
    } while (!last);
    return s.outPos == outN;
}

// ---------------------------------------------------------------- the art, unpacked once
bool artInit() {
    if (s_data) return true;
    uint8_t* d = (uint8_t*)memSlow(artpack::RAW_SIZE);
    if (!d) return false;
    if (!artInflate(artpack::Z, artpack::Z_SIZE, d, artpack::RAW_SIZE)) return false;
    s_data = d;
    for (int i = 0; i < 4; i++) {
        const artpack::VehMeta& v = artpack::VEH[i];
        VEH_ART[i] = {v.w, v.h, v.frames, v.ax, v.ay, d + v.pal, d + v.px};
        const artpack::CockMeta& c = artpack::COCK[i];
        COCK_ART[i] = {d + c.pal, d + c.px, c.wx, c.wy, c.wr};
        SIDE_ART[i] = {artpack::SIDE[i].w, artpack::SIDE[i].h, (const uint16_t*)(d + artpack::SIDE[i].px)};
        REAR_ART[i] = {artpack::REAR[i].w, artpack::REAR[i].h, (const uint16_t*)(d + artpack::REAR[i].px)};
    }
    MM_SIGN = d + artpack::MM_SIGN_OFF; MM_SIGN_L = d + artpack::MM_SIGN_L_OFF;
    MM_SIGN_W = artpack::MM_W; MM_SIGN_H = artpack::MM_H; MM_SIGN_AX = artpack::MM_AX; MM_SIGN_AXL = artpack::MM_AXL;
    return true;
}

}
}
