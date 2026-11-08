// Compact binary codec for PoseFrame (hsm_pose.h): one fighter's full physical state, 60 Hz over
// UDP. Layout: 1-byte version, tsMs/stepUs/flags/grip/pelvis-pos/skeleton-scale header (byte-aligned),
// then a bit-packed body: pelvis world rotation (smallest-three, 15 bit); every other bone as a
// rotation relative to its PARENT'S RECONSTRUCTED pose (smallest-three, 13 bit) so quantisation
// never accumulates down a chain; bone positions by forward kinematics from kPoseRefT (SK_Body_Man
// reference offsets) x the sender's skeleton scale (derived from the frame's own bone positions,
// median of the per-bone offset ratios), with an exact per-bone override wherever the live offset
// differs from the reference by > 0.1 uu; per-body linear (uu/s) and angular (deg/s) velocity,
// 8-bit mu-law; up to two weapons (world pose, velocities, blade base/tip in weapon space, hand,
// 32-bit class hash); then, after all weapons, each flail head (world rotation, position relative to its weapon's
// decoded root, velocities), flagged by bit 2 + slot of the flags byte. The encoder is closed-loop: every child quantity is measured against the
// parent pose the DECODER will rebuild, not the true parent pose, so a bone's error depends only
// on its own quantisation, never on its ancestors'.
//
// Ported from Cyrex0/HalfSword-MP crates/hsmp-pose/src/posecodec_v2.rs (without the control
// layer), Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see
// THIRD_PARTY_NOTICES.md).
#pragma once
#include "sync/hsm_pose.h"
#include "util/hsm_names.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hsm::sync {

// 2: the header carries the grip state (2 bytes after the flags byte). A version-1 frame is refused, not guessed at.
// Flail heads (flags bits 2 and 3, protocol rev 17) stay version 2: their blocks follow every weapon block, so a
// decoder that ignores those bits reads the rest of the frame unchanged.
constexpr uint8_t kPoseCodecVersion = 2;

// ---- quaternion / vector helpers (double precision; safe for NaN/Inf inputs where noted) -----

inline Q4 QuatNormalize(Q4 q) {
    double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!std::isfinite(l) || l <= 1e-6) return Q4{0, 0, 0, 1};
    return Q4{q.x / l, q.y / l, q.z / l, q.w / l};
}
inline Q4 QuatConj(Q4 q) { return Q4{-q.x, -q.y, -q.z, q.w}; }
inline Q4 QuatMul(Q4 a, Q4 b) {
    return Q4{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline V3 QuatRotateVec(Q4 q, V3 v) {
    double tx = 2.0 * (q.y * v.z - q.z * v.y), ty = 2.0 * (q.z * v.x - q.x * v.z), tz = 2.0 * (q.x * v.y - q.y * v.x);
    return V3{v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
              v.z + q.w * tz + (q.x * ty - q.y * tx)};
}
// Angle between two rotations, degrees (test / diagnostics use).
inline double QuatAngleDeg(Q4 a, Q4 b) {
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    double d = std::min(1.0, std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
    return 2.0 * std::acos(d) * kRadToDeg;
}
inline V3 Vec3Add(V3 a, V3 b) { return V3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Vec3Sub(V3 a, V3 b) { return V3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Vec3Scale(V3 a, double k) { return V3{a.x * k, a.y * k, a.z * k}; }
inline double Vec3Dist(V3 a, V3 b) { V3 d = Vec3Sub(a, b); return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z); }

inline double SafeFinite(double x) { return std::isfinite(x) ? x : 0.0; }
inline V3 SafeV3(V3 v) { return V3{SafeFinite(v.x), SafeFinite(v.y), SafeFinite(v.z)}; }
inline Q4 SafeQ4(Q4 q) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w)) return Q4{0, 0, 0, 1};
    return q;
}

// SK_Body_Man reference local translations (GetRefPosePosition), uu. Parents first, as kPoseParent.
inline constexpr std::array<V3, kPoseBones> kPoseRefT = {{
    {0.0, 0.0, 0.0},
    {0.0, -0.23, 3.67}, {0.0, 1.60, 6.60}, {0.0, 1.42, 7.10}, {0.0, 0.21, 8.52}, {0.0, -3.00, 19.41},
    {0.0, -0.61, 11.87}, {0.0, 0.61, 5.02}, {0.0, 0.0, 4.91},
    {1.43, -1.60, 5.44}, {17.81, 0.0, 0.0}, {27.77, -0.01, 0.01}, {27.25, 0.0, 0.0},
    {-1.43, -1.60, 5.44}, {-17.81, 0.0, 0.0}, {-27.77, -0.01, 0.01}, {-27.25, 0.0, 0.0},
    {9.97, 0.26, -2.35}, {2.36, 2.60, -43.20}, {1.76, -2.60, -42.10},
    {-9.97, 0.26, -2.35}, {-2.36, 2.60, -43.20}, {-1.76, -2.60, -42.10},
}};

// ---- wire constants ---------------------------------------------------------------------------

constexpr unsigned kRootQBits = 15;        // pelvis world rotation
constexpr unsigned kChildQBits = 13;       // every other bone / weapon rotation, parent-relative
constexpr unsigned kOvrBits = 16;          // per-bone position override component
constexpr unsigned kWeaponPosBits = 16;    // weapon position component, relative to pelvis
constexpr unsigned kBladeBits = 16;        // weapon-space blade base/tip component
constexpr unsigned kHeadPosBits = 16;      // flail head position component, relative to its weapon's root
constexpr double kOverrideUu = 0.1;        // FK offset vs. reference beyond this: send exactly
constexpr double kOvrStep = 1.0 / 64.0;
constexpr double kWeaponPosStep = 1.0 / 8.0;
constexpr double kBladeStep = 1.0 / 16.0;
constexpr double kHeadPosStep = 1.0 / 16.0;   // +-2048 uu: far beyond any chain
constexpr double kVLinMax = 10000.0;       // uu/s, mu-law full scale
constexpr double kVAngMax = 5000.0;        // deg/s, mu-law full scale
constexpr double kMuLaw = 255.0;

// Grip state, a u16 after the flags byte: bits 0-4 gripR, 5-9 gripL, 10 offhand, 11 altGrip, 12 mordhau, 13 switched2H,
// 14 reverseR, 15 spare (written 0, ignored when read).
constexpr unsigned kGripTypeBits = 5;
constexpr uint8_t kGripTypeMax = (1u << kGripTypeBits) - 1;
constexpr unsigned kGripLShift = kGripTypeBits, kGripOffhandBit = 10, kGripAltBit = 11, kGripMordhauBit = 12,
                   kGripSwitchedBit = 13, kGripReverseBit = 14;

// FNV-1a of the class name with ASCII letters folded to lower case: a name's display casing is whichever casing its
// FName registered first in that process, so two machines can spell one class differently.
constexpr uint32_t WeaponClassHash(const char* s) {
    uint32_t h = 2166136261u;
    if (s) for (const char* p = s; *p; ++p) h = (h ^ static_cast<uint32_t>(static_cast<unsigned char>(FoldAscii(*p)))) * 16777619u;
    return h;
}

// Header: version(1) + tsMs(4) + stepUs(2) + flags(1) + grip(2) + pelvis pos(4*3) + skeleton scale(2) = 24 bytes.
constexpr size_t kPoseHeaderBytes = 1 + 4 + 2 + 1 + 2 + 4 * 3 + 2;

namespace detail {

constexpr unsigned CountBodies() {
    unsigned n = 0;
    for (int i = 0; i < kPoseBones; ++i) if (kPoseHasBody[i]) ++n;
    return n;
}
constexpr unsigned kNumBodies = CountBodies();
constexpr unsigned kNumChildBones = kPoseBones - 1;
constexpr size_t QuatWireBits(unsigned bits) { return 2 + 3 * size_t(bits); }

// ---- bit stream --------------------------------------------------------------------------------

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void Put(uint32_t v, unsigned bits) {
        if (bits == 0) return;
        uint64_t mask = (bits >= 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1);
        acc_ |= (uint64_t(v) & mask) << n_;
        n_ += bits;
        while (n_ >= 8) { out_.push_back(uint8_t(acc_ & 0xFF)); acc_ >>= 8; n_ -= 8; }
    }
    void PutI(int32_t v, unsigned bits) { Put(uint32_t(v), bits); }
    void Finish() {
        if (n_ > 0) out_.push_back(uint8_t(acc_ & 0xFF));
    }

private:
    std::vector<uint8_t>& out_;   // append after the pose header, using its already reserved storage
    uint64_t acc_ = 0;
    unsigned n_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool Get(unsigned bits, uint32_t& out) {
        while (nbits_ < bits) {
            if (pos_ >= n_) return false;
            acc_ |= uint64_t(d_[pos_]) << nbits_;
            ++pos_;
            nbits_ += 8;
        }
        uint64_t mask = (bits >= 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1);
        out = uint32_t(acc_ & mask);
        acc_ >>= bits;
        nbits_ -= bits;
        return true;
    }
    bool GetI(unsigned bits, int32_t& out) {
        uint32_t v;
        if (!Get(bits, v)) return false;
        unsigned sh = 32 - bits;
        out = int32_t(uint32_t(v << sh)) >> sh;
        return true;
    }

private:
    const uint8_t* d_;
    size_t n_;
    size_t pos_ = 0;
    uint64_t acc_ = 0;
    unsigned nbits_ = 0;
};

// ---- scalar quantisation ------------------------------------------------------------------

inline int32_t QuantI(double x, double step, unsigned bits) {
    if (!std::isfinite(x)) return 0;
    int64_t lim = (int64_t(1) << (bits - 1)) - 1;
    double v = std::round(x / step);
    if (!std::isfinite(v)) return 0;
    if (v > double(lim)) v = double(lim);
    if (v < double(-lim)) v = double(-lim);
    return int32_t(v);
}

inline uint32_t MuEncode(double x, double max) {
    if (!std::isfinite(x)) return 0;
    double a = std::min(1.0, std::fabs(x) / max);
    double m = std::round(std::log1p(kMuLaw * a) / std::log1p(kMuLaw) * 127.0);
    uint32_t mi = uint32_t(std::clamp(m, 0.0, 127.0));
    return (x < 0.0 ? 0x80u : 0u) | mi;
}
inline double MuDecode(uint32_t c, double max) {
    // Only 128 magnitudes at each wire scale. Keep the original operation order (including the final multiply)
    // so every decoded double, including signed zero, is identical to the formula. Other scales keep the formula.
    static const auto table = [] {
        std::array<std::array<double, 128>, 2> values{};
        for (size_t i = 0; i < 128; ++i) {
            const double m = double(i) / 127.0;
            const double a = (std::pow(1.0 + kMuLaw, m) - 1.0) / kMuLaw;
            values[0][i] = a * kVLinMax;
            values[1][i] = a * kVAngMax;
        }
        return values;
    }();
    double a;
    if (max == kVLinMax) a = table[0][c & 0x7Fu];
    else if (max == kVAngMax) a = table[1][c & 0x7Fu];
    else {
        const double m = double(c & 0x7Fu) / 127.0;
        a = (std::pow(1.0 + kMuLaw, m) - 1.0) / kMuLaw * max;
    }
    return (c & 0x80u) ? -a : a;
}
inline void PutVelMu(BitWriter& w, V3 v, double max) {
    w.Put(MuEncode(v.x, max), 8);
    w.Put(MuEncode(v.y, max), 8);
    w.Put(MuEncode(v.z, max), 8);
}
inline bool GetVelMu(BitReader& r, double max, V3& out) {
    uint32_t a, b, c;
    if (!r.Get(8, a) || !r.Get(8, b) || !r.Get(8, c)) return false;
    out = V3{MuDecode(a, max), MuDecode(b, max), MuDecode(c, max)};
    return true;
}

// Smallest-three quaternion: 2-bit index of the dropped (largest-magnitude) component + 3 x bits.
// Returns the value the decoder will reconstruct, so the caller can chain against it (closed loop).
inline Q4 PutQuat(BitWriter& w, Q4 qIn, unsigned bits) {
    Q4 q = QuatNormalize(qIn);
    double c[4] = {q.x, q.y, q.z, q.w};
    int li = 0;
    for (int i = 1; i < 4; ++i) if (std::fabs(c[i]) > std::fabs(c[li])) li = i;
    if (c[li] < 0.0) for (double& v : c) v = -v;
    w.Put(uint32_t(li), 2);
    double step = (1.0 / std::sqrt(2.0)) / double((1u << (bits - 1)) - 1);
    double rec[4] = {0, 0, 0, 0};
    double sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (i == li) continue;
        int32_t qv = QuantI(c[i], step, bits);
        w.PutI(qv, bits);
        rec[i] = double(qv) * step;
        sum += rec[i] * rec[i];
    }
    rec[li] = std::sqrt(std::max(0.0, 1.0 - sum));
    return QuatNormalize(Q4{rec[0], rec[1], rec[2], rec[3]});
}
inline bool GetQuat(BitReader& r, unsigned bits, Q4& out) {
    uint32_t liU;
    if (!r.Get(2, liU)) return false;
    int li = int(liU);
    double step = (1.0 / std::sqrt(2.0)) / double((1u << (bits - 1)) - 1);
    double c[4] = {0, 0, 0, 0};
    double sum = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (i == li) continue;
        int32_t v;
        if (!r.GetI(bits, v)) return false;
        c[i] = double(v) * step;
        sum += c[i] * c[i];
    }
    c[li] = std::sqrt(std::max(0.0, 1.0 - sum));
    out = QuatNormalize(Q4{c[0], c[1], c[2], c[3]});
    return true;
}

// Quantised V3, 3 x `bits` signed components of `step` size. Returns the reconstructed value.
inline V3 PutV3(BitWriter& w, V3 v, double step, unsigned bits) {
    int32_t qx = QuantI(v.x, step, bits), qy = QuantI(v.y, step, bits), qz = QuantI(v.z, step, bits);
    w.PutI(qx, bits);
    w.PutI(qy, bits);
    w.PutI(qz, bits);
    return V3{double(qx) * step, double(qy) * step, double(qz) * step};
}
inline bool GetV3(BitReader& r, double step, unsigned bits, V3& out) {
    int32_t a, b, c;
    if (!r.GetI(bits, a) || !r.GetI(bits, b) || !r.GetI(bits, c)) return false;
    out = V3{double(a) * step, double(b) * step, double(c) * step};
    return true;
}
// Quantise without writing (used to decide/measure an override before the mask is known).
inline V3 QuantizeV3Preview(V3 v, double step, unsigned bits) {
    return V3{double(QuantI(v.x, step, bits)) * step, double(QuantI(v.y, step, bits)) * step,
              double(QuantI(v.z, step, bits)) * step};
}

// Sender skeleton scale from the frame's own bone positions: median of dist(bone, parent) /
// |reference offset| over bones whose reference offset is long enough to be a stable ratio.
// Robust to one dislocated or overridden joint. 1.0 if nothing usable is present.
inline double ComputeScale(const PoseFrame& f) {
    std::array<double, kPoseBones> ratios{};
    size_t n = 0;
    for (int i = 1; i < kPoseBones; ++i) {
        const V3& ref = kPoseRefT[i];
        double lr = std::sqrt(ref.x * ref.x + ref.y * ref.y + ref.z * ref.z);
        if (lr < 5.0) continue;
        double d = Vec3Dist(SafeV3(f.bones[i].pos), SafeV3(f.bones[kPoseParent[i]].pos));
        double rr = d / lr;
        if (std::isfinite(rr) && rr > 0.5 && rr < 1.5) ratios[n++] = rr;
    }
    if (n == 0) return 1.0;
    std::sort(ratios.begin(), ratios.begin() + n);
    return ratios[n / 2];
}

inline void AppendU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t((v >> 8) & 0xFF));
}
inline void AppendU32LE(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}
inline void AppendF32LE(std::vector<uint8_t>& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    AppendU32LE(out, bits);
}
inline uint16_t ReadU16LE(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8); }
inline uint32_t ReadU32LE(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline float ReadF32LE(const uint8_t* p) {
    uint32_t bits = ReadU32LE(p);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

// Grip types are saturated, never wrapped: a byte the game should not hold must not turn into a plausible grip type.
constexpr unsigned SatGripType(uint8_t v) { return v > kGripTypeMax ? unsigned(kGripTypeMax) : unsigned(v); }
inline uint16_t PackGrip(const GripState& g) {
    uint16_t w = uint16_t(SatGripType(g.gripR));
    w = uint16_t(w | (SatGripType(g.gripL) << kGripLShift));
    if (g.offhand) w = uint16_t(w | (1u << kGripOffhandBit));
    if (g.altGrip) w = uint16_t(w | (1u << kGripAltBit));
    if (g.mordhau) w = uint16_t(w | (1u << kGripMordhauBit));
    if (g.switched2H) w = uint16_t(w | (1u << kGripSwitchedBit));
    if (g.reverseR) w = uint16_t(w | (1u << kGripReverseBit));
    return w;
}
inline GripState UnpackGrip(uint16_t w) {
    GripState g;
    g.gripR = uint8_t(w & kGripTypeMax);
    g.gripL = uint8_t((w >> kGripLShift) & kGripTypeMax);
    g.offhand = ((w >> kGripOffhandBit) & 1u) != 0;
    g.altGrip = ((w >> kGripAltBit) & 1u) != 0;
    g.mordhau = ((w >> kGripMordhauBit) & 1u) != 0;
    g.switched2H = ((w >> kGripSwitchedBit) & 1u) != 0;
    g.reverseR = ((w >> kGripReverseBit) & 1u) != 0;
    return g;
}

}  // namespace detail

// ---- worst-case byte budget ---------------------------------------------------------------

namespace detail {
constexpr size_t kBitsRootQuat = QuatWireBits(kRootQBits);
constexpr size_t kBitsChildQuats = size_t(kNumChildBones) * QuatWireBits(kChildQBits);
constexpr size_t kBitsVelAll = size_t(kNumBodies) * (3 * 8 + 3 * 8);
constexpr size_t kBitsOverrideMask = kNumChildBones;
constexpr size_t kBitsOverrideValuesMax = size_t(kNumChildBones) * 3 * kOvrBits;
constexpr size_t kBitsPerWeapon =
    1 /* hand */ + 32 /* classHash */ + 3 * kWeaponPosBits /* pos */ + QuatWireBits(kChildQBits) /* rot */ +
    3 * 8 + 3 * 8 /* vel, angVel */ + 3 * kBladeBits /* base */ + 3 * kBladeBits /* tip */;
constexpr size_t kBitsPerHead =
    3 * kHeadPosBits /* pos */ + QuatWireBits(kChildQBits) /* rot */ + 3 * 8 + 3 * 8 /* vel, angVel */;
constexpr size_t kBitsWeaponsMax = size_t(kPoseWeapons) * (kBitsPerWeapon + kBitsPerHead);
constexpr size_t kBitsMax =
    kBitsRootQuat + kBitsChildQuats + kBitsVelAll + kBitsOverrideMask + kBitsOverrideValuesMax + kBitsWeaponsMax;
}  // namespace detail

// Worst case: every non-root bone overridden, both weapon slots present, each with a flail head.
constexpr size_t kPoseFrameMaxBytes = kPoseHeaderBytes + (detail::kBitsMax + 7) / 8;

// ---- encode / decode ------------------------------------------------------------------------

inline void EncodePoseInto(const PoseFrame& f, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(kPoseFrameMaxBytes);
    out.push_back(kPoseCodecVersion);
    detail::AppendU32LE(out, f.tsMs);
    detail::AppendU16LE(out, f.stepUs);

    bool head[kPoseWeapons];
    for (int wi = 0; wi < kPoseWeapons; ++wi) head[wi] = f.weapons[wi].present && f.weapons[wi].hasHead;
    const uint8_t flags = uint8_t((f.weapons[0].present ? 1 : 0) | (f.weapons[1].present ? 2 : 0) | (head[0] ? 4 : 0) |
                                  (head[1] ? 8 : 0));
    out.push_back(flags);
    detail::AppendU16LE(out, detail::PackGrip(f.grip));

    const V3 pelvisPos = SafeV3(f.bones[0].pos);
    detail::AppendF32LE(out, float(pelvisPos.x));
    detail::AppendF32LE(out, float(pelvisPos.y));
    detail::AppendF32LE(out, float(pelvisPos.z));

    const double k = std::clamp(detail::ComputeScale(f), 0.5, 1.5);
    const uint16_t kq = uint16_t(std::clamp(std::round((k - 0.5) * 65535.0), 0.0, 65535.0));
    detail::AppendU16LE(out, kq);
    const double kRec = 0.5 + double(kq) / 65535.0;

    detail::BitWriter w(out);
    std::array<V3, kPoseBones> recPos{};
    std::array<Q4, kPoseBones> recRot{};
    recPos[0] = pelvisPos;
    recRot[0] = detail::PutQuat(w, SafeQ4(f.bones[0].rot), kRootQBits);

    std::array<int, kPoseBones> ovrIdx{};
    std::array<V3, kPoseBones> ovrVal{};
    size_t nOvr = 0;

    for (int i = 1; i < kPoseBones; ++i) {
        const int par = kPoseParent[i];
        const Q4 parRot = recRot[par];
        const V3 parPos = recPos[par];
        const Q4 lq = QuatMul(QuatConj(parRot), QuatNormalize(SafeQ4(f.bones[i].rot)));
        const Q4 lqRec = detail::PutQuat(w, lq, kChildQBits);
        recRot[i] = QuatNormalize(QuatMul(parRot, lqRec));

        const V3 need = QuatRotateVec(QuatConj(parRot), Vec3Sub(SafeV3(f.bones[i].pos), parPos));
        const V3 reft = Vec3Scale(kPoseRefT[i], kRec);
        V3 t;
        if (Vec3Dist(need, reft) > kOverrideUu) {
            t = detail::QuantizeV3Preview(need, kOvrStep, kOvrBits);
            ovrIdx[nOvr] = i;
            ovrVal[nOvr] = t;
            ++nOvr;
        } else {
            t = reft;
        }
        recPos[i] = Vec3Add(parPos, QuatRotateVec(parRot, t));
    }

    for (int i = 0; i < kPoseBones; ++i) {
        if (!kPoseHasBody[i]) continue;
        detail::PutVelMu(w, SafeV3(f.bones[i].vel), kVLinMax);
        detail::PutVelMu(w, SafeV3(f.bones[i].angVelDeg), kVAngMax);
    }

    uint32_t mask = 0;
    for (size_t j = 0; j < nOvr; ++j) mask |= (1u << (ovrIdx[j] - 1));
    w.Put(mask, kPoseBones - 1);
    for (size_t j = 0; j < nOvr; ++j) detail::PutV3(w, ovrVal[j], kOvrStep, kOvrBits);

    std::array<V3, kPoseWeapons> recWeaponPos{};
    for (int wi = 0; wi < kPoseWeapons; ++wi) {
        const WeaponSample& wp = f.weapons[wi];
        if (!wp.present) continue;
        w.Put(uint32_t(wp.hand & 1), 1);
        w.Put(wp.classHash, 32);
        recWeaponPos[wi] = Vec3Add(pelvisPos, detail::PutV3(w, Vec3Sub(SafeV3(wp.pos), pelvisPos), kWeaponPosStep, kWeaponPosBits));
        detail::PutQuat(w, SafeQ4(wp.rot), kChildQBits);
        detail::PutVelMu(w, SafeV3(wp.vel), kVLinMax);
        detail::PutVelMu(w, SafeV3(wp.angVelDeg), kVAngMax);
        detail::PutV3(w, SafeV3(wp.base), kBladeStep, kBladeBits);
        detail::PutV3(w, SafeV3(wp.tip), kBladeStep, kBladeBits);
    }
    // Heads after all weapons: one bit-packed block per flagged slot, slot 0 first.
    for (int wi = 0; wi < kPoseWeapons; ++wi) {
        if (!head[wi]) continue;
        const BoneSample& h = f.weapons[wi].head;
        detail::PutV3(w, Vec3Sub(SafeV3(h.pos), recWeaponPos[wi]), kHeadPosStep, kHeadPosBits);
        detail::PutQuat(w, SafeQ4(h.rot), kChildQBits);
        detail::PutVelMu(w, SafeV3(h.vel), kVLinMax);
        detail::PutVelMu(w, SafeV3(h.angVelDeg), kVAngMax);
    }

    w.Finish();
}

inline std::vector<uint8_t> EncodePose(const PoseFrame& f) {
    std::vector<uint8_t> out;
    EncodePoseInto(f, out);
    return out;
}

inline bool DecodePose(const uint8_t* data, size_t size, PoseFrame& out) {
    if (data == nullptr || size < kPoseHeaderBytes) return false;
    size_t p = 0;
    const uint8_t ver = data[p++];
    if (ver != kPoseCodecVersion) return false;
    const uint32_t tsMs = detail::ReadU32LE(data + p); p += 4;
    const uint16_t stepUs = detail::ReadU16LE(data + p); p += 2;
    const uint8_t flags = data[p++];
    const bool present[kPoseWeapons] = {(flags & 0x01) != 0, (flags & 0x02) != 0};
    const bool head[kPoseWeapons] = {(flags & 0x04) != 0, (flags & 0x08) != 0};
    if ((head[0] && !present[0]) || (head[1] && !present[1])) return false;   // a head without its weapon
    const GripState grip = detail::UnpackGrip(detail::ReadU16LE(data + p)); p += 2;
    const float pelX = detail::ReadF32LE(data + p); p += 4;
    const float pelY = detail::ReadF32LE(data + p); p += 4;
    const float pelZ = detail::ReadF32LE(data + p); p += 4;
    if (!std::isfinite(pelX) || !std::isfinite(pelY) || !std::isfinite(pelZ)) return false;
    const uint16_t kq = detail::ReadU16LE(data + p); p += 2;
    const double k = 0.5 + double(kq) / 65535.0;

    detail::BitReader r(data + p, size - p);
    std::array<V3, kPoseBones> recPos{};
    std::array<Q4, kPoseBones> recRot{};
    const V3 pelvisPos{double(pelX), double(pelY), double(pelZ)};
    recPos[0] = pelvisPos;
    if (!detail::GetQuat(r, kRootQBits, recRot[0])) return false;

    std::array<Q4, kPoseBones> lq{};
    for (int i = 1; i < kPoseBones; ++i) if (!detail::GetQuat(r, kChildQBits, lq[i])) return false;

    std::array<V3, kPoseBones> vel{}, angVel{};
    for (int i = 0; i < kPoseBones; ++i) {
        if (!kPoseHasBody[i]) continue;
        if (!detail::GetVelMu(r, kVLinMax, vel[i])) return false;
        if (!detail::GetVelMu(r, kVAngMax, angVel[i])) return false;
    }

    uint32_t mask;
    if (!r.Get(kPoseBones - 1, mask)) return false;
    std::array<V3, kPoseBones> t{};
    for (int i = 1; i < kPoseBones; ++i) t[i] = Vec3Scale(kPoseRefT[i], k);
    for (int i = 1; i < kPoseBones; ++i) {
        if (mask & (1u << (i - 1))) {
            if (!detail::GetV3(r, kOvrStep, kOvrBits, t[i])) return false;
        }
    }
    for (int i = 1; i < kPoseBones; ++i) {
        const int par = kPoseParent[i];
        recRot[i] = QuatNormalize(QuatMul(recRot[par], lq[i]));
        recPos[i] = Vec3Add(recPos[par], QuatRotateVec(recRot[par], t[i]));
    }

    PoseFrame res{};
    res.tsMs = tsMs;
    res.stepUs = stepUs;
    res.grip = grip;
    for (int i = 0; i < kPoseBones; ++i) {
        res.bones[i].pos = recPos[i];
        res.bones[i].rot = recRot[i];
        res.bones[i].vel = vel[i];
        res.bones[i].angVelDeg = angVel[i];
    }

    for (int wi = 0; wi < kPoseWeapons; ++wi) {
        WeaponSample ws{};
        if (present[wi]) {
            uint32_t handBit, classHash;
            V3 relPos, v, av, base, tip;
            Q4 rot;
            if (!r.Get(1, handBit) || !r.Get(32, classHash) ||
                !detail::GetV3(r, kWeaponPosStep, kWeaponPosBits, relPos) || !detail::GetQuat(r, kChildQBits, rot) ||
                !detail::GetVelMu(r, kVLinMax, v) || !detail::GetVelMu(r, kVAngMax, av) ||
                !detail::GetV3(r, kBladeStep, kBladeBits, base) || !detail::GetV3(r, kBladeStep, kBladeBits, tip)) {
                return false;
            }
            ws.present = true;
            ws.hand = uint8_t(handBit);
            ws.classHash = classHash;
            ws.pos = Vec3Add(pelvisPos, relPos);
            ws.rot = rot;
            ws.vel = v;
            ws.angVelDeg = av;
            ws.base = base;
            ws.tip = tip;
        }
        res.weapons[wi] = ws;
    }
    for (int wi = 0; wi < kPoseWeapons; ++wi) {
        if (!head[wi]) continue;
        BoneSample& h = res.weapons[wi].head;
        V3 relPos;
        if (!detail::GetV3(r, kHeadPosStep, kHeadPosBits, relPos) || !detail::GetQuat(r, kChildQBits, h.rot) ||
            !detail::GetVelMu(r, kVLinMax, h.vel) || !detail::GetVelMu(r, kVAngMax, h.angVelDeg)) {
            return false;
        }
        h.pos = Vec3Add(res.weapons[wi].pos, relPos);
        res.weapons[wi].hasHead = true;
    }

    out = res;
    return true;
}

}  // namespace hsm::sync
