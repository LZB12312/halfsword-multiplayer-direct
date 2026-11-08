#include "sync/hsm_two_hand_pose.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static bool Near(V3 a, V3 b, double epsilon = 1e-8) { return Vec3Dist(a, b) < epsilon; }
static Q4 Turn(V3 axis, double degrees) {
    const double half = degrees * 3.14159265358979323846 / 360.0;
    return {axis.x * std::sin(half), axis.y * std::sin(half), axis.z * std::sin(half), std::cos(half)};
}
static GripFrame Basis(Q4 q, V3 pos = {}) {
    return {pos, QuatRotateVec(q, {1, 0, 0}), QuatRotateVec(q, {0, 1, 0})};
}
static PoseFrame Spear() {
    PoseFrame f;
    f.grip.offhand = true;
    f.weapons[0].present = true;
    f.weapons[0].classHash = 4321;
    f.weapons[0].pos = {100, 200, 300};
    f.bones[kBoneHandR].pos = {100, 200, 330};
    f.bones[kBoneHandL].pos = {100, 200, 380};
    return f;
}
static TwoHandPosePolicy Ready() {
    TwoHandPosePolicy p;
    p.enabled = p.gripLock = p.localReady = true;
    p.localClassHash = 4321;
    p.validBones = kTwoHandArmBones;
    return p;
}
static void SameFrame(const GripFrame& a, const GripFrame& b) {
    CHECK(Near(a.pos, b.pos)); CHECK(Near(a.primary, b.primary)); CHECK(Near(a.secondary, b.secondary));
}
// Independent world-space invariant: at the transmitted pose, both references of each joint coincide.
static void Coincide(const PoseFrame& f, const std::array<GripFrame, 2>& hand, const std::array<GripFrame, 2>& weapon) {
    constexpr int bones[2] = {kBoneHandR, kBoneHandL};
    const WeaponSample& w = f.weapons[0];
    for (int i = 0; i < 2; ++i) {
        const BoneSample& h = f.bones[bones[i]];
        CHECK(Near(Vec3Add(w.pos, QuatRotateVec(w.rot, weapon[i].pos)), Vec3Add(h.pos, QuatRotateVec(h.rot, hand[i].pos))));
        CHECK(Near(QuatRotateVec(w.rot, weapon[i].primary), QuatRotateVec(h.rot, hand[i].primary)));
        CHECK(Near(QuatRotateVec(w.rot, weapon[i].secondary), QuatRotateVec(h.rot, hand[i].secondary)));
        Q4 reference;
        CHECK(GripFrameRotation(weapon[i], reference));
        CHECK(Near(QuatRotateVec(reference, {1, 0, 0}), weapon[i].primary));
        CHECK(Near(QuatRotateVec(reference, {0, 1, 0}), weapon[i].secondary));
    }
}

static void TestSlidesAndNonidentityFrames() {
    PoseFrame f = Spear();
    f.bones[kBoneHandR].rot = Turn({1, 0, 0}, 30);
    f.bones[kBoneHandL].rot = Turn({0, 1, 0}, -70);
    const std::array<GripFrame, 2> hand = {Basis(Turn({0, 0, 1}, 25), {3, -2, 5}), Basis(Turn({1, 0, 0}, -15), {-4, 7, 2})};
    std::array<GripFrame, 2> previous{}, current{};
    CHECK(BuildTwoHandPose(f, Ready(), hand, previous));
    Coincide(f, hand, previous);
    for (int step = 1; step <= 20; ++step) {
        // Owner slides continuously, only 2 uu per step: no old 60 uu threshold or stable-seat wait.
        f.bones[kBoneHandR].pos.z += 2;
        f.bones[kBoneHandL].pos.z -= 1;
        CHECK(BuildTwoHandPose(f, Ready(), hand, current));
        CHECK(Near(Vec3Sub(current[0].pos, previous[0].pos), {0, 0, 2}));
        CHECK(Near(Vec3Sub(current[1].pos, previous[1].pos), {0, 0, -1}));
        Coincide(f, hand, current);
        previous = current;
    }
}

static void TestRebuiltConstraintScale() {
    const std::array<GripFrame, 2> stored = {Basis(Turn({0, 1, 0}, 35), {8, -16, 24}),
                                           Basis(Turn({0, 0, 1}, -70), {-32, 40, 64})};
    for (double scale : {0.875, 0.9375, 1.0, 2.0}) {
        std::array<GripFrame, 2> active;
        CHECK(ScaleGripFrames(stored, {scale, scale, scale}, active));
        for (size_t i = 0; i < active.size(); ++i) {
            CHECK(Near(active[i].pos, Vec3Scale(stored[i].pos, scale)));
            CHECK(Near(active[i].primary, stored[i].primary));
            CHECK(Near(active[i].secondary, stored[i].secondary));
        }
        // Rebuilt hand-side anchors at short fighter heights must still meet both pose-derived weapon frames.
        PoseFrame owner = Spear();
        owner.bones[kBoneHandR].rot = Turn({1, 0, 0}, -30);
        owner.bones[kBoneHandL].rot = Turn({0, 1, 0}, 50);
        std::array<GripFrame, 2> weapon;
        CHECK(BuildTwoHandPose(owner, Ready(), active, weapon));
        Coincide(owner, active, weapon);
        auto aliased = stored;
        CHECK(ScaleGripFrames(aliased, {scale, scale, scale}, aliased));
        SameFrame(aliased[0], active[0]); SameFrame(aliased[1], active[1]);
    }
    // Small transform roundoff is harmless; use the engine's minimum axis converted to float, not an average.
    std::array<GripFrame, 2> rounded;
    CHECK(ScaleGripFrames(stored, {0.937501, 0.9375, 0.937502}, rounded));
    CHECK(Near(rounded[0].pos, Vec3Scale(stored[0].pos, 0.9375)));
    CHECK(ScaleGripFrames(stored, {0.91, 0.91, 0.91}, rounded));
    CHECK(Near(rounded[0].pos, Vec3Scale(stored[0].pos, double(float(0.91)))));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    auto reject = [&](const std::array<GripFrame, 2>& input, V3 scale) {
        auto output = stored;
        CHECK(!ScaleGripFrames(input, scale, output));
        SameFrame(output[0], stored[0]); SameFrame(output[1], stored[1]);
    };
    for (V3 scale : {V3{0, 0, 0}, V3{-1, -1, -1}, V3{1, -1, 1}, V3{0.001, 0.001, 0.001},
                     V3{0.875, 0.875, 0.95}, V3{0.95, 0.875, 0.875}, V3{0.875, 0.95, 0.875},
                     V3{nan, 1, 1}, V3{1, inf, 1}, V3{1, 1, nan}, V3{1e300, 1e300, 1e300}}) reject(stored, scale);
    for (size_t i = 0; i < stored.size(); ++i) {
        auto bad = stored; bad[i].pos.z = nan; reject(bad, {1, 1, 1});
        bad = stored; bad[i].primary = {}; reject(bad, {1, 1, 1});
        bad = stored; bad[i].pos.x = std::numeric_limits<double>::max(); reject(bad, {2, 2, 2});
    }
}

static void TestWorldTransformAndQuaternionSign() {
    PoseFrame f = Spear();
    f.weapons[0].rot = Turn({0, 1, 0}, 70);
    f.bones[kBoneHandR].rot = Turn({1, 0, 0}, -40);
    f.bones[kBoneHandL].rot = Turn({0, 0, 1}, 135);
    const std::array<GripFrame, 2> hand = {Basis(Turn({0, 1, 0}, 20), {1, 3, 7}), Basis(Turn({0, 0, 1}, -90), {-5, 9, 2})};
    std::array<GripFrame, 2> before{}, after{};
    CHECK(BuildTwoHandPose(f, Ready(), hand, before));
    Coincide(f, hand, before);
    const Q4 turn = Turn({0, 0, 1}, 65);
    const V3 shift{700, -200, 55};
    auto move = [&](V3& p, Q4& q) { p = Vec3Add(shift, QuatRotateVec(turn, p)); q = QuatMul(turn, q); };
    move(f.weapons[0].pos, f.weapons[0].rot);
    move(f.bones[kBoneHandR].pos, f.bones[kBoneHandR].rot);
    move(f.bones[kBoneHandL].pos, f.bones[kBoneHandL].rot);
    CHECK(BuildTwoHandPose(f, Ready(), hand, after));
    SameFrame(before[0], after[0]); SameFrame(before[1], after[1]);
    Coincide(f, hand, after);
    auto negate = [](Q4& q) { q = {-q.x, -q.y, -q.z, -q.w}; };
    negate(f.weapons[0].rot); negate(f.bones[kBoneHandL].rot);
    CHECK(BuildTwoHandPose(f, Ready(), hand, after));
    SameFrame(before[0], after[0]); SameFrame(before[1], after[1]);
}

static void TestSwitchesAndReverseUsePose() {
    PoseFrame f = Spear();
    const std::array<GripFrame, 2> hand{};
    std::array<GripFrame, 2> out{};
    for (unsigned flags = 0; flags < 16; ++flags) {
        f.grip.altGrip = (flags & 1) != 0; f.grip.mordhau = (flags & 2) != 0;
        f.grip.switched2H = (flags & 4) != 0; f.grip.reverseR = (flags & 8) != 0;
        f.bones[kBoneHandR].pos.z = f.grip.switched2H ? 380 : 330;
        f.bones[kBoneHandL].pos.z = f.grip.switched2H ? 330 : 380;
        f.weapons[0].rot = Turn({0, 1, 0}, f.grip.reverseR ? 180 : 0);
        CHECK(BuildTwoHandPose(f, Ready(), hand, out));
        Coincide(f, hand, out);
    }
}

static void TestGuards() {
    const PoseFrame f = Spear();
    const TwoHandPosePolicy good = Ready();
    CHECK(TwoHandPoseAllowed(f, good));
    auto noPolicy = [&](TwoHandPosePolicy p) { CHECK(!TwoHandPoseAllowed(f, p)); };
    auto p = good; p.enabled = false; noPolicy(p);
    p = good; p.gripLock = false; noPolicy(p);
    p = good; p.localReady = false; noPolicy(p);
    p = good; p.localFlail = true; noPolicy(p);
    p = good; p.localLeftWeapon = true; noPolicy(p);
    p = good; p.localClassHash = 0; noPolicy(p);
    p = good; p.localClassHash = 4322; noPolicy(p);
    for (int slot = 0; slot < kPoseWeapons; ++slot) { p = good; p.pinnedWeapons = 1u << slot; noPolicy(p); }
    for (int bone = 0; bone < kPoseBones; ++bone) {
        p = good; p.heldBones = 1u << bone;
        CHECK(TwoHandPoseAllowed(f, p) == ((kTwoHandArmBones & (1u << bone)) == 0));
        p = good; p.validBones &= ~(1u << bone);
        CHECK(TwoHandPoseAllowed(f, p) == ((kTwoHandArmBones & (1u << bone)) == 0));
    }
    auto noPose = [&](PoseFrame bad) { CHECK(!TwoHandPoseAllowed(bad, good)); };
    auto bad = f; bad.grip.offhand = false; noPose(bad);
    bad = f; bad.weapons[0].present = false; noPose(bad);
    bad = f; bad.weapons[0].hand = 1; noPose(bad);
    bad = f; bad.weapons[0].classHash = 0; noPose(bad);
    bad = f; bad.weapons[0].hasHead = true; noPose(bad);
    bad = f; bad.weapons[1].present = true; noPose(bad);
}

static void TestInvalidInputsAreAtomic() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const std::array<GripFrame, 2> sentinel = {Basis(Q4{}, {42, 2, 3}), Basis(Q4{}, {-42, 4, 5})};
    auto reject = [&](const PoseFrame& f, const std::array<GripFrame, 2>& frames) {
        auto out = sentinel;
        TwoHandPoseReason reason = TwoHandPoseReason::Ready;
        CHECK(!BuildTwoHandPose(f, Ready(), frames, out, &reason));
        CHECK(reason == TwoHandPoseReason::InvalidPose);
        SameFrame(out[0], sentinel[0]); SameFrame(out[1], sentinel[1]);
    };
    const PoseFrame f = Spear();
    for (double bad : {nan, inf, -inf}) {
        auto pose = f; pose.weapons[0].pos.x = bad; reject(pose, {});
        pose = f; pose.weapons[0].rot.w = bad; reject(pose, {});
        for (int bone : {kBoneHandR, kBoneHandL}) {
            pose = f; pose.bones[bone].pos.y = bad; reject(pose, {});
            pose = f; pose.bones[bone].rot.x = bad; reject(pose, {});
        }
        std::array<GripFrame, 2> frames{};
        frames[1].pos.z = bad; reject(f, frames);
        frames = {}; frames[0].primary.x = bad; reject(f, frames);
        frames = {}; frames[1].secondary.y = bad; reject(f, frames);
    }
    auto pose = f; pose.weapons[0].rot = {0, 0, 0, 0}; reject(pose, {});
    pose = f; pose.bones[kBoneHandL].rot = {0, 0, 0, 0}; reject(pose, {});
    pose = f; pose.weapons[0].rot.x = 1e300; reject(pose, {});  // norm overflow
    pose = f; pose.weapons[0].pos.x = -1e308; pose.bones[kBoneHandR].pos.x = 1e308; reject(pose, {});
    std::array<GripFrame, 2> frames{};
    frames[1].primary = {}; reject(f, frames);
    frames = {}; frames[0].secondary = frames[0].primary; reject(f, frames);
    frames = {}; frames[1].primary = {2, 0, 0}; reject(f, frames);
}

static void TestReasons() {
    const PoseFrame f = Spear();
    const TwoHandPosePolicy good = Ready();
    CHECK(TwoHandPoseStatus(f, good) == TwoHandPoseReason::Ready);
    auto p = good; p.enabled = false;
    CHECK(TwoHandPoseStatus(f, p) == TwoHandPoseReason::Disabled);
    p = good; p.gripLock = false;
    CHECK(TwoHandPoseStatus(f, p) == TwoHandPoseReason::FreeGrip);
    auto pose = f; pose.grip.offhand = false;
    CHECK(TwoHandPoseStatus(pose, good) == TwoHandPoseReason::NotTwoHanded);
    p = good; p.localClassHash = 123;
    CHECK(TwoHandPoseStatus(f, p) == TwoHandPoseReason::WeaponMismatch);
    p = good; p.localFlail = true;
    CHECK(TwoHandPoseStatus(f, p) == TwoHandPoseReason::Flail);
    p = good; p.pinnedWeapons = 1;
    CHECK(TwoHandPoseStatus(f, p) == TwoHandPoseReason::Pinned);
    p = good; p.heldBones = 1u << kBoneHandL;
    CHECK(TwoHandPoseStatus(f, p) == TwoHandPoseReason::HeldArm);
    p = good; p.validBones = 0;
    CHECK(TwoHandPoseStatus(f, p) == TwoHandPoseReason::MissingBodies);
    p = good; p.localReady = false;
    CHECK(TwoHandPoseStatus(f, p) == TwoHandPoseReason::NotReady);
    pose = f; pose.bones[kBoneHandR].rot = {0, 0, 0, 0};
    CHECK(TwoHandPoseStatus(pose, good) == TwoHandPoseReason::InvalidPose);
    // The application result reports the policy reason too; a caller need not duplicate the decision to log it.
    std::array<GripFrame, 2> out{};
    TwoHandPoseReason reason = TwoHandPoseReason::Ready;
    CHECK(!BuildTwoHandPose(f, p, {}, out, &reason));
    CHECK(reason == TwoHandPoseReason::NotReady);
    CHECK(BuildTwoHandPose(f, good, {}, out, &reason));
    CHECK(reason == TwoHandPoseReason::Ready);
}

static void TestKnownRigidMultipartWeapons() {
    // Refused up to 0.8.0-pre30, all but the first two spears and the warstaffs.
    for (const char* name : {"ModularWeaponBP_Spear_A_C", "ModularWeaponBP_Spear_B_C", "ModularWeaponBP_Spear_C_C",
                             "ModularWeaponBP_Spear_D_C", "ModularWeaponBP_WarStaff_A_C", "ModularWeaponBP_WarStaff_B_C",
                             "ModularWeaponBP_Staff_C", "ModularWeaponBP_Billhook_A_C", "ModularWeaponBP_Billhook_B_C", "ModularWeaponBP_Billhook_C_C",
                             "ModularWeaponBP_Billhook_D_C", "ModularWeaponBP_Halberd_A_C", "ModularWeaponBP_Halberd_B_C",
                             "ModularWeaponBP_Halberd_C_C", "ModularWeaponBP_Halberd_D_C"}) {
        PoseFrame owner = Spear();
        TwoHandPosePolicy policy = Ready();
        owner.weapons[0].classHash = policy.localClassHash = WeaponClassHash(name);
        owner.weapons[0].hasHead = true;   // the existing reader finds the rigid Blade component
        CHECK(IsRigidTwoHandWeapon(owner.weapons[0].classHash));
        CHECK(TwoHandPoseStatus(owner, policy) == TwoHandPoseReason::Ready);
        std::array<GripFrame, 2> frames;
        CHECK(BuildTwoHandPose(owner, policy, {}, frames));
        policy.localMultipart = true;      // its parts here are no reason to refuse it
        CHECK(TwoHandPoseStatus(owner, policy) == TwoHandPoseReason::Ready);
        policy.localFlail = true;          // a real chain here wins over a class name
        CHECK(TwoHandPoseStatus(owner, policy) == TwoHandPoseReason::Flail);
    }
    CHECK(IsRigidTwoHandWeapon(WeaponClassHash("modularweaponbp_warstaff_a_c")));
    // Multipart weapons off the list stay on the grip path, the wide catch-all classes first of all.
    for (const char* name : {"ModularWeaponBP_Flail_A_C", "ModularWeaponBP_Flail_B_C", "ModularWeaponBP_Flail_C_C",
                             "ModularWeaponBP_Flail_D_C", "ModularWeaponBP_Spear_Custom_C", "ModularWeaponBP_WarStaff_A",
                             "ModularWeaponBP_C", "ModularWeaponBP_Axe_C", "ModularWeaponBP_Axe_2_C", "ModularWeaponBP_Axe_6_C",
                             "ModularWeaponBP_Axe2H_C", "ModularWeaponBP_Staff"}) {
        PoseFrame owner = Spear();
        TwoHandPosePolicy policy = Ready();
        owner.weapons[0].classHash = policy.localClassHash = WeaponClassHash(name);
        owner.weapons[0].hasHead = true;
        CHECK(!IsRigidTwoHandWeapon(owner.weapons[0].classHash));
        CHECK(TwoHandPoseStatus(owner, policy) == TwoHandPoseReason::NotListed);
        owner.weapons[0].hasHead = false;   // the owner's sample missed the parts: what is held here still counts
        policy.localMultipart = true;
        CHECK(TwoHandPoseStatus(owner, policy) == TwoHandPoseReason::NotListed);
        policy.localMultipart = false;      // a weapon of one piece needs no list
        CHECK(TwoHandPoseStatus(owner, policy) == TwoHandPoseReason::Ready);
        policy.localFlail = true;           // the log names what it is
        CHECK(TwoHandPoseStatus(owner, policy) == TwoHandPoseReason::Flail);
    }
    CHECK(!IsRigidTwoHandWeapon(0));
    CHECK(std::string(TwoHandPoseReasonName(TwoHandPoseReason::NotListed)) == "not-listed");
    CHECK(TwoHandWeaponAccepted(false, WeaponClassHash("ModularWeaponBP_C")));
    CHECK(!TwoHandWeaponAccepted(true, WeaponClassHash("ModularWeaponBP_C")));
    CHECK(TwoHandWeaponAccepted(true, WeaponClassHash("ModularWeaponBP_Halberd_D_C")));

    // The head of a weapon the pose holds is not servoed: a welded Blade's velocity is the whole weapon's.
    CHECK(HeadServoed(true, false, 0));
    CHECK(!HeadServoed(true, true, 0));
    CHECK(HeadServoed(true, true, 1));     // the pose is weapon 0's alone
    CHECK(!HeadServoed(false, false, 0));

    // Under the pose both hands lock; without it the off hand keeps the game's sliding hold and the right hand stays locked.
    CHECK(HoldFor(1, true, true, true) == GripHold::Locked);
    CHECK(HoldFor(0, false, true, true) == GripHold::Locked);
    CHECK(HoldFor(1, true, false, true) == GripHold::GameOffhand);   // the pose let go: the game's limits again
    CHECK(HoldFor(1, true, true, false) == GripHold::GameOffhand);   // standin_two_hand_lock=0
    CHECK(HoldFor(0, false, false, true) == GripHold::Locked);
    CHECK(HoldFor(0, true, false, false) == GripHold::Locked);      // only the off hand has the game's sliding hold
    CHECK(HoldFor(1, false, false, true) == GripHold::Locked);      // a weapon held in the left hand alone
}

static FighterNow ReplicaOf(const PoseFrame& f) {
    FighterNow now;
    for (int i = 0; i < kPoseBones; ++i) {
        now.valid[i] = true;
        now.bones[i] = {f.bones[i].pos, f.bones[i].rot, {}};
    }
    now.weaponValid[0] = true;
    now.weapons[0] = {f.weapons[0].pos, f.weapons[0].rot, {}};
    return now;
}

static void TestMeasurementsExposeLongWeaponError() {
    PoseFrame f = Spear();
    f.weapons[0].tip = {100, 200, 500};   // a 2 m shaft from the root
    FighterNow now = ReplicaOf(f);
    std::array<double, 5> errors{};
    CHECK(MeasureTwoHandPose(f, now, 4321, f.weapons[0].tip, errors));
    for (double e : errors) CHECK(std::fabs(e) < 1e-8);
    now.weapons[0].rot = Turn({0, 1, 0}, 90);
    V3 localTip = Vec3Add(now.weapons[0].pos, QuatRotateVec(now.weapons[0].rot, {0, 0, 200}));
    CHECK(MeasureTwoHandPose(f, now, 4321, localTip, errors));
    // Hand errors alone would claim perfect playback, despite a visibly wrong direction of attack.
    CHECK(errors[0] == 0 && errors[1] == 0 && errors[2] == 0);
    CHECK(std::fabs(errors[3] - std::sqrt(80000.0)) < 1e-8);
    CHECK(std::fabs(errors[4] - 90.0) < 1e-8);
    now.bones[kBoneHandR].pos.x += 3;
    now.bones[kBoneHandL].pos.y -= 4;
    now.weapons[0].pos.z += 5;
    now.weapons[0].rot = Q4{};
    localTip = Vec3Add(now.weapons[0].pos, {0, 0, 200});
    CHECK(MeasureTwoHandPose(f, now, 4321, localTip, errors));
    CHECK(errors[0] == 3 && errors[1] == 4 && errors[2] == 5 && errors[3] == 5);
    CHECK(std::fabs(errors[4]) < 1e-8);
    f.weapons[0].classHash = WeaponClassHash("ModularWeaponBP_Spear_A_C");
    f.weapons[0].hasHead = true;
    CHECK(MeasureTwoHandPose(f, now, f.weapons[0].classHash, localTip, errors));
}

static void TestMeasurementsIgnorePacketTip() {
    PoseFrame f = Spear();
    f.weapons[0].rot = QuatMul(Turn({0, 1, 0}, 70), Turn({0, 0, 1}, -35));
    const FighterNow now = ReplicaOf(f);
    const V3 localTip = Vec3Add(now.weapons[0].pos, QuatRotateVec(now.weapons[0].rot, {20, 40, 200}));
    std::array<double, 5> errors{};
    // A packet tip comes from a neighboring frame and may also have been clamped by its wire representation. It is
    // unrelated to the sampled root/rotation: perfect root/rotation playback must still measure a perfect tip.
    for (double packetCoordinate : {0.0, -3276.7, 987654.0, 1e308,
                                    std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        f.weapons[0].tip = {packetCoordinate, 123, -456};
        CHECK(MeasureTwoHandPose(f, now, 4321, localTip, errors));
        CHECK(errors[0] == 0 && errors[1] == 0 && errors[2] == 0);
        CHECK(errors[3] < 1e-8);
        CHECK(errors[4] < 1e-5);
    }
}

static void TestMeasurementsRejectIncompleteData() {
    const PoseFrame f = Spear();
    const FighterNow good = ReplicaOf(f);
    const std::array<double, 5> sentinel = {1, 2, 3, 4, 5};
    auto reject = [&](const PoseFrame& pose, const FighterNow& now, uint32_t cls, V3 tip) {
        auto out = sentinel;
        CHECK(!MeasureTwoHandPose(pose, now, cls, tip, out));
        CHECK(out == sentinel);
    };
    reject(f, good, 0, {}); reject(f, good, 4322, {});
    auto pose = f; pose.grip.offhand = false; reject(pose, good, 4321, {});
    pose = f; pose.weapons[0].hasHead = true; reject(pose, good, 4321, {});
    pose = f; pose.weapons[1].present = true; reject(pose, good, 4321, {});
    auto now = good; now.weaponValid[0] = false; reject(f, now, 4321, {});
    now = good; now.weaponValid[1] = true; reject(f, now, 4321, {});
    for (int bone : {kBoneHandR, kBoneHandL}) {
        now = good; now.valid[bone] = false; reject(f, now, 4321, {});
        now = good; now.bones[bone].rot = {0, 0, 0, 0}; reject(f, now, 4321, {});
    }
    for (double bad : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        now = good; now.weapons[0].pos.x = bad; reject(f, now, 4321, {});
        now = good; now.weapons[0].rot.y = bad; reject(f, now, 4321, {});
        now = good; now.bones[kBoneHandR].pos.x = bad; reject(f, now, 4321, {});
        now = good; now.bones[kBoneHandL].pos.z = bad; reject(f, now, 4321, {});
        reject(f, good, 4321, {0, bad, 0});
        pose = f; pose.weapons[0].pos.z = bad; reject(pose, good, 4321, {});
    }
    pose = f; pose.weapons[0].pos.z = 1e308; reject(pose, good, 4321, {});   // finite inputs, overflowing distance
    now = good; now.weapons[0].pos.x = -1e308; reject(f, now, 4321, {1e308, 0, 0});  // overflowing local tip offset
}

static void TestFrameRotations() {
    // Exercise positive-trace and each diagonal-dominant quaternion reconstruction branch, and handedness.
    for (V3 axis : {V3{1, 0, 0}, V3{0, 1, 0}, V3{0, 0, 1}}) {
        for (double degrees : {0.0, 35.0, 90.0, 179.0, 180.0, 215.0}) {
            const GripFrame f = Basis(Turn(axis, degrees));
            Q4 q;
            CHECK(GripFrameRotation(f, q));
            CHECK(Near(QuatRotateVec(q, {1, 0, 0}), f.primary));
            CHECK(Near(QuatRotateVec(q, {0, 1, 0}), f.secondary));
            CHECK(Near(QuatRotateVec(q, {0, 0, 1}), twohanddetail::Cross(f.primary, f.secondary)));
        }
    }
    GripFrame rounded; rounded.primary.x = 1.0000001; rounded.secondary.x = 0.0000001;
    Q4 q;
    CHECK(GripFrameRotation(rounded, q));
    CHECK(Near(QuatRotateVec(q, {1, 0, 0}), {1, 0, 0}));
    for (GripFrame invalid : {GripFrame{{}, {}, {0, 1, 0}}, GripFrame{{}, {1, 0, 0}, {1, 0, 0}}, GripFrame{{}, {2, 0, 0}, {0, 1, 0}}}) {
        q = {0, 0, 1, 0};
        CHECK(!GripFrameRotation(invalid, q));
        CHECK(q.x == 0 && q.y == 0 && q.z == 1 && q.w == 0);
    }
}

int main() {
    TestSlidesAndNonidentityFrames();
    TestRebuiltConstraintScale();
    TestWorldTransformAndQuaternionSign();
    TestSwitchesAndReverseUsePose();
    TestGuards();
    TestInvalidInputsAreAtomic();
    TestReasons();
    TestKnownRigidMultipartWeapons();
    TestMeasurementsExposeLongWeaponError();
    TestMeasurementsIgnorePacketTip();
    TestMeasurementsRejectIncompleteData();
    TestFrameRotations();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("test_hsm_two_hand_pose: ok");
    return 0;
}
