// Continuous two-handed grip references, independent of the engine. The right hand keeps driving the weapon and
// the off hand keeps riding its joint. Only the weapon-side reference changes: W * weaponFrame = H * handFrame,
// where W/H are the sampled owner's weapon/hand transforms and handFrame is the joint's actual unchanged frame.
// The engine adapter rebuilds each verified joint once at takeover to obtain unambiguous scaled frames. Updates
// then need no guessed socket, weapon teleport, repeated joint recreation, limit change or independent weapon drive.
#pragma once
#include "sync/hsm_pose_codec.h"
#include "sync/hsm_standin.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace hsm::sync {

// A constraint reference frame: position plus its X and Y axes in the connected body's coordinates. Both input
// axes must be finite, approximately unit length and perpendicular; the third axis is their right-handed cross.
struct GripFrame {
    V3 pos{};
    V3 primary{1, 0, 0};
    V3 secondary{0, 1, 0};
};

// Both clavicles, upper arms, forearms and hands. A local grab or lodged blade anywhere on either arm takes priority.
inline constexpr uint32_t kTwoHandArmBones = ((uint32_t{1} << 17) - 1u) & ~((uint32_t{1} << 9) - 1u);

// These stock weapons are one rigid piece: ModularWeaponBP welds their Grip, Blade and Spike parts to BaseMesh, and no joint holds
// them. The generic multipart reader still streams a "head" for them; that metadata alone does not make them articulated.
// Up to 0.8.0-pre30 only Spear_A/B and WarStaff_A/B were listed, so billhooks, halberds, staffs and the other spears were refused
// and played on the grip path. Keep this an exact-class list: a multipart weapon not on it stays on the grip path, which is most
// axes, tool axes and plain ModularWeaponBP weapons too. They can join once this path has been shown to work in game.
inline constexpr std::array<uint32_t, 15> kRigidTwoHandClasses = {
    WeaponClassHash("ModularWeaponBP_Spear_A_C"),    WeaponClassHash("ModularWeaponBP_Spear_B_C"),
    WeaponClassHash("ModularWeaponBP_Spear_C_C"),    WeaponClassHash("ModularWeaponBP_Spear_D_C"),
    WeaponClassHash("ModularWeaponBP_WarStaff_A_C"), WeaponClassHash("ModularWeaponBP_WarStaff_B_C"),
    WeaponClassHash("ModularWeaponBP_Staff_C"),      WeaponClassHash("ModularWeaponBP_Billhook_A_C"),
    WeaponClassHash("ModularWeaponBP_Billhook_B_C"), WeaponClassHash("ModularWeaponBP_Billhook_C_C"),
    WeaponClassHash("ModularWeaponBP_Billhook_D_C"), WeaponClassHash("ModularWeaponBP_Halberd_A_C"),
    WeaponClassHash("ModularWeaponBP_Halberd_B_C"),  WeaponClassHash("ModularWeaponBP_Halberd_C_C"),
    WeaponClassHash("ModularWeaponBP_Halberd_D_C"),
};
inline constexpr bool IsRigidTwoHandWeapon(uint32_t classHash) {
    for (uint32_t rigid : kRigidTwoHandClasses) if (classHash == rigid) return true;
    return false;
}

// May the pose carry this weapon? One piece, or a multipart weapon known to be rigid.
inline constexpr bool TwoHandWeaponAccepted(bool multipart, uint32_t classHash) {
    return !multipart || IsRigidTwoHandWeapon(classHash);
}

// The pose holds weapon 0 by its two hand joints. A velocity on its welded head would reach the whole weapon and pull against them.
inline constexpr bool HeadServoed(bool headDrive, bool twoHandPose, int weapon) {
    return headDrive && !(twoHandPose && weapon == 0);
}

// How a stand-in's hand joint holds its weapon (standin_rig.cpp LockGrip). The game's own off-hand hold slides along the
// shaft and turns up to 36 degrees, and attaching it lets the right hand slide as well (Willie_BP Event Slide Right Hand
// Along Shaft); only the game's ReceiveTick steers those slides, and a stand-in's is blocked. While the pose writes both
// references from the owner every tick, nothing is left for a slide to do: both joints lock, or the hands float about the shaft.
// standin_two_hand_lock=0 keeps the game's off-hand hold under the pose too.
enum class GripHold { Locked, GameOffhand };
inline constexpr GripHold HoldFor(int hand, bool offhandJoint, bool poseOwnsJoint, bool lockToPose) {
    if (poseOwnsJoint && lockToPose) return GripHold::Locked;
    return hand == 1 && offhandJoint ? GripHold::GameOffhand : GripHold::Locked;
}

struct TwoHandPosePolicy {
    bool enabled = false;
    bool gripLock = false;
    // Verified live weapon/hand bodies and both joints, with ordering and active, scale-adjusted frames known.
    // The sampled transforms are rigid world transforms; reference positions use world units in each body's axes.
    bool localReady = false;
    bool localFlail = false;        // a real chain here: a skeletal one, or Link parts
    bool localMultipart = false;    // simulating parts of any kind here
    bool localLeftWeapon = false;
    uint32_t localClassHash = 0;
    uint32_t pinnedWeapons = 0;
    uint32_t heldBones = 0;
    uint32_t validBones = 0;
};

enum class TwoHandPoseReason { Ready, Disabled, FreeGrip, NotTwoHanded, WeaponMismatch, Flail, NotListed, Pinned, HeldArm,
                               MissingBodies, NotReady, InvalidPose };

inline const char* TwoHandPoseReasonName(TwoHandPoseReason reason) {
    switch (reason) {
        case TwoHandPoseReason::Ready: return "ready";
        case TwoHandPoseReason::Disabled: return "disabled";
        case TwoHandPoseReason::FreeGrip: return "free-grip";
        case TwoHandPoseReason::NotTwoHanded: return "not-two-handed";
        case TwoHandPoseReason::WeaponMismatch: return "weapon-mismatch";
        case TwoHandPoseReason::Flail: return "flail";
        case TwoHandPoseReason::NotListed: return "not-listed";
        case TwoHandPoseReason::Pinned: return "pinned";
        case TwoHandPoseReason::HeldArm: return "held-arm";
        case TwoHandPoseReason::MissingBodies: return "missing-bodies";
        case TwoHandPoseReason::NotReady: return "not-ready";
        case TwoHandPoseReason::InvalidPose: return "invalid-pose";
    }
    return "invalid-pose";
}

namespace twohanddetail {
inline bool Finite(V3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline bool Rotation(Q4 q) {
    const double n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::isfinite(n) && n > 1e-12;
}
inline bool Frame(const GripFrame& f) {
    constexpr double epsilon = 1e-3;   // reflected axes are float vectors; permit rounding, not a different basis
    return Finite(f.pos) && Finite(f.primary) && Finite(f.secondary) &&
           std::fabs(Dot(f.primary, f.primary) - 1.0) <= epsilon &&
           std::fabs(Dot(f.secondary, f.secondary) - 1.0) <= epsilon &&
           std::fabs(Dot(f.primary, f.secondary)) <= epsilon;
}
}  // namespace twohanddetail

// After SetConstrainedComponents rebuilds a joint, its stored reference positions are divided by the constraint
// component's scale; creation multiplies them back into the active physics frames. Its active reference setters
// take those world-unit rigid offsets directly. This conversion is valid only immediately after that rebuild,
// before any reference setters: stored positions otherwise cannot reveal which scale the active joint used.
// Support positive uniform component scale (ordinary fighter heights); reject skewed/mirrored or tiny scales.
// Match the engine's float GetConstraintScale() and minimum axis, including its conversion from double vectors.
// Both output frames remain unchanged on failure, including finite inputs whose scaled positions overflow.
inline bool ScaleGripFrames(const std::array<GripFrame, 2>& stored, V3 componentScale,
                            std::array<GripFrame, 2>& active) {
    using namespace twohanddetail;
    if (!Finite(componentScale)) return false;
    const double minimum = std::min({componentScale.x, componentScale.y, componentScale.z});
    const double maximum = std::max({componentScale.x, componentScale.y, componentScale.z});
    if (minimum < 0.01 || maximum > std::numeric_limits<float>::max() ||
        maximum - minimum > minimum * 1e-4) return false;
    const double scale = static_cast<float>(minimum);
    auto result = stored;
    for (GripFrame& frame : result) {
        if (!Frame(frame)) return false;
        frame.pos = Vec3Scale(frame.pos, scale);
        if (!Finite(frame.pos)) return false;
    }
    active = result;
    return true;
}

inline TwoHandPoseReason TwoHandPoseStatus(const PoseFrame& owner, const TwoHandPosePolicy& p) {
    using namespace twohanddetail;
    if (!p.enabled) return TwoHandPoseReason::Disabled;
    if (!p.gripLock) return TwoHandPoseReason::FreeGrip;
    if (!owner.grip.offhand || owner.weapons[1].present || p.localLeftWeapon) return TwoHandPoseReason::NotTwoHanded;
    const WeaponSample& w = owner.weapons[0];
    if (!w.present || w.hand != 0 || !w.classHash || w.classHash != p.localClassHash) return TwoHandPoseReason::WeaponMismatch;
    if (p.localFlail) return TwoHandPoseReason::Flail;
    if (!TwoHandWeaponAccepted(w.hasHead || p.localMultipart, w.classHash)) return TwoHandPoseReason::NotListed;
    if (p.pinnedWeapons) return TwoHandPoseReason::Pinned;
    if (p.heldBones & kTwoHandArmBones) return TwoHandPoseReason::HeldArm;
    if ((p.validBones & kTwoHandArmBones) != kTwoHandArmBones) return TwoHandPoseReason::MissingBodies;
    if (!p.localReady) return TwoHandPoseReason::NotReady;
    if (!Finite(w.pos) || !Rotation(w.rot)) return TwoHandPoseReason::InvalidPose;
    for (int bone : {kBoneHandR, kBoneHandL})
        if (!Finite(owner.bones[bone].pos) || !Rotation(owner.bones[bone].rot)) return TwoHandPoseReason::InvalidPose;
    return TwoHandPoseReason::Ready;
}

inline bool TwoHandPoseAllowed(const PoseFrame& owner, const TwoHandPosePolicy& p) {
    return TwoHandPoseStatus(owner, p) == TwoHandPoseReason::Ready;
}

// Convert a validated reference basis to an FQuat-compatible unit quaternion. Remove only float-rounding skew;
// malformed/degenerate bases return false and leave out unchanged. Useful when invoking SetConstraintReferenceFrame.
inline bool GripFrameRotation(const GripFrame& frame, Q4& out) {
    using namespace twohanddetail;
    if (!Frame(frame)) return false;
    const V3 x = Vec3Scale(frame.primary, 1.0 / std::sqrt(Dot(frame.primary, frame.primary)));
    const V3 y0 = Vec3Sub(frame.secondary, Vec3Scale(x, Dot(x, frame.secondary)));
    const V3 y = Vec3Scale(y0, 1.0 / std::sqrt(Dot(y0, y0)));
    const V3 z = Cross(x, y);
    Q4 q;
    const double trace = x.x + y.y + z.z;
    if (trace > 0.0) {
        const double s = 2.0 * std::sqrt(trace + 1.0);
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25 * s};
    } else if (x.x > y.y && x.x > z.z) {
        const double s = 2.0 * std::sqrt(1.0 + x.x - y.y - z.z);
        q = {0.25 * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const double s = 2.0 * std::sqrt(1.0 + y.y - x.x - z.z);
        q = {(y.x + x.y) / s, 0.25 * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const double s = 2.0 * std::sqrt(1.0 + z.z - x.x - y.y);
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25 * s, (x.y - y.x) / s};
    }
    if (!Rotation(q)) return false;
    out = QuatNormalize(q);
    return true;
}

// Arrays are [right, left]. Preserve each hand-side reference verbatim and derive the matching weapon-side frame
// from this same sampled pose, including slides, switched hands, alternate/reverse grips and mordhau. A missing
// owner offhand flag suspends updates immediately; any native attach/release hysteresis belongs to the caller.
// On any invalid input neither output frame is changed, so callers never apply a half-updated pair.
inline bool BuildTwoHandPose(const PoseFrame& owner, const TwoHandPosePolicy& policy,
                             const std::array<GripFrame, 2>& handFrames, std::array<GripFrame, 2>& weaponFrames,
                             TwoHandPoseReason* reason = nullptr) {
    using namespace twohanddetail;
    const TwoHandPoseReason status = TwoHandPoseStatus(owner, policy);
    if (reason) *reason = status;
    if (status != TwoHandPoseReason::Ready) return false;
    auto invalid = [&] { if (reason) *reason = TwoHandPoseReason::InvalidPose; return false; };
    const WeaponSample& weapon = owner.weapons[0];
    const Q4 inverseWeapon = QuatConj(QuatNormalize(weapon.rot));
    std::array<GripFrame, 2> result;
    constexpr int hands[2] = {kBoneHandR, kBoneHandL};
    for (size_t i = 0; i < result.size(); ++i) {
        const BoneSample& hand = owner.bones[hands[i]];
        const GripFrame& local = handFrames[i];
        if (!Frame(local)) return invalid();
        const Q4 rotation = QuatNormalize(hand.rot);
        const V3 anchor = Vec3Add(hand.pos, QuatRotateVec(rotation, local.pos));
        result[i].pos = QuatRotateVec(inverseWeapon, Vec3Sub(anchor, weapon.pos));
        result[i].primary = QuatRotateVec(inverseWeapon, QuatRotateVec(rotation, local.primary));
        result[i].secondary = QuatRotateVec(inverseWeapon, QuatRotateVec(rotation, local.secondary));
        if (!Frame(result[i])) return invalid();
    }
    weaponFrames = result;
    return true;
}

// Compare the read-back replica against exactly the sampled owner pose, including with the feature disabled.
// Errors: right hand uu, left hand uu, weapon root uu, weapon tip uu, weapon angle degrees. The caller verifies the
// local weapon has no flail chain and its tip was read successfully. Missing/mismatched samples leave errors intact.
// Tip error projects the observed local weapon geometry through the sampled owner's root/rotation. The packet's
// base/tip are not interpolated with that root, so comparing directly to owner.weapons[0].tip would mix sample times
// (and inherit its compressed-coordinate range). This metric measures pose error, not a different weapon's geometry.
inline bool MeasureTwoHandPose(const PoseFrame& owner, const FighterNow& now, uint32_t localClassHash, V3 localTip,
                               std::array<double, 5>& errors) {
    using namespace twohanddetail;
    TwoHandPosePolicy policy;
    policy.enabled = policy.gripLock = policy.localReady = true;
    policy.localClassHash = localClassHash;
    for (int i = 0; i < kPoseBones; ++i) if (now.valid[i]) policy.validBones |= uint32_t{1} << i;
    if (!TwoHandPoseAllowed(owner, policy) || !now.weaponValid[0] || now.weaponValid[1] ||
        !Finite(now.bones[kBoneHandR].pos) || !Finite(now.bones[kBoneHandL].pos) ||
        !Rotation(now.bones[kBoneHandR].rot) || !Rotation(now.bones[kBoneHandL].rot) ||
        !Finite(now.weapons[0].pos) || !Rotation(now.weapons[0].rot) || !Finite(localTip)) return false;
    const WeaponSample& w = owner.weapons[0];
    const Q4 localRotation = QuatNormalize(now.weapons[0].rot);
    const Q4 targetRotation = QuatNormalize(w.rot);
    const V3 tipInWeapon = QuatRotateVec(QuatConj(localRotation), Vec3Sub(localTip, now.weapons[0].pos));
    const V3 targetTip = Vec3Add(w.pos, QuatRotateVec(targetRotation, tipInWeapon));
    if (!Finite(tipInWeapon) || !Finite(targetTip)) return false;
    const std::array<double, 5> result = {
        Vec3Dist(owner.bones[kBoneHandR].pos, now.bones[kBoneHandR].pos),
        Vec3Dist(owner.bones[kBoneHandL].pos, now.bones[kBoneHandL].pos),
        Vec3Dist(w.pos, now.weapons[0].pos), Vec3Dist(targetTip, localTip),
        QuatAngleDeg(targetRotation, localRotation)};
    for (double value : result) if (!std::isfinite(value)) return false;
    errors = result;
    return true;
}

}  // namespace hsm::sync
