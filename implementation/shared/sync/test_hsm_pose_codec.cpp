// Unit tests for hsm_pose_codec.h. Run via implementation/shared/run_tests.sh test_hsm_pose_codec.
#include "sync/hsm_pose_codec.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

// ---- test fixtures: plausible poses built by forward kinematics from kPoseRefT, exactly like
// the real sender would produce, so the only error source under test is the codec itself. ------

static Q4 AxisAngleQuat(V3 axis, double deg) {
    double l = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (l < 1e-9) l = 1e-9;
    double h = deg * (3.14159265358979323846 / 180.0) * 0.5;
    double s = std::sin(h), c = std::cos(h);
    return Q4{axis.x / l * s, axis.y / l * s, axis.z / l * s, c};
}

// A realistic full-body pose: random pelvis placement/orientation, random limb joint rotations
// (<= limbDeg, applied about a random axis), bone positions derived by FK from kPoseRefT * k (so
// a correct codec needs zero overrides), random per-body velocities within the mu-law range, and
// 0..maxWeapons weapons placed near the hands.
static PoseFrame BuildFkFrame(std::mt19937& rng, double k, int numWeapons, double limbDeg = 120.0) {
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    PoseFrame f{};
    f.tsMs = uint32_t(std::uniform_int_distribution<uint32_t>(0, 0xFFFFFFFFu)(rng));
    f.stepUs = uint16_t(std::uniform_int_distribution<int>(0, 33333)(rng));

    std::array<V3, kPoseBones> pos{};
    std::array<Q4, kPoseBones> rot{};
    pos[0] = V3{u(rng) * 50000.0, u(rng) * 50000.0, u(rng) * 50000.0};
    rot[0] = AxisAngleQuat(V3{u(rng), u(rng), u(rng)}, u(rng) * 180.0);
    for (int i = 1; i < kPoseBones; ++i) {
        int par = kPoseParent[i];
        Q4 localQ = AxisAngleQuat(V3{u(rng), u(rng), u(rng) + 0.01}, u(rng) * limbDeg);
        rot[i] = QuatNormalize(QuatMul(rot[par], localQ));
        pos[i] = Vec3Add(pos[par], QuatRotateVec(rot[par], Vec3Scale(kPoseRefT[i], k)));
    }
    for (int i = 0; i < kPoseBones; ++i) {
        f.bones[i].pos = pos[i];
        f.bones[i].rot = rot[i];
        if (kPoseHasBody[i]) {
            f.bones[i].vel = V3{u(rng) * kVLinMax, u(rng) * kVLinMax, u(rng) * kVLinMax};
            f.bones[i].angVelDeg = V3{u(rng) * kVAngMax, u(rng) * kVAngMax, u(rng) * kVAngMax};
        }
    }
    const int handBone[2] = {kBoneHandR, kBoneHandL};
    for (int wi = 0; wi < numWeapons && wi < kPoseWeapons; ++wi) {
        WeaponSample ws{};
        ws.present = true;
        ws.hand = uint8_t(wi);
        ws.classHash = WeaponClassHash(wi == 0 ? "BP_Longsword_Tier3_C" : "BP_Dagger_Tier1_C");
        ws.pos = Vec3Add(f.bones[handBone[wi]].pos, V3{u(rng) * 3.0, u(rng) * 3.0, u(rng) * 3.0});
        ws.rot = AxisAngleQuat(V3{u(rng), u(rng), u(rng) + 0.01}, u(rng) * 180.0);
        ws.vel = V3{u(rng) * kVLinMax, u(rng) * kVLinMax, u(rng) * kVLinMax};
        ws.angVelDeg = V3{u(rng) * kVAngMax, u(rng) * kVAngMax, u(rng) * kVAngMax};
        ws.base = V3{0.0, 0.0, 18.0};
        ws.tip = V3{0.0, 0.0, 105.5};
        f.weapons[wi] = ws;
    }
    return f;
}

// A flail head on weapon slot wi: somewhere within a chain's reach of the root (up to ~1.5 m, any direction), any
// rotation, velocities across the mu-law range.
static void AddHead(std::mt19937& rng, PoseFrame& f, int wi) {
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    WeaponSample& w = f.weapons[wi];
    w.hasHead = true;
    w.head.pos = Vec3Add(w.pos, V3{u(rng) * 150.0, u(rng) * 150.0, u(rng) * 150.0});
    w.head.rot = AxisAngleQuat(V3{u(rng), u(rng), u(rng) + 0.01}, u(rng) * 180.0);
    w.head.vel = V3{u(rng) * kVLinMax, u(rng) * kVLinMax, u(rng) * kVLinMax};
    w.head.angVelDeg = V3{u(rng) * kVAngMax, u(rng) * kVAngMax, u(rng) * kVAngMax};
}

static bool FiniteV3(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
static bool FiniteQ4(const Q4& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }
static bool AllFinite(const PoseFrame& f) {
    for (int i = 0; i < kPoseBones; ++i) {
        if (!FiniteV3(f.bones[i].pos) || !FiniteQ4(f.bones[i].rot) || !FiniteV3(f.bones[i].vel) || !FiniteV3(f.bones[i].angVelDeg)) return false;
    }
    for (int i = 0; i < kPoseWeapons; ++i) {
        const WeaponSample& w = f.weapons[i];
        if (!FiniteV3(w.pos) || !FiniteQ4(w.rot) || !FiniteV3(w.vel) || !FiniteV3(w.angVelDeg) || !FiniteV3(w.base) || !FiniteV3(w.tip)) return false;
        if (!FiniteV3(w.head.pos) || !FiniteQ4(w.head.rot) || !FiniteV3(w.head.vel) || !FiniteV3(w.head.angVelDeg)) return false;
    }
    return true;
}

// Bound justification (see top-of-file comment in hsm_pose_codec.h for the closed-loop argument):
// position and rotation error at a bone depend only on that bone's own quantisation, not on how
// deep it is in the chain, because both the local rotation and the FK-vs-override decision are
// measured against the PARENT'S RECONSTRUCTED pose (the exact value the decoder will also
// compute), not the true parent pose. HSMP's own f32 round-trip test (posecodec_v2.rs,
// roundtrip_is_exact_within_spec) asserts position error < 0.3 uu and rotation error < 0.1 deg
// for the same 13-bit smallest-three / 0.1 uu override threshold; we use the same 0.3 uu position
// bound (documented by HSMP) and a rotation bound of 0.15 deg -- looser than HSMP's observed 0.1
// deg to leave headroom for running 2000 seeds (40x HSMP's 50) and double- vs f32-precision
// differences in the reference math, but tighter than the 0.25 deg ceiling the port allows for.
static constexpr double kMaxPosErrUu = 0.3;
static constexpr double kMaxRotErrDeg = 0.15;
// Mu-law relative error bound (HSMP's own test, same MU/VLIN_MAX/VANG_MAX constants): a relative
// error with a small additive floor near zero, since mu-law's finest step is always > 0.
static bool VelOk(double decoded, double truth, double floorAbs) { return std::fabs(decoded - truth) <= 0.03 * std::fabs(truth) + floorAbs; }

static void TestPropertyRoundTrip() {
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<double> ku(0.85, 1.2);
    std::uniform_int_distribution<int> wn(0, 2);
    const int N = 2000;
    double maxPos = 0, maxRot = 0, maxLin = 0, maxAng = 0, maxWeaponPos = 0, maxBlade = 0, maxHeadPos = 0, maxHeadRot = 0;
    std::array<double, kPoseBones> maxPosByBone{}, maxRotByBone{};
    size_t maxSize = 0;

    for (int iter = 0; iter < N; ++iter) {
        double k = ku(rng);
        int nw = wn(rng);
        PoseFrame f = BuildFkFrame(rng, k, nw);
        for (int wi = 0; wi < nw; ++wi) if (rng() & 1) AddHead(rng, f, wi);
        std::vector<uint8_t> buf = EncodePose(f);
        maxSize = std::max(maxSize, buf.size());
        CHECK(buf.size() <= kPoseFrameMaxBytes);

        PoseFrame d{};
        CHECK(DecodePose(buf.data(), buf.size(), d));
        CHECK(AllFinite(d));
        CHECK(d.tsMs == f.tsMs);
        CHECK(d.stepUs == f.stepUs);

        for (int i = 0; i < kPoseBones; ++i) {
            double pe = Vec3Dist(d.bones[i].pos, f.bones[i].pos);
            double re = QuatAngleDeg(d.bones[i].rot, f.bones[i].rot);
            maxPos = std::max(maxPos, pe);
            maxRot = std::max(maxRot, re);
            maxPosByBone[i] = std::max(maxPosByBone[i], pe);
            maxRotByBone[i] = std::max(maxRotByBone[i], re);
            CHECK(pe <= kMaxPosErrUu);
            CHECK(re <= kMaxRotErrDeg);
            if (kPoseHasBody[i]) {
                double lx = std::fabs(d.bones[i].vel.x - f.bones[i].vel.x), ly = std::fabs(d.bones[i].vel.y - f.bones[i].vel.y), lz = std::fabs(d.bones[i].vel.z - f.bones[i].vel.z);
                CHECK(VelOk(d.bones[i].vel.x, f.bones[i].vel.x, 2.0));
                CHECK(VelOk(d.bones[i].vel.y, f.bones[i].vel.y, 2.0));
                CHECK(VelOk(d.bones[i].vel.z, f.bones[i].vel.z, 2.0));
                CHECK(VelOk(d.bones[i].angVelDeg.x, f.bones[i].angVelDeg.x, 1.0));
                CHECK(VelOk(d.bones[i].angVelDeg.y, f.bones[i].angVelDeg.y, 1.0));
                CHECK(VelOk(d.bones[i].angVelDeg.z, f.bones[i].angVelDeg.z, 1.0));
                maxLin = std::max({maxLin, lx, ly, lz});
                double ax = std::fabs(d.bones[i].angVelDeg.x - f.bones[i].angVelDeg.x), ay = std::fabs(d.bones[i].angVelDeg.y - f.bones[i].angVelDeg.y), az = std::fabs(d.bones[i].angVelDeg.z - f.bones[i].angVelDeg.z);
                maxAng = std::max({maxAng, ax, ay, az});
            }
        }
        for (int wi = 0; wi < kPoseWeapons; ++wi) {
            if (!f.weapons[wi].present) { CHECK(!d.weapons[wi].present); continue; }
            CHECK(d.weapons[wi].present);
            CHECK(d.weapons[wi].hand == f.weapons[wi].hand);
            CHECK(d.weapons[wi].classHash == f.weapons[wi].classHash);
            double wpe = Vec3Dist(d.weapons[wi].pos, f.weapons[wi].pos);
            double wre = QuatAngleDeg(d.weapons[wi].rot, f.weapons[wi].rot);
            maxWeaponPos = std::max(maxWeaponPos, wpe);
            CHECK(wpe <= kMaxPosErrUu);
            CHECK(wre <= kMaxRotErrDeg);
            CHECK(VelOk(d.weapons[wi].vel.x, f.weapons[wi].vel.x, 2.0) && VelOk(d.weapons[wi].vel.y, f.weapons[wi].vel.y, 2.0) && VelOk(d.weapons[wi].vel.z, f.weapons[wi].vel.z, 2.0));
            CHECK(VelOk(d.weapons[wi].angVelDeg.x, f.weapons[wi].angVelDeg.x, 1.0) && VelOk(d.weapons[wi].angVelDeg.y, f.weapons[wi].angVelDeg.y, 1.0) && VelOk(d.weapons[wi].angVelDeg.z, f.weapons[wi].angVelDeg.z, 1.0));
            // Blade endpoints in world space (weapon pose composed with weapon-space base/tip).
            V3 tipTrueWorld = Vec3Add(f.weapons[wi].pos, QuatRotateVec(f.weapons[wi].rot, f.weapons[wi].tip));
            V3 tipDecWorld = Vec3Add(d.weapons[wi].pos, QuatRotateVec(d.weapons[wi].rot, d.weapons[wi].tip));
            double te = Vec3Dist(tipTrueWorld, tipDecWorld);
            maxBlade = std::max(maxBlade, te);
            CHECK(te <= kMaxPosErrUu);
            CHECK(d.weapons[wi].hasHead == f.weapons[wi].hasHead);
            if (!f.weapons[wi].hasHead) continue;
            const BoneSample& fh = f.weapons[wi].head;
            const BoneSample& dh = d.weapons[wi].head;
            const double hpe = Vec3Dist(dh.pos, fh.pos), hre = QuatAngleDeg(dh.rot, fh.rot);
            maxHeadPos = std::max(maxHeadPos, hpe);
            maxHeadRot = std::max(maxHeadRot, hre);
            CHECK(hpe <= kMaxPosErrUu);
            CHECK(hre <= kMaxRotErrDeg);
            CHECK(VelOk(dh.vel.x, fh.vel.x, 2.0) && VelOk(dh.vel.y, fh.vel.y, 2.0) && VelOk(dh.vel.z, fh.vel.z, 2.0));
            CHECK(VelOk(dh.angVelDeg.x, fh.angVelDeg.x, 1.0) && VelOk(dh.angVelDeg.y, fh.angVelDeg.y, 1.0) && VelOk(dh.angVelDeg.z, fh.angVelDeg.z, 1.0));
        }
    }

    std::printf("property test (%d frames): max pos err %.4f uu, max rot err %.4f deg, max lin vel err %.4f uu/s, "
                "max ang vel err %.4f deg/s, max weapon pos err %.4f uu, max blade-tip world err %.4f uu, max size %zu B\n",
                N, maxPos, maxRot, maxLin, maxAng, maxWeaponPos, maxBlade, maxSize);
    std::printf("flail heads: max pos err %.4f uu, max rot err %.4f deg\n", maxHeadPos, maxHeadRot);

    // Error does not accumulate down a chain: clavicle_r (depth 6 from pelvis) vs. hand_r (3
    // joints further out, depth 9) should show the same order of magnitude of error, not ~4x.
    constexpr int kBoneClavicleR = 13;  // kPoseBoneNames[13] == "clavicle_r"; hand_r (16) is 3 joints further out
    std::printf("chain depth check: clavicle_r max pos/rot err %.4f uu / %.4f deg, hand_r (3 joints deeper) %.4f uu / %.4f deg\n",
                maxPosByBone[kBoneClavicleR], maxRotByBone[kBoneClavicleR], maxPosByBone[kBoneHandR], maxRotByBone[kBoneHandR]);
    CHECK(maxPosByBone[kBoneHandR] <= maxPosByBone[kBoneClavicleR] * 3.0 + 0.05);
    CHECK(maxRotByBone[kBoneHandR] <= maxRotByBone[kBoneClavicleR] * 3.0 + 0.05);
    CHECK(maxPosByBone[kBoneFootR] <= maxPosByBone[20 /* thigh_r */] * 3.0 + 0.05);
}

static void TestDislocatedBoneOverridesExactly() {
    std::mt19937 rng(42);
    PoseFrame f = BuildFkFrame(rng, 1.0, 0);
    // Pull the left hand 5 uu out of its socket; everything above it in the chain is unaffected FK.
    V3 dir = Vec3Scale(V3{0.6, 0.6, 0.5296}, 5.0 / Vec3Dist(V3{0.6, 0.6, 0.5296}, V3{}));
    PoseFrame g = f;
    g.bones[kBoneHandL].pos = Vec3Add(f.bones[kBoneHandL].pos, dir);

    std::vector<uint8_t> buf = EncodePose(g);
    PoseFrame d{};
    CHECK(DecodePose(buf.data(), buf.size(), d));
    double handErr = Vec3Dist(d.bones[kBoneHandL].pos, g.bones[kBoneHandL].pos);
    std::printf("dislocated-joint override: hand_l round-trip error %.5f uu (override quant step 1/64 uu)\n", handErr);
    CHECK(handErr <= 0.05);                                             // 16-bit / (1/64 uu) quantisation only
    CHECK(Vec3Dist(d.bones[11 /* lowerarm_l */].pos, g.bones[11].pos) <= kMaxPosErrUu);  // parent still plain FK
}

static void TestEncodedSize() {
    std::mt19937 rng(9);
    std::vector<uint8_t> b0 = EncodePose(BuildFkFrame(rng, 1.0, 0));
    std::vector<uint8_t> b1 = EncodePose(BuildFkFrame(rng, 1.0, 1));
    std::vector<uint8_t> b2 = EncodePose(BuildFkFrame(rng, 1.0, 2));
    std::printf("encoded size: 0 weapons %zu B, 1 weapon %zu B, 2 weapons %zu B (kPoseFrameMaxBytes = %zu B)\n",
                b0.size(), b1.size(), b2.size(), kPoseFrameMaxBytes);
    CHECK(b2.size() <= 400);
    CHECK(b0.size() <= kPoseFrameMaxBytes);
    CHECK(b1.size() <= kPoseFrameMaxBytes);
    CHECK(b2.size() <= kPoseFrameMaxBytes);
}

static void TestNegatedQuatEncodesIdentically() {
    PoseFrame f{};
    f.bones[0].pos = V3{12.0, -34.0, 56.0};
    f.bones[0].rot = QuatNormalize(Q4{0.1, 0.2, 0.3, 0.9});
    PoseFrame g = f;
    g.bones[0].rot = Q4{-f.bones[0].rot.x, -f.bones[0].rot.y, -f.bones[0].rot.z, -f.bones[0].rot.w};
    CHECK(EncodePose(f) == EncodePose(g));

    // Same property for a child bone's rotation.
    PoseFrame h = f;
    h.bones[2].rot = QuatNormalize(Q4{0.2, -0.1, 0.4, 0.88});
    PoseFrame h2 = h;
    h2.bones[2].rot = Q4{-h.bones[2].rot.x, -h.bones[2].rot.y, -h.bones[2].rot.z, -h.bones[2].rot.w};
    CHECK(EncodePose(h) == EncodePose(h2));
}

static void TestIdentityAnd180DegreeRotations() {
    PoseFrame f{};
    f.bones[0].rot = Q4{0, 0, 0, 1};
    std::vector<uint8_t> buf = EncodePose(f);
    PoseFrame d{};
    CHECK(DecodePose(buf.data(), buf.size(), d));
    CHECK(QuatAngleDeg(d.bones[0].rot, f.bones[0].rot) <= kMaxRotErrDeg);

    PoseFrame f180{};
    f180.bones[0].rot = AxisAngleQuat(V3{0, 0, 1}, 180.0);  // w == 0 exactly: smallest-three boundary
    std::vector<uint8_t> buf2 = EncodePose(f180);
    PoseFrame d2{};
    CHECK(DecodePose(buf2.data(), buf2.size(), d2));
    CHECK(QuatAngleDeg(d2.bones[0].rot, f180.bones[0].rot) <= kMaxRotErrDeg);

    // A child bone at exactly 180 degrees from its parent.
    PoseFrame f3{};
    f3.bones[2].rot = AxisAngleQuat(V3{1, 0, 0}, 180.0);
    std::vector<uint8_t> buf3 = EncodePose(f3);
    PoseFrame d3{};
    CHECK(DecodePose(buf3.data(), buf3.size(), d3));
    CHECK(QuatAngleDeg(d3.bones[2].rot, f3.bones[2].rot) <= kMaxRotErrDeg);
}

static void TestUnknownVersionRejected() {
    std::mt19937 rng(3);
    std::vector<uint8_t> buf = EncodePose(BuildFkFrame(rng, 1.0, 1));
    buf[0] = kPoseCodecVersion + 1;
    PoseFrame d{};
    CHECK(!DecodePose(buf.data(), buf.size(), d));
    buf[0] = 0;
    CHECK(!DecodePose(buf.data(), buf.size(), d));
}

static void TestNaNHeaderRejected() {
    std::mt19937 rng(4);
    std::vector<uint8_t> buf = EncodePose(BuildFkFrame(rng, 1.0, 0));
    float nanv = std::numeric_limits<float>::quiet_NaN();
    uint8_t nanBytes[4];
    std::memcpy(nanBytes, &nanv, 4);
    std::vector<uint8_t> bad = buf;
    for (int i = 0; i < 4; ++i) bad[10 + i] = nanBytes[i];  // pelvis.x, right after the 10-byte header prefix (v2: version, tsMs, stepUs, flags, grip)
    PoseFrame d{};
    CHECK(!DecodePose(bad.data(), bad.size(), d));

    std::vector<uint8_t> inf = buf;
    float infv = std::numeric_limits<float>::infinity();
    uint8_t infBytes[4];
    std::memcpy(infBytes, &infv, 4);
    for (int i = 0; i < 4; ++i) inf[14 + i] = infBytes[i];  // pelvis.y
    CHECK(!DecodePose(inf.data(), inf.size(), d));
}

static void TestTruncationAlwaysRejected() {
    std::mt19937 rng(5);
    std::vector<uint8_t> buf = EncodePose(BuildFkFrame(rng, 1.0, 2));
    PoseFrame d{};
    CHECK(DecodePose(buf.data(), buf.size(), d));  // sanity: the full buffer still decodes
    for (size_t cut = 0; cut < buf.size(); ++cut) CHECK(!DecodePose(buf.data(), cut, d));
    PoseFrame h = BuildFkFrame(rng, 1.0, 2);
    AddHead(rng, h, 0);
    AddHead(rng, h, 1);
    buf = EncodePose(h);
    CHECK(DecodePose(buf.data(), buf.size(), d));
    for (size_t cut = 0; cut < buf.size(); ++cut) CHECK(!DecodePose(buf.data(), cut, d));   // a cut inside a head block too
    CHECK(!DecodePose(nullptr, 0, d));
    CHECK(!DecodePose(buf.data(), 0, d));
}

static void TestHostileRandomBuffersNeverCrashAndStayFinite() {
    std::mt19937 rng(0x5EED5EEDu);
    std::uniform_int_distribution<int> sizeDist(0, 600);
    std::uniform_int_distribution<int> byteDist(0, 255);
    const int N = 20000;
    int ok = 0;
    std::vector<uint8_t> buf;
    for (int iter = 0; iter < N; ++iter) {
        buf.resize(size_t(sizeDist(rng)));
        for (auto& b : buf) b = uint8_t(byteDist(rng));
        PoseFrame d{};
        bool decoded = DecodePose(buf.empty() ? nullptr : buf.data(), buf.size(), d);
        if (decoded) { ++ok; CHECK(AllFinite(d)); }
    }
    std::printf("hostile fuzz: %d/%d random buffers decoded successfully (rest rejected cleanly)\n", ok, N);
}

static void TestWeaponClassHash() {
    CHECK(WeaponClassHash("") == 2166136261u);
    CHECK(WeaponClassHash(nullptr) == 2166136261u);
    CHECK(WeaponClassHash("BP_Longsword_Tier3_C") != WeaponClassHash("BP_Dagger_Tier1_C"));
    CHECK(WeaponClassHash("a") == WeaponClassHash("a"));
    // A class name's display casing is whichever casing its FName registered first in that process, so two machines can
    // spell the same class differently: the hash ignores ASCII case, and nothing else.
    CHECK(WeaponClassHash("ModularWeaponBP_C") == WeaponClassHash("modularweaponbp_c"));
    CHECK(WeaponClassHash("BP_Longsword_Tier3_C") == WeaponClassHash("BP_LONGSWORD_TIER3_C"));
    CHECK(WeaponClassHash("ModularWeaponBP_C") != WeaponClassHash("ModularWeaponBP_D"));
    CHECK(WeaponClassHash("[") != WeaponClassHash("{"));        // only letters fold
    CHECK(WeaponClassHash("\xC4") != WeaponClassHash("\xE4"));  // and only ASCII ones
}

// ---- codec v2: the grip state (hands' grip types and the five grip flags) in the header ----------------------------
// Wire layout of the 2 bytes after the flags byte (header offsets 8 and 9, little-endian u16): bits 0-4 gripR, 5-9 gripL,
// 10 offhand, 11 altGrip, 12 mordhau, 13 switched2H, 14 reverseR, 15 spare (written 0, ignored when read).
static constexpr size_t kGripOff = 8;
static constexpr size_t kV1HeaderBytes = 1 + 4 + 2 + 1 + 4 * 3 + 2;   // the documented v1 header: 22 bytes

static uint16_t GripWord(const std::vector<uint8_t>& buf) { return uint16_t(buf[kGripOff] | (buf[kGripOff + 1] << 8)); }

static bool SameGrip(const GripState& a, const GripState& b) {
    return a.gripR == b.gripR && a.gripL == b.gripL && a.offhand == b.offhand && a.altGrip == b.altGrip &&
           a.mordhau == b.mordhau && a.switched2H == b.switched2H && a.reverseR == b.reverseR;
}

static GripState MakeGrip(unsigned r, unsigned l, bool off, bool alt, bool mord, bool sw, bool rev) {
    GripState g;
    g.gripR = uint8_t(r); g.gripL = uint8_t(l);
    g.offhand = off; g.altGrip = alt; g.mordhau = mord; g.switched2H = sw; g.reverseR = rev;
    return g;
}

static bool GripRoundTrips(const GripState& g) {
    std::mt19937 rng(77);
    PoseFrame f = BuildFkFrame(rng, 1.0, 1);
    f.grip = g;
    std::vector<uint8_t> buf = EncodePose(f);
    PoseFrame d{};
    return DecodePose(buf.data(), buf.size(), d) && SameGrip(d.grip, g);
}

static void TestGripRoundTripExtremes() {
    CHECK(kPoseCodecVersion == 2);
    // Both grip types at both ends of the 5-bit range, independently and together, plus the bit-4 / bit-0 neighbours.
    const unsigned vals[] = {0, 1, 15, 16, 30, 31};
    for (unsigned r : vals) for (unsigned l : vals) CHECK(GripRoundTrips(MakeGrip(r, l, false, false, false, false, false)));
    CHECK(GripRoundTrips(MakeGrip(31, 0, false, false, false, false, false)));   // R alone: swapped R/L shows here
    CHECK(GripRoundTrips(MakeGrip(0, 31, false, false, false, false, false)));
    // Each flag alone, then all five, then none: a dropped, merged or shifted flag shows here.
    CHECK(GripRoundTrips(MakeGrip(0, 0, true,  false, false, false, false)));
    CHECK(GripRoundTrips(MakeGrip(0, 0, false, true,  false, false, false)));
    CHECK(GripRoundTrips(MakeGrip(0, 0, false, false, true,  false, false)));
    CHECK(GripRoundTrips(MakeGrip(0, 0, false, false, false, true,  false)));
    CHECK(GripRoundTrips(MakeGrip(0, 0, false, false, false, false, true)));
    CHECK(GripRoundTrips(MakeGrip(31, 31, true, true, true, true, true)));
    CHECK(GripRoundTrips(MakeGrip(0, 0, false, false, false, false, false)));
    // Flags with grips in between do not bleed into each other.
    CHECK(GripRoundTrips(MakeGrip(31, 0, true, false, true, false, true)));
    CHECK(GripRoundTrips(MakeGrip(0, 31, false, true, false, true, false)));
    // Every grip value 0..31 on each hand, across 2000 random frames' worth of bone data: the header is independent of the body.
    std::mt19937 rng(0x6121Bu);
    for (int i = 0; i < 2000; ++i) {
        PoseFrame f = BuildFkFrame(rng, 1.0, int(rng() % 3));
        f.grip = MakeGrip(rng() % 32, rng() % 32, rng() & 1, rng() & 1, rng() & 1, rng() & 1, rng() & 1);
        std::vector<uint8_t> buf = EncodePose(f);
        PoseFrame d{};
        CHECK(DecodePose(buf.data(), buf.size(), d));
        CHECK(SameGrip(d.grip, f.grip));
    }
}

static void TestGripWireLayout() {
    std::mt19937 rng(11);
    PoseFrame f = BuildFkFrame(rng, 1.0, 2);
    PoseFrame none = f;                                         // grip all zero
    PoseFrame full = f;
    full.grip = MakeGrip(21, 10, true, false, true, false, true);   // gripR 10101, gripL 01010
    std::vector<uint8_t> bn = EncodePose(none), bf = EncodePose(full);
    CHECK(bn.size() == bf.size());
    CHECK(GripWord(bn) == 0);
    const uint16_t want = uint16_t(21 | (10 << 5) | (1 << 10) | (1 << 12) | (1 << 14));
    CHECK(GripWord(bf) == want);
    CHECK((GripWord(bf) & 0x8000) == 0);                        // the spare bit is written 0
    // Nothing but those two bytes differs: the grip lives in the byte-aligned header, the body is untouched.
    std::vector<uint8_t> a = bn, b = bf;
    a.erase(a.begin() + kGripOff, a.begin() + kGripOff + 2);
    b.erase(b.begin() + kGripOff, b.begin() + kGripOff + 2);
    CHECK(a == b);
    // Each field has its own bit(s).
    PoseFrame g = f;
    g.grip.gripR = 31;      CHECK(GripWord(EncodePose(g)) == 0x001F);
    g.grip = GripState{}; g.grip.gripL = 31;      CHECK(GripWord(EncodePose(g)) == 0x03E0);
    g.grip = GripState{}; g.grip.offhand = true;  CHECK(GripWord(EncodePose(g)) == 0x0400);
    g.grip = GripState{}; g.grip.altGrip = true;  CHECK(GripWord(EncodePose(g)) == 0x0800);
    g.grip = GripState{}; g.grip.mordhau = true;  CHECK(GripWord(EncodePose(g)) == 0x1000);
    g.grip = GripState{}; g.grip.switched2H = true; CHECK(GripWord(EncodePose(g)) == 0x2000);
    g.grip = GripState{}; g.grip.reverseR = true; CHECK(GripWord(EncodePose(g)) == 0x4000);
}

static void TestGripSaturatesNotWraps() {
    // A byte the game should never hold (garbage read) must not wrap into a plausible grip type: 200 -> 31, 32 -> 31.
    std::mt19937 rng(12);
    PoseFrame f = BuildFkFrame(rng, 1.0, 0);
    f.grip.gripR = 200; f.grip.gripL = 32;
    std::vector<uint8_t> buf = EncodePose(f);
    PoseFrame d{};
    CHECK(DecodePose(buf.data(), buf.size(), d));
    CHECK(d.grip.gripR == 31 && d.grip.gripL == 31);
    CHECK((GripWord(buf) & 0x7FFF) == 0x03FF);
}

static void TestGripSpareBitIgnored() {
    std::mt19937 rng(13);
    PoseFrame f = BuildFkFrame(rng, 1.0, 1);
    f.grip = MakeGrip(5, 9, false, true, false, false, false);
    std::vector<uint8_t> buf = EncodePose(f);
    PoseFrame ref{};
    CHECK(DecodePose(buf.data(), buf.size(), ref));
    buf[kGripOff + 1] |= 0x80;                                  // the spare bit set by a sender
    PoseFrame d{};
    CHECK(DecodePose(buf.data(), buf.size(), d));
    CHECK(SameGrip(d.grip, ref.grip) && SameGrip(d.grip, f.grip));      // read into no flag and no grip type
    for (int i = 0; i < kPoseBones; ++i) CHECK(Vec3Dist(d.bones[i].pos, ref.bones[i].pos) == 0.0);
    // And with every grip bit zero except the spare one: still an all-zero grip.
    f.grip = GripState{};
    buf = EncodePose(f);
    buf[kGripOff + 1] |= 0x80;
    CHECK(DecodePose(buf.data(), buf.size(), d));
    CHECK(SameGrip(d.grip, GripState{}));
}

static void TestVersionOneFrameRefused() {
    std::mt19937 rng(14);
    PoseFrame f = BuildFkFrame(rng, 1.0, 2);
    std::vector<uint8_t> v2 = EncodePose(f);
    CHECK(v2[0] == 2);
    // A real version-1 frame: version byte 1 and no grip bytes.
    std::vector<uint8_t> v1 = v2;
    v1.erase(v1.begin() + kGripOff, v1.begin() + kGripOff + 2);
    v1[0] = 1;
    PoseFrame d{};
    CHECK(!DecodePose(v1.data(), v1.size(), d));
    // The same bytes with the version byte flipped back to 1 (grip bytes present) are refused too.
    std::vector<uint8_t> relabelled = v2;
    relabelled[0] = 1;
    CHECK(!DecodePose(relabelled.data(), relabelled.size(), d));
    CHECK(DecodePose(v2.data(), v2.size(), d));      // sanity: the v2 frame itself decodes
}

static void TestHeaderGrewByExactlyTwoBytes() {
    // Documented v2 header: version(1) + tsMs(4) + stepUs(2) + flags(1) + grip(2) + pelvis pos(4*3) + skeleton scale(2) = 24.
    CHECK(kPoseHeaderBytes == kV1HeaderBytes + 2);
    CHECK(kPoseHeaderBytes == 24);
    CHECK(kPoseFrameMaxBytes == kV1HeaderBytes + 2 + (detail::kBitsMax + 7) / 8);
    // An FK frame needs no overrides, so its size is exactly the header plus the bit-packed body.
    std::mt19937 rng(15);
    const size_t bodyBits0 = detail::kBitsRootQuat + detail::kBitsChildQuats + detail::kBitsVelAll + detail::kBitsOverrideMask;
    const size_t bodyBits1 = bodyBits0 + detail::kBitsPerWeapon;
    PoseFrame f0 = BuildFkFrame(rng, 1.0, 0), f1 = BuildFkFrame(rng, 1.0, 1);
    CHECK(EncodePose(f0).size() == kV1HeaderBytes + 2 + (bodyBits0 + 7) / 8);
    CHECK(EncodePose(f1).size() == kV1HeaderBytes + 2 + (bodyBits1 + 7) / 8);
    // The size does not depend on the grip values.
    f0.grip = MakeGrip(31, 31, true, true, true, true, true);
    CHECK(EncodePose(f0).size() == kV1HeaderBytes + 2 + (bodyBits0 + 7) / 8);
    // Anything shorter than the new header is refused, a header-only buffer is not a frame.
    std::vector<uint8_t> buf = EncodePose(f0);
    PoseFrame d{};
    CHECK(!DecodePose(buf.data(), kPoseHeaderBytes - 1, d));
}

// ---- flail heads (protocol rev 17): flags bit 2 + slot, one block per head after every weapon block ------------------
static constexpr size_t kFlagsOff = 7;   // version(1) + tsMs(4) + stepUs(2)

static bool SameWeapon(const WeaponSample& a, const WeaponSample& b) {
    return a.present == b.present && a.hand == b.hand && a.classHash == b.classHash && Vec3Dist(a.pos, b.pos) == 0.0 &&
           QuatAngleDeg(a.rot, b.rot) == 0.0 && Vec3Dist(a.vel, b.vel) == 0.0 && Vec3Dist(a.angVelDeg, b.angVelDeg) == 0.0 &&
           Vec3Dist(a.base, b.base) == 0.0 && Vec3Dist(a.tip, b.tip) == 0.0;
}

static void TestFlailHeadFlagsAndSize() {
    std::mt19937 rng(21);
    const PoseFrame f = BuildFkFrame(rng, 1.0, 2);
    const size_t bodyBits = detail::kBitsRootQuat + detail::kBitsChildQuats + detail::kBitsVelAll + detail::kBitsOverrideMask +
                            2 * detail::kBitsPerWeapon;
    CHECK(EncodePose(f).size() == kPoseHeaderBytes + (bodyBits + 7) / 8);
    // Each slot has its own bit, and each head adds exactly one head block.
    for (int mask = 0; mask < 4; ++mask) {
        PoseFrame g = f;
        for (int wi = 0; wi < kPoseWeapons; ++wi) if (mask & (1 << wi)) AddHead(rng, g, wi);
        const std::vector<uint8_t> buf = EncodePose(g);
        const int heads = (mask & 1) + ((mask >> 1) & 1);
        CHECK(buf[kFlagsOff] == uint8_t(3 | (mask << 2)));
        CHECK(buf.size() == kPoseHeaderBytes + (bodyBits + size_t(heads) * detail::kBitsPerHead + 7) / 8);
        PoseFrame d{};
        CHECK(DecodePose(buf.data(), buf.size(), d));
        CHECK(d.weapons[0].hasHead == ((mask & 1) != 0));
        CHECK(d.weapons[1].hasHead == ((mask & 2) != 0));
        // Each head decodes into its own slot.
        for (int wi = 0; wi < kPoseWeapons; ++wi)
            if (mask & (1 << wi)) CHECK(Vec3Dist(d.weapons[wi].head.pos, g.weapons[wi].head.pos) <= kMaxPosErrUu);
    }
    // The budget: 17.1 bytes a head (pos 3x16, rot 2+3x13, vel and angVel 6x8 bits); the worst case has both.
    CHECK(detail::kBitsPerHead == 137);
    CHECK(detail::kBitsWeaponsMax == 2 * (detail::kBitsPerWeapon + detail::kBitsPerHead));
    CHECK(kPoseFrameMaxBytes == kPoseHeaderBytes + (detail::kBitsMax + 7) / 8);
}

static void TestFlailHeadOnlyWithItsWeapon() {
    std::mt19937 rng(22);
    // A head on an absent weapon slot is not sent and does not decode.
    PoseFrame f = BuildFkFrame(rng, 1.0, 1);
    const std::vector<uint8_t> plain = EncodePose(f);
    f.weapons[1].hasHead = true;
    f.weapons[1].head.pos = V3{1, 2, 3};
    const std::vector<uint8_t> buf = EncodePose(f);
    CHECK(buf == plain);
    PoseFrame d{};
    CHECK(DecodePose(buf.data(), buf.size(), d));
    CHECK(!d.weapons[1].hasHead && !d.weapons[1].present);
    // A frame that flags a head for a slot with no weapon is refused, for either slot, even when the other slot holds one.
    for (int wi = 0; wi < kPoseWeapons; ++wi) {
        for (int other = 0; other < 2; ++other) {
            PoseFrame g = BuildFkFrame(rng, 1.0, 2);
            g.weapons[wi].present = false;
            if (!other) g.weapons[1 - wi].present = false;
            std::vector<uint8_t> b = EncodePose(g);
            CHECK(DecodePose(b.data(), b.size(), d));
            b[kFlagsOff] = uint8_t(b[kFlagsOff] | (4 << wi));
            b.resize(b.size() + 32, 0);   // room for a head block: the refusal is the flag check, not truncation
            CHECK(!DecodePose(b.data(), b.size(), d));
        }
    }
}

static void TestFlailHeadLeavesTheRestAlone() {
    std::mt19937 rng(23);
    PoseFrame f = BuildFkFrame(rng, 1.0, 2);
    PoseFrame g = f;
    AddHead(rng, g, 0);
    AddHead(rng, g, 1);
    std::vector<uint8_t> plain = EncodePose(f), headed = EncodePose(g);
    PoseFrame dp{}, dh{};
    CHECK(DecodePose(plain.data(), plain.size(), dp));
    CHECK(DecodePose(headed.data(), headed.size(), dh));
    for (int i = 0; i < kPoseBones; ++i) CHECK(Vec3Dist(dp.bones[i].pos, dh.bones[i].pos) == 0.0);
    for (int wi = 0; wi < kPoseWeapons; ++wi) CHECK(SameWeapon(dp.weapons[wi], dh.weapons[wi]));
    // The head blocks follow every weapon block: with the head bits cleared, a headed frame reads as the plain one (the
    // trailing head bytes are ignored), so a decoder that does not know the bits still reads both weapons.
    headed[kFlagsOff] = uint8_t(headed[kFlagsOff] & ~0x0C);
    PoseFrame dc{};
    CHECK(DecodePose(headed.data(), headed.size(), dc));
    for (int wi = 0; wi < kPoseWeapons; ++wi) CHECK(SameWeapon(dp.weapons[wi], dc.weapons[wi]) && !dc.weapons[wi].hasHead);
}

static void TestFlailHeadPositionIsClosedLoop() {
    // The head goes relative to the weapon root the DECODER rebuilds (itself quantised to 1/8 uu), so the head's error is
    // only its own 1/16 uu step, never the root's error on top.
    std::mt19937 rng(24);
    double maxErr = 0;
    for (int i = 0; i < 500; ++i) {
        PoseFrame f = BuildFkFrame(rng, 1.0, 1);
        f.bones[0].pos = V3{100.0, -200.0, 50.0};   // a float-exact pelvis: no pelvis rounding in the bound
        f.weapons[0].pos = Vec3Add(f.bones[0].pos, V3{30.0 + 0.0611 * i, -20.0 - 0.0587 * i, 10.0 + 0.0533 * i});
        AddHead(rng, f, 0);
        f.weapons[0].head.pos = Vec3Add(f.weapons[0].pos, V3{40.03 + 0.0013 * i, -60.02, 75.01});
        std::vector<uint8_t> buf = EncodePose(f);
        PoseFrame d{};
        CHECK(DecodePose(buf.data(), buf.size(), d));
        const V3 e = Vec3Sub(d.weapons[0].head.pos, f.weapons[0].head.pos);
        CHECK(std::fabs(e.x) <= kHeadPosStep / 2 + 1e-9 && std::fabs(e.y) <= kHeadPosStep / 2 + 1e-9 &&
              std::fabs(e.z) <= kHeadPosStep / 2 + 1e-9);
        maxErr = std::max(maxErr, Vec3Dist(d.weapons[0].head.pos, f.weapons[0].head.pos));
    }
    std::printf("flail head closed loop: max pos err %.4f uu (step 1/16 uu)\n", maxErr);
}

static void TestFlailHeadHostileValuesStayFinite() {
    std::mt19937 rng(25);
    PoseFrame f = BuildFkFrame(rng, 1.0, 1);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    f.weapons[0].hasHead = true;
    f.weapons[0].head.pos = V3{nan, inf, -inf};
    f.weapons[0].head.rot = Q4{nan, 0, 0, 1};
    f.weapons[0].head.vel = V3{inf, nan, 1e30};
    f.weapons[0].head.angVelDeg = V3{-inf, nan, -1e30};
    std::vector<uint8_t> buf = EncodePose(f);
    PoseFrame d{};
    CHECK(DecodePose(buf.data(), buf.size(), d));
    CHECK(AllFinite(d) && d.weapons[0].hasHead);
    // A head beyond the wire range saturates at the range, it does not wrap to the other side.
    f = BuildFkFrame(rng, 1.0, 1);
    AddHead(rng, f, 0);
    f.weapons[0].head.pos = Vec3Add(f.weapons[0].pos, V3{5000.0, -5000.0, 0.0});
    buf = EncodePose(f);
    CHECK(DecodePose(buf.data(), buf.size(), d));
    const V3 rel = Vec3Sub(d.weapons[0].head.pos, d.weapons[0].pos);
    CHECK(rel.x > 2000.0 && rel.y < -2000.0 && std::fabs(rel.z) < 0.1);
}

static void TestMuDecodePreservesEveryCode() {
    for (double max : {kVLinMax, kVAngMax, 1234.5}) {
        for (uint32_t c = 0; c < 256; ++c) {
            const double m = double(c & 0x7Fu) / 127.0;
            const double magnitude = (std::pow(1.0 + kMuLaw, m) - 1.0) / kMuLaw * max;
            const double expected = (c & 0x80u) ? -magnitude : magnitude;
            // Exact decoded values are the compatibility contract, including negative zero and the fallback scale.
            CHECK(std::bit_cast<uint64_t>(detail::MuDecode(c, max)) == std::bit_cast<uint64_t>(expected));
        }
    }
}

int main() {
    TestMuDecodePreservesEveryCode();
    TestPropertyRoundTrip();
    TestDislocatedBoneOverridesExactly();
    TestEncodedSize();
    TestNegatedQuatEncodesIdentically();
    TestIdentityAnd180DegreeRotations();
    TestUnknownVersionRejected();
    TestNaNHeaderRejected();
    TestTruncationAlwaysRejected();
    TestHostileRandomBuffersNeverCrashAndStayFinite();
    TestWeaponClassHash();
    TestGripRoundTripExtremes();
    TestGripWireLayout();
    TestGripSaturatesNotWraps();
    TestGripSpareBitIgnored();
    TestVersionOneFrameRefused();
    TestHeaderGrewByExactlyTwoBytes();
    TestFlailHeadFlagsAndSize();
    TestFlailHeadOnlyWithItsWeapon();
    TestFlailHeadLeavesTheRestAlone();
    TestFlailHeadPositionIsClosedLoop();
    TestFlailHeadHostileValuesStayFinite();
    if (g_fail) std::printf("%d CHECK(S) FAILED\n", g_fail);
    else std::printf("all tests passed\n");
    return g_fail != 0;
}
