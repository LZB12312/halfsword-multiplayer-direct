#include "gameplay/standin_rig.h"
#include "combat/hsm_handloss.h"
#include "sync/hsm_grip.h"
#include "sync/hsm_pose_codec.h"
#include "sync/hsm_two_hand_update.h"
#include "util/hsm_names.h"
#include "log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace hsm::game {
using namespace hsm::ue;
using sync::Q4;
using sync::V3;

namespace {
struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };

// Willie_BP_C fields Neutralise writes every frame: HSMP main.lua PX.TONUS, neutralise_bp (Head Tonus, the root
// constraint powers), PAIN_ZERO, MUSCLE_FLAGS and PAIN_FALSE, plus the plan's damage backstops (Invulnerable, Force
// Disable Dismemberment).
constexpr const char* kStandInZeroDoubleFields[] = {
    "All Body Tonus", "Upper Body Tonus", "Arm R Tonus", "Arm L Tonus", "Leg R Tonus", "Leg L Tonus", "Muscle Power",
    "Head Tonus", "Root Angular Constraint Power", "Root Linear Constraint Power",
    "Pain Shock Rate", "Pain Upper Body", "Pain Lower Body", "Pain Neck", "Pain Head", "Pain Arm R", "Pain Arm L",
    "Pain Leg R", "Pain Leg L", "Pain Grab Rate", "Pain L Arm Alpha", "Pain R Arm Alpha"};
constexpr const char* kStandInTrueBoolFields[] = {
    "Block Body Muscles", "Block Upper Leg Muscles", "Block Lower Leg Muscles", "Block Shoulder Muscles",
    "Block Arm Muscles", "Invulnerable", "Force Disable Dismemberment"};
constexpr const char* kStandInFalseBoolFields[] = {"Pain Shock", "Bend Over Extreme"};

constexpr double kOffhandRetryMs = 500.0;   // a failed off-hand attach is tried again this often, not every frame
// The owner's offhand flag drops for a frame or two in fast swings: the off hand lets go only after it has been off this
// long, and takes the shaft only after it has been on this long, so the joint does not flap at the stream's rate.
constexpr double kOffhandAttachMs = 100.0, kOffhandReleaseMs = 250.0;
// While both hands hold the shaft a re-seat of the right hand breaks the off hand's joint and swings a long weapon:
// only a seat this far off is fixed then (one-handed: hsm_held.h's thresholds).
constexpr double kHeldTwoHandDeg = 60.0, kHeldTwoHandUu = 45.0;   // 0.8.0-pre7: 45 (was 30): fewer re-seats on a polearm (#74, #79)
// The off hand just took or left the shaft: the weapon swings to its new balance for a moment, which is no reason to
// re-seat the right hand (ApplyHeld).
constexpr double kOffhandSettleMs = 250.0;
constexpr const char* kWeaponFields[sync::kPoseWeapons] = {"Weapon R", "Weapon L"};   // slot = hand
constexpr const char* kGripFields[2] = {"PhysicsConstraint R Hand", "PhysicsConstraint L Hand"};
// The sheath slots (SheathSlots_Enum order, standin_slots.cpp SlotActor): a weapon the game puts in one is anchored on the
// next pass (Neutralise).
constexpr const char* kSlotFields[FighterRig::kSheathSlots] = {"Weapon Slot R 1", "Weapon Slot R 2", "Weapon Slot Back",
                                                               "Weapon Slot L 1", "Weapon Slot L 2"};
// The hands' grip variables (docs/TWO_PLAYER_TEST_6.md section 1): the fingers follow the two grip types and the offhand flag in
// ABP_Willie; the other flags move the left hand's attach point. Order = GripState: offhand, alt, mordhau, switched, reverse.
constexpr const char* kGripBoolFields[] = {"L Hand In Offhand Attached", "R Alt Grip", "Mordhau Grip", "2H Switched Hands (Temp)",
                                            "R Hand Reverse Grip"};
static_assert(sizeof(kGripBoolFields) / sizeof(kGripBoolFields[0]) == 5, "GripState has five flags");
constexpr const char* kRootConstraints[] = {"PhysicsConstraint Main", "PhysicsConstraint_Z"};
constexpr const char* kHiddenParts[] = {"DriverSkeleton", "CharacterMesh_Head"};   // standin_parts_off (SetStandInPartsOff)
constexpr const char* kHandles[] = {"PhysicsHandle GetUp", "PhysicsHandle LowerBody", "PhysicsHandle UpperBody"};
constexpr const char* kFistsPrefix = "Weapon_Fists";   // an empty hand's pseudo weapon, kinematic at the world origin
constexpr int kHandBone[2] = {sync::kBoneHandR, sync::kBoneHandL};

constexpr double kHeldMaxUu = 150.0;          // a weapon (root to tip) farther than this from its hand is not held
constexpr double kBoneComMaxUu = 80.0;        // a centre of mass this far from its bone origin is a bad read
constexpr double kWeaponComMaxUu = 300.0;
constexpr int kComTries = 120;                // frames a bad centre-of-mass read is retried before the origin is used
constexpr const char* kFlailTag = "Flail";    // ModularWeaponBP's chain component (SetAllBodiesBelowSimulatePhysics('Joint1'))
constexpr int kHeadScanEvery = 15;            // frames between searches for a flail chain's simulating head while none is found
constexpr int32_t kFlailMaxBones = 64;
constexpr double kFullEarlyEveryMs = 500.0, kFullEarlyForMs = 6000.0, kFullEveryMs = 2000.0;
constexpr double kCapsuleTeleportUu = 150.0;  // HSMP main.lua:133 TELEPORT_DIST
// ECollisionChannel: WorldStatic 0, WorldDynamic 1, Pawn 2, Visibility 3, Camera 4, PhysicsBody 5. 0.8.0-pre7: PhysicsBody
// was 4 (the camera channel), so a chain "through fighters' bodies" still met their armour.
constexpr uint8_t kWorldStatic = 0, kWorldDynamic = 1, kPawn = 2, kPhysicsBody = 5;
constexpr uint8_t kBodyCollision = 16;        // the game's BodyCollision channel (GameTraceChannel3): all Box1 still blocks
constexpr uint8_t kIgnore = 0;                // ECollisionResponse::ECR_Ignore
constexpr uint8_t kNoCollision = 0;           // ECollisionEnabled::NoCollision
constexpr uint8_t kLocked = 2;                 // ELinearConstraintMotion::LCM_Locked, EAngularConstraintMotion::ACM_Locked
bool g_gripLock = true;                        // standin_grip (SetStandInGripLock)
bool g_partsOff = false;                       // standin_parts_off (SetStandInPartsOff)
bool g_twoHandPose = true;                     // standin_two_hand_pose (receiver only)
bool g_twoHandLock = true;                     // standin_two_hand_lock (receiver only)
bool g_twoHandAudit = false;                   // Help improve only
constexpr uint8_t kLimited = 1;               // ELinearConstraintMotion::LCM_Limited, EAngularConstraintMotion::ACM_Limited
constexpr double kOffhandAngleDeg = 36.0;      // the game's MapRangeClamped(Soft Attach Dot 0.9, 0.5, 1, 180, 0), Willie_BP.cpp:18200
constexpr uint8_t kFree = 0;                  // ELinearConstraintMotion::LCM_Free, EAngularConstraintMotion::ACM_Free
constexpr uint8_t kWorldSpace = 0;            // ERelativeTransformSpace::RTS_World
constexpr uint8_t kAlwaysTickPose = 0;        // EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones

double SteadyMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

V3 ToV3(const FVector& v) { return {v.x, v.y, v.z}; }
FQuat ToFQuat(const Q4& q) { return {q.x, q.y, q.z, q.w}; }
// FRotator::Quaternion (UE 5, degrees).
Q4 RotatorQuat(double pitch, double yaw, double roll) {
    constexpr double kHalf = 3.14159265358979323846 / 360.0;
    const double sp = std::sin(pitch * kHalf), cp = std::cos(pitch * kHalf), sy = std::sin(yaw * kHalf), cy = std::cos(yaw * kHalf);
    const double sr = std::sin(roll * kHalf), cr = std::cos(roll * kHalf);
    return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
}
bool WorldTransform(UObject* c, FTransform& out) {
    Call t(c, "K2_GetComponentToWorld");
    if (!t.invoke()) return false;
    out = t.get<FTransform>("ReturnValue");
    return true;
}
bool SetWorldTransform(UObject* c, const FTransform& t) {
    return Call(c, "K2_SetWorldTransform").set("NewTransform", t).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
}
FVector ToF(const V3& v) { return {v.x, v.y, v.z}; }
Q4 ToQ4(const FQuat& q) { return {q.x, q.y, q.z, q.w}; }
V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
double Len(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
V3 Unrotate(const Q4& q, const V3& v) { return sync::detail::QRot(sync::detail::QConj(q), v); }

// Distance from p to the segment a-b.
double SegmentDistance(const V3& p, const V3& a, const V3& b) {
    const V3 ab = Sub(b, a), ap = Sub(p, a);
    const double len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    const double t = len2 > 1e-9 ? std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2, 0.0, 1.0) : 0.0;
    return Len(Sub(ap, V3{ab.x * t, ab.y * t, ab.z * t}));
}

UObject* ObjField(UObject* o, const char* name) { UObject** p = Field<UObject*>(o, name); return p ? *p : nullptr; }
bool Live(const UObject* o, int32_t index) { return o && index >= 0 && ObjectAt(index) == o; }

bool UniformGripScale(const V3& scale) {
    std::array<sync::GripFrame, 2> ignored;
    return sync::ScaleGripFrames({}, scale, ignored);
}

bool ParamSize(UFunction* fn, const char* name, size_t size) {
    const FProperty* p = fn ? FindProperty(fn, name) : nullptr;
    return p && PropertySize(p) == static_cast<int32_t>(size);
}

// Reflected FConstraintInstance members, never native layout offsets. The hand side is a socket frame, not in
// general the hand bone origin. Stored positions need scale conversion immediately after the adapter rebuilds a
// joint; later reference setters store active offsets directly, so an arbitrary native snapshot is ambiguous.
bool ReadGripFrames(UObject* joint, std::array<sync::GripFrame, 2>& frames) {
    const FProperty* instance = FindProperty(ClassOf(joint), "ConstraintInstance");
    UObject* definition = FindStruct("ConstraintInstance");
    if (!instance || !definition || PropertyType(instance) != "StructProperty") return false;
    const FProperty* members[] = {StructMember(definition, "Pos1"), StructMember(definition, "PriAxis1"),
                                 StructMember(definition, "SecAxis1"), StructMember(definition, "Pos2"),
                                 StructMember(definition, "PriAxis2"), StructMember(definition, "SecAxis2")};
    const int32_t size = PropertySize(instance);
    const auto* data = reinterpret_cast<const uint8_t*>(joint) + PropertyOffset(instance);
    V3 values[6];
    for (size_t i = 0; i < 6; ++i) {
        const FProperty* p = members[i];
        if (!p) return false;
        const int32_t offset = PropertyOffset(p), bytes = PropertySize(p);
        if (offset < 0 || bytes <= 0 || offset > size - bytes) return false;
        if (bytes == sizeof(FVector)) {
            FVector value;
            std::memcpy(&value, data + offset, sizeof value);
            values[i] = ToV3(value);
        } else if (bytes == sizeof(float) * 3) {
            float value[3];
            std::memcpy(value, data + offset, sizeof value);
            values[i] = {value[0], value[1], value[2]};
        } else return false;
    }
    frames = {{{values[0], values[1], values[2]}, {values[3], values[4], values[5]}}};
    Q4 rotation;
    return sync::GripFrameRotation(frames[0], rotation) && sync::GripFrameRotation(frames[1], rotation);
}

// A Willie field of the given reflected type, or nullptr (logged) when it is missing or of another type.
FProperty* TypedField(const UClass* cls, const char* name, const char* type) {
    FProperty* p = FindProperty(cls, name);
    if (!p) { HSM_LOG("rig: Willie has no field '%s'", name); return nullptr; }
    const std::string t = PropertyType(p);
    if (t != type) { HSM_LOG("rig: Willie field '%s' is %s, not %s", name, t.c_str(), type); return nullptr; }
    return p;
}

// Teleports a component to a world location (HSMP main.lua snap_mesh).
void MoveTo(UObject* c, const V3& at) {
    Call(c, "K2_SetWorldLocation").set("NewLocation", ToF(at)).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
}
void MoveBy(UObject* c, const V3& d) {
    Call at(c, "K2_GetComponentLocation");
    if (at.invoke()) MoveTo(c, Add(ToV3(at.get<FVector>("ReturnValue")), d));
}

// Collision off for a component, asked first: a change rebuilds its physics state (HSMP main.lua:5037).
void CollisionOff(UObject* c) {
    if (!c) return;
    Call get(c, "GetCollisionEnabled");
    if (get.invoke() && get.get<uint8_t>("ReturnValue") == kNoCollision) return;
    Call(c, "SetCollisionEnabled").set<uint8_t>("NewType", kNoCollision).invoke();
}

bool HasBone(UObject* mesh, const char* bone) {
    Call index(mesh, "GetBoneIndex");
    index.setName("BoneName", MakeName(bone));
    return index.invoke() && index.get<int32_t>("ReturnValue") >= 0;
}

// The weapon's flail chain: its SkeletalMeshComponent tagged "Flail", else (test 2026-10-25: every flail on both
// machines logged "flail no", the tag lookup found nothing) any of its SkeletalMeshComponents with a Joint1 bone, the
// bone the game simulates the chain below. nullptr for every other weapon.
UObject* FlailComponent(UObject* actor) {
    UClass* skel = FindClass("SkeletalMeshComponent");
    if (!skel) return nullptr;
    // The engine filled the returned array: its buffer is released through the property's own ClearValue (Call frees nothing).
    auto first = [](Call& c, bool joint) -> UObject* {
        UObject* found = nullptr;
        if (!c.invoke()) return nullptr;
        void* value = c.paramPtr("ReturnValue");
        const FObjArray* arr = static_cast<const FObjArray*>(value);
        if (arr && arr->data && arr->num > 0 && arr->num <= 64) {
            if (!joint) found = arr->data[0];
            else
                for (int32_t i = 0; i < arr->num && !found; ++i)
                    if (UObject* o = arr->data[i]; o && HasBone(o, "Joint1")) found = o;
        }
        if (FProperty* prop = FindProperty(c.function(), "ReturnValue"); prop && value) ClearPropertyValue(prop, value);
        return found;
    };
    Call byTag(actor, "GetComponentsByTag");
    byTag.set<UClass*>("ComponentClass", skel).setName("Tag", MakeName(kFlailTag));
    if (UObject* c = first(byTag, false)) return c;
    Call byClass(actor, "K2_GetComponentsByClass");
    byClass.set<UClass*>("ComponentClass", skel);
    return first(byClass, true);
}

// A flail's chain simulates only below Joint1, and the game switches it on again after every BaseMesh.SetSimulatePhysics
// (ModularWeaponBP Set_Up_Module:275, Refresh_Collisions:1657). The rig's own toggles of the root (ApplySeat, ApplyHeld)
// must do the same, or the chain hangs stiff from the head of a re-seated flail.
void RearmFlail(UObject* weapon) {
    UObject* const flail = weapon ? FlailComponent(weapon) : nullptr;
    if (!flail) return;
    Call below(flail, "SetAllBodiesBelowSimulatePhysics");
    below.setName("InBoneName", MakeName("Joint1")).set<bool>("bNewSimulate", true).set<bool>("bIncludeSelf", true);
    if (!below.invoke()) { HSM_LOG("rig: %s: flail chain NOT re-armed", ObjectName(weapon).c_str()); return; }
    // The chain wakes with whatever it carried while the root was off: it starts still, as Shift leaves it.
    Call(flail, "SetAllPhysicsLinearVelocity").set("NewVel", FVector{}).set<bool>("bAddToCurrent", false).invoke();
    Call(flail, "SetAllPhysicsAngularVelocityInDegrees").set("NewAngVel", FVector{}).set<bool>("bAddToCurrent", false).invoke();
}

int ObjectType(UObject* c) {
    if (!c) return -1;
    Call t(c, "GetCollisionObjectType");
    return t.invoke() ? int(t.get<uint8_t>("ReturnValue")) : -1;
}
}  // namespace

// ---- binding ---------------------------------------------------------------------------------------------------------

bool FighterRig::Bind(UObject* willie) {
    if (willie && willie == willie_ && StillValid()) return RestoreDeferredTwoHandPoses(mesh_, meshIndex_);
    UObject* const quiet = failLogged_;   // a fighter that keeps failing (retried every frame) is logged once
    Unbind();
    failLogged_ = quiet;
    const bool log = willie != failLogged_;
    UObject* mesh = willie ? ObjField(willie, "Mesh") : nullptr;
    // An old rig may have been destroyed while reflection was unavailable. Never snapshot its override as the
    // native reference when this fighter binds again; the saved reference must be restored first.
    const bool restored = RestoreDeferredTwoHandPoses(mesh, mesh ? ObjectIndex(mesh) : -1);
    if (!restored) return false;
    if (!mesh) return false;
    for (int i = 0; i < sync::kPoseBones; ++i) {
        const FName n = MakeName(sync::kPoseBoneNames[i]);
        Call c(mesh, "GetBoneIndex");
        c.setName("BoneName", n);
        if (!c.invoke() || c.get<int32_t>("ReturnValue") < 0) {
            if (log) HSM_LOG("rig: %s has no bone %s", ObjectName(willie).c_str(), sync::kPoseBoneNames[i]);
            failLogged_ = willie;
            return false;
        }
        bones_[i] = n;
    }
    const UClass* mc = ClassOf(mesh);
    socket_ = PreparedCall(mc, "GetSocketTransform", {"InSocketName", "TransformSpace", "ReturnValue"});
    linVelAt_ = PreparedCall(mc, "GetPhysicsLinearVelocityAtPoint", {"Point", "BoneName", "ReturnValue"});
    linVel_ = PreparedCall(mc, "GetPhysicsLinearVelocity", {"BoneName", "ReturnValue"});
    angVel_ = PreparedCall(mc, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    setLin_ = PreparedCall(mc, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
    setAng_ = PreparedCall(mc, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
    com_ = PreparedCall(mc, "GetCenterOfMass", {"BoneName", "ReturnValue"});
    const UClass* wc = ClassOf(willie);
    placeActor_ = PreparedCall(wc, "K2_SetActorLocation", {"NewLocation", "bSweep", "bTeleport"});
    actorLoc_ = PreparedCall(wc, "K2_GetActorLocation", {"ReturnValue"});
    turnActor_ = PreparedCall(wc, "K2_SetActorRotation", {"NewRotation", "bTeleportPhysics"});   // optional (PlaceCapsule yaw)
    if (!socket_.valid() || !linVelAt_.valid() || !linVel_.valid() || !angVel_.valid() || !setLin_.valid() || !setAng_.valid() || !com_.valid() ||
        !placeActor_.valid() || !actorLoc_.valid()) {
        if (log) HSM_LOG("rig: %s: per-frame engine calls did not resolve", ObjectName(willie).c_str());
        Unbind();
        failLogged_ = willie;
        return false;
    }
    for (int w = 0; w < sync::kPoseWeapons; ++w)
        if (FProperty* p = TypedField(wc, kWeaponFields[w], "ObjectProperty")) weaponOffset_[w] = PropertyOffset(p);
    for (int h = 0; h < 2; ++h)
        if (FProperty* p = TypedField(wc, kGripFields[h], "ObjectProperty")) gripOffset_[h] = PropertyOffset(p);
    for (int k = 0; k < kSheathSlots; ++k)
        if (FProperty* p = TypedField(wc, kSlotFields[k], "ObjectProperty")) slotOffset_[k] = PropertyOffset(p);
    if (uint8_t* p = Field<uint8_t>(willie, "R_GripType_Current")) gripTypeOffset_[0] = int32_t(p - reinterpret_cast<uint8_t*>(willie));
    if (uint8_t* p = Field<uint8_t>(willie, "L_GripType_Current")) gripTypeOffset_[1] = int32_t(p - reinterpret_cast<uint8_t*>(willie));
    for (int i = 0; i < 5; ++i) gripFlag_[i] = TypedField(wc, kGripBoolFields[i], "BoolProperty");
    for (const char* f : kStandInZeroDoubleFields)
        if (FProperty* p = TypedField(wc, f, "DoubleProperty")) zeroDoubles_.push_back(PropertyOffset(p));
    for (const char* f : kStandInTrueBoolFields)
        if (FProperty* p = TypedField(wc, f, "BoolProperty")) trueBools_.push_back(p);
    for (const char* f : kStandInFalseBoolFields)
        if (FProperty* p = TypedField(wc, f, "BoolProperty")) falseBools_.push_back(p);
    willie_ = willie;
    willieIndex_ = ObjectIndex(willie);
    mesh_ = mesh;
    meshIndex_ = ObjectIndex(mesh);
    HSM_LOG("rig: bound %s (%d bones; neutralise writes %zu doubles, %zu+%zu flags)", ObjectName(willie).c_str(),
            sync::kPoseBones, zeroDoubles_.size(), trueBools_.size(), falseBools_.size());
    return true;
}

void FighterRig::Unbind() {
    if (!RestoreTwoHandPose()) {
        auto& deferred = DeferredTwoHandRestores();
        size_t retained = 0;
        for (TwoHandJoint& saved : twoHandJoints_) if (saved.changed) {
            deferred.push_back(std::move(saved));
            ++retained;
        }
        if (g_twoHandAudit) HSM_LOG("rig: deferred native two-hand reference restoration for %zu joint(s)", retained);
    }
    *this = FighterRig();   // the bind-failure log mark too: Bind keeps it itself
}

bool FighterRig::StillValid() const { return Live(willie_, willieIndex_) && Live(mesh_, meshIndex_); }

UObject* FighterRig::FieldAt(int32_t offset) const {
    return offset >= 0 ? *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(willie_) + offset) : nullptr;
}

void FighterRig::RefreshWeapon(int w) {
    Weapon& wp = weapons_[w];
    UObject* actor = FieldAt(weaponOffset_[w]);
    if (w == 1 && actor && actor == FieldAt(weaponOffset_[0])) actor = nullptr;   // one weapon in both hands: slot 0 has it
    if (actor == wp.actor && (!actor || Live(actor, wp.index))) return;
    RestoreTwoHandPose();   // before the old held weapon is re-armed or the new one is cached
    // Let go while neutralised (a stand-in's hand opened; my own fighter's while another machine held it): the held-weapon
    // policy (no gravity, blind to the world) would stay on it for good, floating where it was (#145, #138-#141).
    if (wp.actor && actor != wp.actor && !FieldAt(weaponOffset_[w])) CheckHandLoss(w, wp);   // the variable went null
    if (boundAtMs_ >= 0.0 && wp.actor && actor != wp.actor) RearmDropped(wp);
    wp = Weapon{};
    weaponChanged_ = true;
    gripFreed_[0] = gripFreed_[1] = nullptr;   // a re-made grip comes with its limits locked again
    gripLoose_[0] = gripLoose_[1] = nullptr;
    if (!actor) return;
    wp.actor = actor;
    wp.index = ObjectIndex(actor);
    const std::string cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(actor)));
    wp.classHash = sync::WeaponClassHash(cls.c_str());
    wp.fists = StartsWithNameAscii(cls, kFistsPrefix);
    if (wp.fists) return;
    Call rc(actor, "K2_GetRootComponent");
    UObject* root = rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr;
    if (!root) { HSM_LOG("rig: %s slot %d (%s) has no root component", ObjectName(willie_).c_str(), w, cls.c_str()); return; }
    const UClass* c = ClassOf(root);
    wp.toWorld = PreparedCall(c, "K2_GetComponentToWorld", {"ReturnValue"});
    wp.simulating = PreparedCall(c, "IsSimulatingPhysics", {"BoneName", "ReturnValue"});
    wp.linVelAt = PreparedCall(c, "GetPhysicsLinearVelocityAtPoint", {"Point", "BoneName", "ReturnValue"});
    wp.angVel = PreparedCall(c, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    wp.setLin = PreparedCall(c, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
    wp.setAng = PreparedCall(c, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
    wp.com = PreparedCall(c, "GetCenterOfMass", {"BoneName", "ReturnValue"});
    if (!wp.toWorld.valid() || !wp.simulating.valid() || !wp.linVelAt.valid() || !wp.angVel.valid() || !wp.setLin.valid() ||
        !wp.setAng.valid() || !wp.com.valid()) {
        HSM_LOG("rig: %s slot %d (%s): root %s is not a physics body", ObjectName(willie_).c_str(), w, cls.c_str(),
                ObjectName(root).c_str());
        return;
    }
    wp.root = root;
    wp.rootIndex = ObjectIndex(root);
    wp.base = ObjField(actor, "Root Scene");
    wp.tip = ObjField(actor, "TippyTipScene");
    if (wp.base) wp.baseIndex = ObjectIndex(wp.base);
    if (wp.tip) wp.tipIndex = ObjectIndex(wp.tip);
    if (wp.base) wp.baseLoc = PreparedCall(ClassOf(wp.base), "K2_GetComponentLocation", {"ReturnValue"});
    if (wp.tip) wp.tipLoc = PreparedCall(ClassOf(wp.tip), "K2_GetComponentLocation", {"ReturnValue"});
    if (UObject* flail = FlailComponent(actor)) {
        const UClass* fc = ClassOf(flail);
        wp.headSocket = PreparedCall(fc, "GetSocketTransform", {"InSocketName", "TransformSpace", "ReturnValue"});
        wp.headSim = PreparedCall(fc, "IsSimulatingPhysics", {"BoneName", "ReturnValue"});
        wp.headLinVelAt = PreparedCall(fc, "GetPhysicsLinearVelocityAtPoint", {"Point", "BoneName", "ReturnValue"});
        wp.headAngVel = PreparedCall(fc, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
        wp.headSetLin = PreparedCall(fc, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
        wp.headSetAng = PreparedCall(fc, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
        if (wp.headSocket.valid() && wp.headSim.valid() && wp.headLinVelAt.valid() && wp.headAngVel.valid() && wp.headSetLin.valid() &&
            wp.headSetAng.valid()) {
            wp.flail = flail;
            wp.flailIndex = ObjectIndex(flail);
        } else {
            HSM_LOG("rig: %s: flail chain %s found but its physics functions are missing", cls.c_str(), ObjectName(flail).c_str());
        }
    }
    HSM_LOG("rig: %s slot %d = %s (root %s, base %s, tip %s, flail %s)", ObjectName(willie_).c_str(), w, cls.c_str(),
            ObjectName(root).c_str(), wp.base ? "yes" : "no", wp.tip ? "yes" : "no", wp.flail ? ObjectName(wp.flail).c_str() : "no");
}

// #249: the player's own real weapon left its hand with the grab still pressed and none of the game's release paths run
// (combat::LeftHandUnasked). A log line only, on the player's own live fighter; it changes nothing. "frame" is the time
// since this fighter was last read (a hitch shows as a long one).
void FighterRig::CheckHandLoss(int w, const Weapon& wp) {
    if (!sendsFrames_ || boundAtMs_ >= 0.0 || wp.fists || !wp.actor || !Live(wp.actor, wp.index)) return;
    combat::HandLossInput in;
    in.ownLive = true;
    in.wasHeld = true;
    in.gone = true;
    if (w == 0) {
        GetBool(willie_, "R Grab Pressed", in.grabPressed);
        GetBool(willie_, "Dropping R", in.dropping);
    } else {
        GetBool(willie_, "L Grab Pressed", in.grabPressed);
        GetBool(willie_, "Dropping L", in.dropping);
    }
    for (int k = 0; k < kSheathSlots; ++k)
        if (FieldAt(slotOffset_[k]) == wp.actor) in.sheathed = true;
    Call dying(wp.actor, "IsActorBeingDestroyed");
    in.destroyed = dying.invoke() && dying.get<bool>("ReturnValue");
    if (!combat::LeftHandUnasked(in)) return;
    HSM_LOG("rig: own %s left the hand (frame %.0f ms, no release pressed)", ObjectName(wp.actor).c_str(), readGapMs_);
}

// ---- reading ---------------------------------------------------------------------------------------------------------

bool FighterRig::ReadWeapon(int w, const V3& hand) {
    Weapon& wp = weapons_[w];
    wp.read = wp.haveTip = wp.farFromHand = false;
    if (!wp.root || !Live(wp.actor, wp.index) || !Live(wp.root, wp.rootIndex)) return false;
    wp.simulating.put(0, FName{});
    if (!wp.simulating.invoke(wp.root) || !wp.simulating.take<bool>(1)) return false;   // kinematic: it follows the hand
    if (!wp.toWorld.invoke(wp.root)) return false;
    const FTransform t = wp.toWorld.take<FTransform>(0);
    wp.pos = ToV3(t.translation);
    wp.rot = ToQ4(t.rotation);
    wp.scale = ToV3(t.scale);
    wp.basePos = wp.tipPos = wp.pos;
    if (Live(wp.base, wp.baseIndex) && wp.baseLoc.invoke(wp.base)) wp.basePos = ToV3(wp.baseLoc.take<FVector>(0));
    if (Live(wp.tip, wp.tipIndex) && wp.tipLoc.invoke(wp.tip)) { wp.tipPos = ToV3(wp.tipLoc.take<FVector>(0)); wp.haveTip = true; }
    // Held: some point between the root and the tip is within reach of the hand (a polearm is gripped mid-shaft). A
    // dropped weapon still in the field, or one parked at the world origin, is not.
    wp.farFromHand = SegmentDistance(hand, wp.pos, wp.tipPos) > kHeldMaxUu;
    if (wp.farFromHand) return false;
    wp.read = true;
    return true;
}

// The flail head: the chain's last simulating body (highest bone index; the chain runs parents first from Joint1). Searched
// for while none is found, since a sheathed chain is kinematic; a frame where it is kinematic sends no head.
bool FighterRig::ReadHead(Weapon& wp, sync::BoneSample& out) {
    if (wp.head) {   // a component chain (ScanParts)
        if (!Live(wp.head, wp.headIndex) || !Live(wp.actor, wp.index)) {   // the game made the chain again: found anew
            wp.parts.clear(); wp.partIndex.clear(); wp.head = nullptr; wp.headIndex = -1; wp.linked = false;
            return false;
        }
        wp.headSim.put(0, FName{});
        if (!wp.headSim.invoke(wp.head) || !wp.headSim.take<bool>(1)) return false;
        if (!wp.headToWorld.invoke(wp.head)) return false;
        const FTransform t = wp.headToWorld.take<FTransform>(0);
        out = sync::BoneSample{ToV3(t.translation), ToQ4(t.rotation), V3{}, V3{}};
        wp.headLinVelAt.put(0, t.translation).put(1, FName{});
        if (wp.headLinVelAt.invoke(wp.head)) out.vel = ToV3(wp.headLinVelAt.take<FVector>(2));
        wp.headAngVel.put(0, FName{});
        if (wp.headAngVel.invoke(wp.head)) out.angVelDeg = ToV3(wp.headAngVel.take<FVector>(1));
        return true;
    }
    if (!wp.flail || !Live(wp.flail, wp.flailIndex)) {
        // No skeletal chain: the game's flails carry theirs as components, simulating once drawn. Looked for now and then.
        if (wp.root && !wp.fists && wp.headTries++ % kHeadScanEvery == 0) ScanParts(wp);
        return false;   // read from the next frame on
    }
    if (!wp.haveHeadBone) {
        if (wp.headTries++ % kHeadScanEvery != 0) return false;
        Call count(wp.flail, "GetNumBones");
        const int32_t n = count.invoke() ? std::min(count.get<int32_t>("ReturnValue"), kFlailMaxBones) : 0;
        for (int32_t b = n - 1; b >= 0; --b) {
            Call name(wp.flail, "GetBoneName");
            name.set<int32_t>("BoneIndex", b);
            if (!name.invoke()) continue;
            const FName bone = name.get<FName>("ReturnValue");
            wp.headSim.put(0, bone);
            if (!wp.headSim.invoke(wp.flail) || !wp.headSim.take<bool>(1)) continue;
            wp.headBone = bone;
            wp.haveHeadBone = true;
            HSM_LOG("rig: %s flail head = bone %s (%d of %d)", ObjectName(willie_).c_str(), NameToString(bone).c_str(), b, n);
            break;
        }
        if (!wp.haveHeadBone) return false;
    }
    wp.headSim.put(0, wp.headBone);
    if (!wp.headSim.invoke(wp.flail) || !wp.headSim.take<bool>(1)) return false;
    wp.headSocket.put(0, wp.headBone);   // TransformSpace stays RTS_World (0), as for the bones
    if (!wp.headSocket.invoke(wp.flail)) return false;
    const FTransform t = wp.headSocket.take<FTransform>(2);
    out = sync::BoneSample{ToV3(t.translation), ToQ4(t.rotation), V3{}, V3{}};
    wp.headLinVelAt.put(0, t.translation).put(1, wp.headBone);
    if (wp.headLinVelAt.invoke(wp.flail)) out.vel = ToV3(wp.headLinVelAt.take<FVector>(2));
    wp.headAngVel.put(0, wp.headBone);
    if (wp.headAngVel.invoke(wp.flail)) out.angVelDeg = ToV3(wp.headAngVel.take<FVector>(1));
    return true;
}

// The weapon actor's simulating primitive components other than its root: a flail's links and head (Flail_A..D: 'Blade1'
// and the links 'Link 1', 'Link 2', each a body joined to the next by a constraint), and the parts of other modular weapons, which
// are welded and no chain (a polearm's Grip and Blade, sync/hsm_two_hand_pose.h). The head is the 'Blade*' part, else the part
// farthest from the handle. On a stand-in, found parts get the chain's collision policy at once.
void FighterRig::ScanParts(Weapon& wp) {
    if (!primitiveClass_) primitiveClass_ = FindClass("PrimitiveComponent");
    if (!primitiveClass_ || !wp.actor || !wp.root) return;
    Call byClass(wp.actor, "K2_GetComponentsByClass");
    byClass.set<UClass*>("ComponentClass", primitiveClass_);
    if (!byClass.valid() || !byClass.invoke()) return;
    void* value = byClass.paramPtr("ReturnValue");
    const FObjArray* arr = static_cast<const FObjArray*>(value);
    std::vector<UObject*> found;
    if (arr && arr->data && arr->num > 0 && arr->num <= 64)
        for (int32_t i = 0; i < arr->num; ++i) {
            UObject* c = arr->data[i];
            if (!c || c == wp.root || c == wp.base || c == wp.tip) continue;
            Call sim(c, "IsSimulatingPhysics");
            sim.setName("BoneName", FName{});
            if (sim.valid() && sim.invoke() && sim.get<bool>("ReturnValue")) found.push_back(c);
        }
    if (FProperty* prop = FindProperty(byClass.function(), "ReturnValue"); prop && value) ClearPropertyValue(prop, value);
    if (found.empty()) return;
    UObject* head = nullptr;
    for (UObject* c : found)
        if (StartsWithNameAscii(ObjectName(c), "Blade")) { head = c; break; }
    if (!head) {
        double farthest = -1.0;
        for (UObject* c : found) {
            Call at(c, "K2_GetComponentLocation");
            if (!at.invoke()) continue;
            const double d = Len(Sub(ToV3(at.get<FVector>("ReturnValue")), wp.pos));
            if (d > farthest) { farthest = d; head = c; }
        }
        if (!head) return;
    }
    const UClass* hc = ClassOf(head);
    wp.headToWorld = PreparedCall(hc, "K2_GetComponentToWorld", {"ReturnValue"});
    wp.headSim = PreparedCall(hc, "IsSimulatingPhysics", {"BoneName", "ReturnValue"});
    wp.headLinVelAt = PreparedCall(hc, "GetPhysicsLinearVelocityAtPoint", {"Point", "BoneName", "ReturnValue"});
    wp.headAngVel = PreparedCall(hc, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    wp.headSetLin = PreparedCall(hc, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
    wp.headSetAng = PreparedCall(hc, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
    if (!wp.headToWorld.valid() || !wp.headSim.valid() || !wp.headLinVelAt.valid() || !wp.headAngVel.valid() ||
        !wp.headSetLin.valid() || !wp.headSetAng.valid()) {
        HSM_LOG("rig: %s: chain part %s has no physics functions; the chain is not streamed", ObjectName(willie_).c_str(), ObjectName(head).c_str());
        wp.headTries = 1;   // not scanned again until the weapon changes
        return;
    }
    wp.parts = found;
    wp.partIndex.clear();
    wp.linked = false;
    for (UObject* c : found) {
        wp.partIndex.push_back(ObjectIndex(c));
        if (StartsWithNameAscii(ObjectName(c), "Link")) wp.linked = true;
    }
    wp.head = head;
    wp.headIndex = ObjectIndex(head);
    wp.headBone = FName{};
    wp.haveHeadBone = false;
    // Only on a stand-in (Neutralise ran). Read also runs on the fighters this machine simulates (OwnerStreams, the host's
    // under host physics), and a modular weapon's Pommel, Guard, Head and Grip are such simulating parts: the policy left
    // a live fighter's weapon without gravity and blind to the world, so it floated off once dropped (#138-#141).
    if (boundAtMs_ >= 0.0) ChainPolicy(wp);
    std::string names;
    for (UObject* c : found) { if (!names.empty()) names += ' '; names += ObjectName(c); }
    HSM_LOG("rig: %s: %s chain = %zu simulating part(s) (%s), head %s", ObjectName(willie_).c_str(), ObjectName(wp.actor).c_str(),
            found.size(), names.c_str(), ObjectName(head).c_str());
}

UObject* FighterRig::HeadBody(const Weapon& wp) const {
    if (wp.head) return Live(wp.head, wp.headIndex) ? wp.head : nullptr;
    return wp.haveHeadBone && Live(wp.flail, wp.flailIndex) ? wp.flail : nullptr;
}

UObject* FighterRig::ChainBody(const Weapon& wp) const {
    if (wp.flail && Live(wp.flail, wp.flailIndex)) return wp.flail;
    if (!wp.parts.empty() && Live(wp.parts[0], wp.partIndex[0])) return wp.parts[0];
    return nullptr;
}

void FighterRig::ChainPolicy(const Weapon& wp) {
    if (wp.flail && Live(wp.flail, wp.flailIndex)) SetChainPolicy(wp.flail);
    for (size_t i = 0; i < wp.parts.size(); ++i)
        if (Live(wp.parts[i], wp.partIndex[i])) SetChainPolicy(wp.parts[i]);
}

// A component chain after the rig toggled the handle's physics (ApplySeat, ApplyHeld): every part simulating, and still.
void FighterRig::RearmChain(const Weapon& wp) {
    if (Live(wp.actor, wp.index)) RearmFlail(wp.actor);
    for (size_t i = 0; i < wp.parts.size(); ++i) {
        UObject* c = wp.parts[i];
        if (!Live(c, wp.partIndex[i])) continue;
        Call sim(c, "IsSimulatingPhysics");
        sim.setName("BoneName", FName{});
        if (sim.invoke() && !sim.get<bool>("ReturnValue")) Call(c, "SetSimulatePhysics").set<bool>("bSimulate", true).invoke();
        Call(c, "SetPhysicsLinearVelocity").set("NewVel", FVector{}).set<bool>("bAddToCurrent", false).setName("BoneName", FName{}).invoke();
        Call(c, "SetPhysicsAngularVelocityInDegrees").set("NewAngVel", FVector{}).set<bool>("bAddToCurrent", false).setName("BoneName", FName{}).invoke();
    }
}

bool FighterRig::Read(uint32_t tsMs, uint16_t stepUs, sync::PoseFrame* frame, sync::FighterNow* now) {
    if (!StillValid()) return false;
    sendsFrames_ = frame != nullptr;   // the player's own live fighter (CheckHandLoss)
    {
        const double t = SteadyMs();
        readGapMs_ = lastReadMs_ >= 0.0 ? t - lastReadMs_ : 0.0;
        lastReadMs_ = t;
    }
    if (frame) { frame->tsMs = tsMs; frame->stepUs = stepUs; }
    if (now) *now = sync::FighterNow{};
    pelvisRead_ = false;
    const bool measure = now && !comMeasured_;
    int comBad = 0;
    V3 hand[2]{};
    for (int i = 0; i < sync::kPoseBones; ++i) {
        socket_.put(0, bones_[i]);   // TransformSpace is never written: the zeroed buffer holds RTS_World (0)
        if (!socket_.invoke(mesh_)) return false;
        const FTransform t = socket_.take<FTransform>(2);
        const V3 pos = ToV3(t.translation);
        const Q4 rot = ToQ4(t.rotation);
        if (i == sync::kBonePelvis) { pelvis_ = pos; pelvisRead_ = true; }
        if (i == kHandBone[0]) { hand[0] = pos; handScale_[0] = ToV3(t.scale); }
        if (i == kHandBone[1]) { hand[1] = pos; handScale_[1] = ToV3(t.scale); }
        const bool body = sync::kPoseHasBody[i];
        if (frame) {
            sync::BoneSample& b = frame->bones[i];
            b = sync::BoneSample{pos, rot, V3{}, V3{}};
            if (body) {   // spine_01 has no body: its velocities stay zero
                linVelAt_.put(0, t.translation).put(1, bones_[i]);
                if (linVelAt_.invoke(mesh_)) b.vel = ToV3(linVelAt_.take<FVector>(2));
                angVel_.put(0, bones_[i]);
                if (angVel_.invoke(mesh_)) b.angVelDeg = ToV3(angVel_.take<FVector>(1));
            }
        }
        if (now) {
            if (measure && body) {   // the body's centre of mass in its bone frame (servo.rs `com`), measured once
                com_.put(0, bones_[i]);
                if (com_.invoke(mesh_)) {
                    const V3 off = Sub(ToV3(com_.take<FVector>(1)), pos);
                    if (Len(off) <= kBoneComMaxUu) comLocal_[i] = Unrotate(rot, off); else ++comBad;
                } else {
                    ++comBad;
                }
            }
            now->bones[i] = sync::BodyNow{pos, rot, comLocal_[i]};
            now->valid[i] = true;
        }
    }
    if (now) {   // the bodies the driver watches for blows (StandInDriver::DetectImpact)
        for (int b : sync::kContactBones) {
            linVel_.put(0, bones_[b]);
            if (linVel_.invoke(mesh_)) { now->vel[b] = ToV3(linVel_.take<FVector>(1)); now->velValid[b] = true; }
        }
    }
    if (measure && (comBad == 0 || ++comTries_ >= kComTries)) {
        comMeasured_ = true;
        if (comBad) HSM_LOG("rig: %s: %d bone centres of mass unreadable, servoing those about the bone origin", ObjectName(willie_).c_str(), comBad);
    }
    for (int w = 0; w < sync::kPoseWeapons; ++w) {
        RefreshWeapon(w);
        Weapon& wp = weapons_[w];
        const bool wasHeld = wp.wasHeld;
        const bool held = ReadWeapon(w, hand[w]);
        wp.wasHeld = held;
        if (wasHeld && !held && wp.farFromHand) CheckHandLoss(w, wp);   // the weapon flew out of reach with its variable still set
        sync::BoneSample head{};
        const bool haveHead = held && (frame || now) && ReadHead(wp, head);
        if (frame) {
            sync::WeaponSample& s = frame->weapons[w];
            s = sync::WeaponSample{};
            if (held) {
                s.present = true;
                s.hand = static_cast<uint8_t>(w);
                s.classHash = wp.classHash;
                s.pos = wp.pos;
                s.rot = wp.rot;
                wp.linVelAt.put(0, ToF(wp.pos)).put(1, FName{});
                if (wp.linVelAt.invoke(wp.root)) s.vel = ToV3(wp.linVelAt.take<FVector>(2));
                wp.angVel.put(0, FName{});
                if (wp.angVel.invoke(wp.root)) s.angVelDeg = ToV3(wp.angVel.take<FVector>(1));
                s.base = wp.basePos;
                s.tip = wp.tipPos;
                s.hasHead = haveHead;
                if (haveHead) s.head = head;
            }
        }
        if (now && held) {
            if (!wp.comMeasured) {
                wp.com.put(0, FName{});
                const V3 off = wp.com.invoke(wp.root) ? Sub(ToV3(wp.com.take<FVector>(1)), wp.pos) : V3{kWeaponComMaxUu * 2, 0, 0};
                if (Len(off) <= kWeaponComMaxUu) { wp.comLocal = Unrotate(wp.rot, off); wp.comMeasured = true; }
                else if (++wp.comTries >= kComTries) {
                    wp.comMeasured = true;
                    HSM_LOG("rig: %s slot %d: centre of mass unreadable, servoing about the root", ObjectName(willie_).c_str(), w);
                }
            }
            now->weapons[w] = sync::BodyNow{wp.pos, wp.rot, wp.comLocal};
            now->weaponValid[w] = true;
            if (haveHead) {   // servoed about the bone origin, where its velocity is read and sent
                now->heads[w] = sync::BodyNow{head.pos, head.rot, V3{}};
                now->headValid[w] = true;
            }
        }
    }
    if (frame) {
        frame->grip = ReadGrip();
        // The sender's side of the grip toggles (two-player test 12 asked whether a mordhau was ever sent).
        const sync::GripState& g = frame->grip;
        const int toggles = sync::SeatKey(g.altGrip, g.mordhau, g.reverseR) | (g.switched2H ? 8 : 0);
        if (toggles != ownToggles_) {
            ownToggles_ = toggles;
            HSM_LOG("rig: %s sends grips R %u L %u%s%s%s%s", ObjectName(willie_).c_str(), unsigned(g.gripR), unsigned(g.gripL),
                    g.altGrip ? " alt" : "", g.mordhau ? " mordhau" : "", g.switched2H ? " switched" : "", g.reverseR ? " reverse" : "");
        }
    }
    if (!typesLogged_) {
        typesLogged_ = true;
        auto type = [this](int w) { const Weapon& wp = weapons_[w]; return Live(wp.root, wp.rootIndex) ? ObjectType(wp.root) : -1; };
        HSM_LOG("rig: %s collision object types: mesh %d, weapon R %d, weapon L %d", ObjectName(willie_).c_str(), ObjectType(mesh_),
                type(0), type(1));
    }
    return true;
}

bool FighterRig::WeaponTip(int w, V3& out) const {
    if (w < 0 || w >= sync::kPoseWeapons || !weapons_[w].read || !weapons_[w].haveTip) return false;
    out = weapons_[w].tipPos;
    return true;
}

// ---- driving ---------------------------------------------------------------------------------------------------------

void FighterRig::HoldStill(Weapon& wp) {
    wp.setLin.put(0, FVector{}).put(1, false).put(2, FName{});
    wp.setLin.invoke(wp.root);
    wp.setAng.put(0, FVector{}).put(1, false).put(2, FName{});
    wp.setAng.invoke(wp.root);
}

// HSMP main.lua PX.grip_want: a hand's grip is freed once the weapon it holds is servoed to the owner's pose (hand and
// weapon then each go exactly where the owner has them), or when the hand holds fists, whose kinematic pseudo weapon at
// the world origin would otherwise twist the driven arm. An empty hand has nothing to hold to.
bool FighterRig::GripWanted(int hand) const {
    UObject* held = FieldAt(weaponOffset_[hand]);
    if (!held) return true;
    for (const Weapon& wp : weapons_)
        if (wp.actor == held && (wp.fists || wp.driven)) return true;   // a two-handed weapon sits in slot 0 for both hands
    return false;
}

// The game itself holds a weapon by locking the hand's grip joint once its soft hold has closed (Willie_BP ReceiveTick,
// R Hand Soft Attach Distance reaching 0). A stand-in's ReceiveTick is blocked, so the rig locks it: the hand, which
// follows the owner's pose closely, carries the weapon, and a knock cannot take it out of the hand. The off hand of a
// two-handed grip holds no Weapon L; its joint (PhysicsConstraint L Hand) exists while L Hand In Offhand Attached.
bool FighterRig::GripToLock(int hand) const {
    if (!g_gripLock || !FieldAt(gripOffset_[hand])) return false;
    UObject* held = FieldAt(weaponOffset_[hand]);
    if (!held) return hand == 1 && offJoint_ && FieldAt(gripOffset_[1]) == offJoint_;
    for (const Weapon& wp : weapons_)
        if (wp.actor == held && wp.fists) return false;
    return true;
}

void FighterRig::LockGrip(int hand) {
    UObject* c = FieldAt(gripOffset_[hand]);
    if (!c || c == gripLocked_[hand]) return;
    Call(c, "SetAngularDriveParams").set<float>("PositionStrength", 0.f).set<float>("VelocityStrength", 0.f)
        .set<float>("InForceLimit", 0.f).invoke();
    if (sync::HoldFor(hand, c == offJoint_, poseHeld_[hand], g_twoHandLock) == sync::GripHold::GameOffhand && HoldOffhand(c)) return;
    Call(c, "SetLinearXLimit").set<uint8_t>("ConstraintType", kLocked).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetLinearYLimit").set<uint8_t>("ConstraintType", kLocked).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetLinearZLimit").set<uint8_t>("ConstraintType", kLocked).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetAngularSwing1Limit").set<uint8_t>("MotionType", kLocked).set<float>("Swing1LimitAngle", 0.f).invoke();
    Call(c, "SetAngularSwing2Limit").set<uint8_t>("MotionType", kLocked).set<float>("Swing2LimitAngle", 0.f).invoke();
    Call(c, "SetAngularTwistLimit").set<uint8_t>("ConstraintType", kLocked).set<float>("TwistLimitAngle", 0.f).invoke();
    gripLocked_[hand] = c;
    gripFreed_[hand] = nullptr;
    gripLoose_[hand] = nullptr;   // a locked joint is not loose: HoldStill must not pin a weapon the hand carries
    if (gripLogged_[hand] != c) {
        gripLogged_[hand] = c;
        HSM_LOG("rig: %s: %s locked", ObjectName(willie_).c_str(), kGripFields[hand]);
    }
}

// The off hand of a two-handed grip is not welded to the shaft by the game: once its soft hold closes (Willie_BP.cpp
// :18205-18240, then :164ad) the joint keeps X/Y locked, lets the hand slide along the shaft (Z, limited to the weapon's
// OffhandLimit, AltGrip1_Limit in the alt grip, locked with the hands switched or a hammer grip) under a position drive,
// and leaves the angles limited where the hold closed (Soft Attach Dot >= 0.9: at most 36 degrees). Locking every limit
// (3b0c7fb) welded the hand at the attach frame while the servo turned it toward the owner's hand: the two fought, the
// off hand looked turned round on the shaft and the weapon wobbled. The rig now sets the game's own limits.
bool FighterRig::HoldOffhand(UObject* c) {
    UObject* const weaponR = FieldAt(weaponOffset_[0]);
    if (!weaponR) return false;
    const auto* base = reinterpret_cast<const uint8_t*>(willie_);
    auto flag = [&](int i) {
        bool v = false;
        return gripFlag_[i] && GetBoolValue(gripFlag_[i], base + PropertyOffset(gripFlag_[i]), v) && v;
    };
    const bool alt = flag(1), switched = flag(3);
    const uint8_t gripR = gripTypeOffset_[0] >= 0 ? base[gripTypeOffset_[0]] : 0;
    const double* limit = Field<double>(weaponR, alt ? "AltGrip1_Limit" : "OffhandLimit");
    const double* str = Field<double>(weaponR, alt ? "AltGrip1_Str" : "OffhandStr");
    if (!limit || !str) return false;
    const uint8_t slide = gripR == 15 || switched ? kLocked : kLimited;
    Call(c, "SetLinearXLimit").set<uint8_t>("ConstraintType", kLocked).setReal("LimitSize", 0.0).invoke();
    Call(c, "SetLinearYLimit").set<uint8_t>("ConstraintType", kLocked).setReal("LimitSize", 0.0).invoke();
    Call(c, "SetLinearZLimit").set<uint8_t>("ConstraintType", slide).setReal("LimitSize", *limit).invoke();
    Call(c, "SetLinearPositionDrive").set<bool>("bEnableDriveX", true).set<bool>("bEnableDriveY", true)
        .set<bool>("bEnableDriveZ", true).invoke();
    Call(c, "SetLinearDriveParams").setReal("PositionStrength", *str).setReal("VelocityStrength", 0.0)
        .setReal("InForceLimit", 0.0).invoke();
    Call(c, "SetAngularSwing1Limit").set<uint8_t>("MotionType", kLimited).setReal("Swing1LimitAngle", kOffhandAngleDeg).invoke();
    Call(c, "SetAngularSwing2Limit").set<uint8_t>("MotionType", kLimited).setReal("Swing2LimitAngle", kOffhandAngleDeg).invoke();
    Call(c, "SetAngularTwistLimit").set<uint8_t>("ConstraintType", kLimited).setReal("TwistLimitAngle", kOffhandAngleDeg).invoke();
    gripLocked_[1] = c;
    gripFreed_[1] = nullptr;
    gripLoose_[1] = nullptr;
    if (gripLogged_[1] != c) {
        gripLogged_[1] = c;
        HSM_LOG("rig: %s: %s held on the shaft (slide %.1f, %s, angles %.0f)", ObjectName(willie_).c_str(), kGripFields[1],
                *limit, slide == kLocked ? "locked" : "limited", kOffhandAngleDeg);
    }
    return true;
}

void FighterRig::FreeGrip(int hand) {
    UObject* c = FieldAt(gripOffset_[hand]);
    if (!c || c == gripFreed_[hand]) return;
    // Drive and limits off (HSMP main.lua:1127 grip_limits, :2258 grips_off). Every value is zero, so the float/double
    // width of these parameters does not matter.
    Call(c, "SetAngularDriveParams").set<float>("PositionStrength", 0.f).set<float>("VelocityStrength", 0.f)
        .set<float>("InForceLimit", 0.f).invoke();
    Call(c, "SetLinearXLimit").set<uint8_t>("ConstraintType", kFree).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetLinearYLimit").set<uint8_t>("ConstraintType", kFree).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetLinearZLimit").set<uint8_t>("ConstraintType", kFree).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetAngularSwing1Limit").set<uint8_t>("MotionType", kFree).set<float>("Swing1LimitAngle", 0.f).invoke();
    Call(c, "SetAngularSwing2Limit").set<uint8_t>("MotionType", kFree).set<float>("Swing2LimitAngle", 0.f).invoke();
    Call(c, "SetAngularTwistLimit").set<uint8_t>("ConstraintType", kFree).set<float>("TwistLimitAngle", 0.f).invoke();
    gripFreed_[hand] = c;
    gripLocked_[hand] = nullptr;
    gripLoose_[hand] = c;
    gripLooseIndex_[hand] = ObjectIndex(c);
    if (gripLogged_[hand] != c) {
        gripLogged_[hand] = c;
        HSM_LOG("rig: %s: %s freed", ObjectName(willie_).c_str(), kGripFields[hand]);
    }
}

void FighterRig::Drive(const sync::FighterCommand& cmd) {
    if (!StillValid()) return;
    // The off hand on the shaft rides with the weapon: servoing it as well pulls the shaft sideways against the joint,
    // and a polearm's tip swings by the length between the hands.
    const bool offOnShaft = offJoint_ && FieldAt(gripOffset_[1]) == offJoint_;
    for (int i = 0; i < sync::kPoseBones; ++i) {
        if (!cmd.drive[i] || (offOnShaft && i == sync::kBoneHandL)) continue;
        setLin_.put(0, ToF(cmd.bones[i].vel)).put(1, false).put(2, bones_[i]);
        setLin_.invoke(mesh_);
        setAng_.put(0, ToF(cmd.bones[i].angVelDeg)).put(1, false).put(2, bones_[i]);
        setAng_.invoke(mesh_);
    }
    bool lock[2] = {GripToLock(0), GripToLock(1)};
    for (int w = 0; w < sync::kPoseWeapons; ++w) {
        Weapon& wp = weapons_[w];
        // A weapon a locked hand carries is not servoed on its own: the joint and the servo would fight.
        wp.driven = cmd.weaponDrive[w] && wp.read && !Pinned(w) && !lock[w];
        if (!wp.driven) continue;
        wp.setLin.put(0, ToF(cmd.weapons[w].vel)).put(1, false).put(2, FName{});
        wp.setLin.invoke(wp.root);
        wp.setAng.put(0, ToF(cmd.weapons[w].angVelDeg)).put(1, false).put(2, FName{});
        wp.setAng.invoke(wp.root);
    }
    const bool posed = HasTwoHandPose();
    for (int w = 0; w < sync::kPoseWeapons; ++w) {   // the flail heads, on their own chains (4b)
        Weapon& wp = weapons_[w];
        UObject* const hb = HeadBody(wp);
        if (!sync::HeadServoed(cmd.headDrive[w], posed, w) || !wp.read || Pinned(w) || !hb) continue;
        wp.headSetLin.put(0, ToF(cmd.heads[w].vel)).put(1, false).put(2, wp.headBone);   // headBone: none on a part
        wp.headSetLin.invoke(hb);
        wp.headSetAng.put(0, ToF(cmd.heads[w].angVelDeg)).put(1, false).put(2, wp.headBone);
        wp.headSetAng.invoke(hb);
    }
    for (int h = 0; h < 2; ++h) {   // the pose took this joint over or let it go: held again (sync::HoldFor)
        UObject* const c = FieldAt(gripOffset_[h]);
        const bool owned = c && twoHandJoints_[h].changed && twoHandJoints_[h].joint == c;
        if (owned == poseHeld_[h]) continue;
        poseHeld_[h] = owned;
        gripLocked_[h] = nullptr;
        if (g_twoHandLock) HSM_LOG("rig: %s: %s %s", ObjectName(willie_).c_str(), kGripFields[h],
                                   owned ? "locked to the two-hand pose" : "back to its own hold");
    }
    for (int h = 0; h < 2; ++h) {
        if (lock[h]) LockGrip(h);
        else if (GripWanted(h)) FreeGrip(h);
    }
    for (int w = 0; w < sync::kPoseWeapons; ++w)   // gravity is off: a loose, undriven weapon would drift
        if (weapons_[w].read && !weapons_[w].driven && !Pinned(w) && GripLoose(w)) HoldStill(weapons_[w]);
}

// The hand's current grip constraint is one this rig freed (its limits stay free: they are never restored).
bool FighterRig::GripLoose(int hand) const {
    UObject* c = gripLoose_[hand];
    return c && Live(c, gripLooseIndex_[hand]) && c == FieldAt(gripOffset_[hand]);
}

void FighterRig::Shift(const V3& d) {
    if (!StillValid()) return;
    // Every body goes to where this frame's Read found it plus d, measured now rather than applied as a delta: a
    // teleporting capsule move in between may already have carried the Mesh (HSMP reads the pelvis for snap_mesh after
    // drive_root, main.lua:4773-4791 and :2340).
    V3 meshD = d;
    if (pelvisRead_) {
        socket_.put(0, bones_[sync::kBonePelvis]);
        if (socket_.invoke(mesh_)) meshD = Sub(Add(pelvis_, d), ToV3(socket_.take<FTransform>(2).translation));
    }
    MoveBy(mesh_, meshD);
    Call(mesh_, "SetAllPhysicsLinearVelocity").set("NewVel", FVector{}).set<bool>("bAddToCurrent", false).invoke();
    Call(mesh_, "SetAllPhysicsAngularVelocityInDegrees").set("NewAngVel", FVector{}).set<bool>("bAddToCurrent", false).invoke();
    // The simulated held weapons move with the body; left behind, the servo would drag a polearm back through the
    // moved arm (HSMP main.lua:2350). A pinned weapon stays where the joint holds it.
    for (int w = 0; w < sync::kPoseWeapons; ++w) {
        Weapon& wp = weapons_[w];
        if (!wp.read || Pinned(w)) continue;
        // A skeletal flail chain (wp.flail) stays behind: this build reflects no way to move a simulated chain's bodies
        // (SetAllPhysicsPosition was never in the exe; the bone setters are PoseableMeshComponent-only), so Shift moves
        // only what it can - the parts and the root below - and the joints drag the chain, as before 5d3e67f.
        // A component chain: every part by d, still (0.8.0-pre7). Read before the root moves and placed after it: a part welded
        // to the root goes with it, and moving it by d as well would leave it 2d away.
        std::vector<std::pair<UObject*, V3>> parts;
        for (size_t i = 0; i < wp.parts.size(); ++i) {
            if (!Live(wp.parts[i], wp.partIndex[i])) continue;
            Call at(wp.parts[i], "K2_GetComponentLocation");
            if (at.invoke()) parts.emplace_back(wp.parts[i], ToV3(at.get<FVector>("ReturnValue")));
        }
        MoveTo(wp.root, Add(wp.pos, d));   // wp.pos is the root component's location as read this frame
        for (const auto& [part, at] : parts) {
            MoveTo(part, Add(at, d));
            Call(part, "SetPhysicsLinearVelocity").set("NewVel", FVector{}).set<bool>("bAddToCurrent", false).setName("BoneName", FName{}).invoke();
            Call(part, "SetPhysicsAngularVelocityInDegrees").set("NewAngVel", FVector{}).set<bool>("bAddToCurrent", false).setName("BoneName", FName{}).invoke();
        }
        HoldStill(wp);
    }
    slots_.Shift(willie_, d);   // the sheathed weapons go with the body (standin_slots.h)
}

void FighterRig::PlaceCapsule(const V3& at, const double* yawDeg) {
    if (!StillValid() || !actorLoc_.invoke(willie_)) return;
    const bool jump = Len(Sub(ToV3(actorLoc_.take<FVector>(0)), at)) > kCapsuleTeleportUu;
    placeActor_.put(0, ToF(at)).put(1, false).put(2, jump);
    placeActor_.invoke(willie_);
    if (yawDeg && std::isfinite(*yawDeg) && turnActor_.valid()) {
        turnActor_.put(0, FRotator{0.0, *yawDeg, 0.0}).put(1, false);
        turnActor_.invoke(willie_);
    }
}

// ---- neutralising ----------------------------------------------------------------------------------------------------

void FighterRig::Neutralise(double nowMs) {
    if (!StillValid()) return;
    auto* base = reinterpret_cast<uint8_t*>(willie_);
    if (!savedFields_) {
        savedFields_ = true;
        for (int32_t off : zeroDoubles_) savedDoubles_.push_back(*reinterpret_cast<double*>(base + off));
        for (FProperty* p : trueBools_) { bool v = false; GetBoolValue(p, base + PropertyOffset(p), v); savedTrue_.push_back(v); }
        for (FProperty* p : falseBools_) { bool v = false; GetBoolValue(p, base + PropertyOffset(p), v); savedFalse_.push_back(v); }
    }
    for (int32_t off : zeroDoubles_) *reinterpret_cast<double*>(base + off) = 0.0;
    for (FProperty* p : trueBools_) SetBoolValue(p, base + PropertyOffset(p), true);
    for (FProperty* p : falseBools_) SetBoolValue(p, base + PropertyOffset(p), false);
    if (boundAtMs_ < 0.0) boundAtMs_ = nowMs;
    // A weapon the game just sheathed hangs from BoneCore, which the rig leaves behind when it moves the body: anchored
    // (StandInSlots::Anchor) on this pass, not up to kFullEarlyEveryMs later. Two-player logs 8 and 9: each new stand-in's
    // sheathed weapons were 4-25 m away until a pass about 520 ms after its hand weapons came.
    bool slotsChanged = false;
    for (int k = 0; k < kSheathSlots; ++k)
        if (UObject* s = FieldAt(slotOffset_[k]); s != slotSeen_[k]) { slotSeen_[k] = s; slotsChanged = true; }
    // The game's own collision refreshes undo the chain's policy between full passes: Unsheathed Mode (a draw,
    // Willie_BP.cpp:6426) and Turn On Collision put the chain's WorldStatic response back to Block
    // (ModularWeaponBP.cpp:2232-2234), Refresh Collisions re-attaches it (:1651-1657). One cheap read per chain per frame.
    // The same refreshes put a held weapon's Pawn response back (0.8.0-pre7: weapon roots pass through fighters too).
    for (Weapon& wp : weapons_) {
        if (UObject* c = ChainBody(wp)) {
            Call g(c, "GetCollisionResponseToChannel");
            g.set<uint8_t>("Channel", kWorldStatic);
            if (g.invoke() && g.get<uint8_t>("ReturnValue") != kIgnore) ChainPolicy(wp);
        }
        if (wp.root && Live(wp.actor, wp.index) && Live(wp.root, wp.rootIndex)) {
            Call g(wp.root, "GetCollisionResponseToChannel");
            g.set<uint8_t>("Channel", kPawn);
            if (g.invoke() && g.get<uint8_t>("ReturnValue") != kIgnore) SetChainPolicy(wp.root);
        }
    }
    slots_.Watch(willie_);   // sheathed weapons that drifted from their sockets are put back (standin_slots.h)
    if (nowMs < nextFullMs_ && !weaponChanged_ && !slotsChanged) return;
    NeutraliseFull();
    weaponChanged_ = false;
    nextFullMs_ = nowMs + (nowMs - boundAtMs_ < kFullEarlyForMs ? kFullEarlyEveryMs : kFullEveryMs);
}

// ---- grips -----------------------------------------------------------------------------------------------------------

sync::GripState FighterRig::ReadGrip() const {
    sync::GripState g;
    if (!willie_) return g;
    const auto* base = reinterpret_cast<const uint8_t*>(willie_);
    if (gripTypeOffset_[0] >= 0) g.gripR = base[gripTypeOffset_[0]];
    if (gripTypeOffset_[1] >= 0) g.gripL = base[gripTypeOffset_[1]];
    bool* const flags[5] = {&g.offhand, &g.altGrip, &g.mordhau, &g.switched2H, &g.reverseR};
    for (int i = 0; i < 5; ++i) {
        bool v = false;
        if (gripFlag_[i] && GetBoolValue(gripFlag_[i], base + PropertyOffset(gripFlag_[i]), v)) *flags[i] = v;
    }
    return g;
}

void FighterRig::ApplyGrip(const sync::GripState& g) {
    if (!StillValid()) return;
    auto* base = reinterpret_cast<uint8_t*>(willie_);
    sync::GripState applied = g;
    uint8_t* const appliedType[2] = {&applied.gripR, &applied.gripL};
    const uint8_t poseType[2] = {g.gripR, g.gripL};
    for (int h = 0; h < 2; ++h) {
        if (gripTypeOffset_[h] < 0) continue;
        UObject* held = FieldAt(weaponOffset_[h]);   // this stand-in's own hand: a two-handed grip leaves Weapon L null
        uint8_t def = 0;
        if (held && poseType[h] == 0) {              // the weapon's default matters only where the pose has none
            const uint8_t* d = h == 0 ? Field<uint8_t>(held, "Grip R Hand Default") : Field<uint8_t>(held, "Grip L Hand Default");
            if (d) def = *d;
        }
        const uint8_t v = sync::GripToApply(poseType[h], held != nullptr, def);
        *appliedType[h] = v;
        if (held && v == 0) continue;                // the weapon's own default is 0 too: leave the game's value, 0 destroys it
        base[gripTypeOffset_[h]] = v;
    }
    // The off hand of a two-handed grip: the game's own Attach L Hand To Offhand Position makes its joint (PhysicsConstraint
    // L Hand to the right hand's weapon, Willie_BP.cpp:1829) and sets L Hand In Offhand Attached; Break L Constraint undoes
    // both (:1980). Writing the flag alone (pre9) left the off hand with no joint, flailing beside the shaft. GripToLock
    // then locks the joint like the right hand's. The alt / mordhau flags are written first: the attach point follows them.
    const bool flags[5] = {g.offhand, g.altGrip, g.mordhau, g.switched2H, g.reverseR};
    for (int i = 1; i < 5; ++i)
        if (gripFlag_[i]) SetBoolValue(gripFlag_[i], base + PropertyOffset(gripFlag_[i]), flags[i]);
    UObject* const weaponR = FieldAt(weaponOffset_[0]);
    UObject* const weaponL = FieldAt(weaponOffset_[1]);
    const bool wantOff = g.offhand && weaponR && (!weaponL || weaponL == weaponR);
    if (ApplySeat(g, weaponR) && offJoint_) {
        // The off hand's attach point follows the alt / mordhau flags (Willie_BP.cpp:1831): made again at once.
        Call br(willie_, "Break L Constraint");
        br.set<bool>("Slide", false);
        br.invoke();
        offJoint_ = nullptr;
        offTryMs_ = -1e18;
    }
    const double now = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (wantOff != offWant_) { offWant_ = wantOff; offWantSinceMs_ = now; }
    const bool steady = now - offWantSinceMs_ >= (wantOff ? kOffhandAttachMs : kOffhandReleaseMs);
    if (wantOff && (!offJoint_ || FieldAt(gripOffset_[1]) != offJoint_) && (steady || offJoint_)) {
        if (now - offTryMs_ >= kOffhandRetryMs) {
            if (!RestoreTwoHandPose()) return;
            offTryMs_ = now;
            UObject* const before = FieldAt(gripOffset_[1]);
            Call at(willie_, "Attach L Hand To Offhand Position");
            UObject* const after = at.valid() && at.invoke() ? FieldAt(gripOffset_[1]) : nullptr;
            offJoint_ = after && after != before ? after : nullptr;
            // The attach ends in Event Slide Right Hand Along Shaft (Willie_BP.cpp:1858, :10021): on a polearm the right
            // hand's joint is set to slide, which only the blocked ReceiveTick steers. Locked again on the next Drive.
            gripLocked_[0] = nullptr;
            HSM_LOG("rig: %s: off hand %s", ObjectName(willie_).c_str(), offJoint_ ? "attached to the shaft" : "attach FAILED");
        }
    } else if (!wantOff && offJoint_ && (steady || !weaponR)) {
        if (!RestoreTwoHandPose()) return;
        Call br(willie_, "Break L Constraint");
        br.set<bool>("Slide", false);
        br.invoke();
        offJoint_ = nullptr;
        HSM_LOG("rig: %s: off hand released", ObjectName(willie_).c_str());
    }
    if (gripFlag_[0]) SetBoolValue(gripFlag_[0], base + PropertyOffset(gripFlag_[0]), offJoint_ != nullptr);
    ApplySwitch(g.switched2H && !g.mordhau, weaponR);   // mordhau sets the flag too, but is a seat (ApplySeat), not a slide
    applied.offhand = offJoint_ != nullptr;
    const sync::GripState& l = gripStateLogged_;
    if (gripStateKnown_ && l.gripR == applied.gripR && l.gripL == applied.gripL && l.offhand == applied.offhand &&
        l.altGrip == applied.altGrip && l.mordhau == applied.mordhau && l.switched2H == applied.switched2H && l.reverseR == applied.reverseR)
        return;
    gripStateLogged_ = applied;
    gripStateKnown_ = true;
    HSM_LOG("rig: %s grips R %u L %u%s%s%s%s%s", ObjectName(willie_).c_str(), unsigned(applied.gripR), unsigned(applied.gripL),
            applied.offhand ? " offhand" : "", applied.altGrip ? " alt" : "", applied.mordhau ? " mordhau" : "",
            applied.switched2H ? " switched" : "", applied.reverseR ? " reverse" : "");
}

// The polearm hand switch (X: Willie_BP Polearm Hands Switch, :16393) slides the hands along the shaft by moving the grip
// joints' reference positions; writing "2H Switched Hands (Temp)" alone (pre10) moved the animation only, the hands kept
// their places. The rig moves them as the game does whenever the owner's flag differs from what this right-hand joint was
// set to (a new joint starts unswitched, as after the game's own grab).
void FighterRig::ApplySwitch(bool on, UObject* weaponR) {
    UObject* const r = FieldAt(gripOffset_[0]);
    if (r != switchJoint_) { switchJoint_ = r; switchOn_ = false; }
    // An off-hand joint made again while switched (a re-seat, a re-attach) starts at the unswitched spot: set it again.
    const bool offRemade = on && switchOn_ && offJoint_ && offJoint_ != switchOffJoint_ && FieldAt(gripOffset_[1]) == offJoint_;
    if (!r || !weaponR || (on == switchOn_ && !offRemade)) return;
    if (!RestoreTwoHandPose()) return;
    switchOn_ = on;
    UObject* const grip = ObjField(weaponR, "Grip");
    UObject* const offPos = ObjField(weaponR, "Grip L Hand Off Hand");
    UObject* const defPos = ObjField(weaponR, "Grip R Hand Default Pos");
    const FVector* const g = grip ? Field<FVector>(grip, "RelativeLocation") : nullptr;
    const FVector* const o = offPos ? Field<FVector>(offPos, "RelativeLocation") : nullptr;
    const FVector* const d = defPos ? Field<FVector>(defPos, "RelativeLocation") : nullptr;
    if (!g || !o || !d) {
        HSM_LOG("rig: %s: hand switch FAILED (no grip positions on %s)", ObjectName(willie_).c_str(), ObjectName(weaponR).c_str());
        return;
    }
    const double k = ObjField(grip, "StaticMesh") ? -5.0 : 1.0;   // the game's own factors
    const FVector refR = on ? FVector{o->x + d->x * k, o->y + d->y * k, o->z + d->z * k}
                            : FVector{g->x + d->x, g->y + d->y, g->z + d->z};
    Call(r, "SetConstraintReferencePosition").set<uint8_t>("Frame", 0).set("RefPosition", refR).invoke();
    const bool off = offJoint_ && FieldAt(gripOffset_[1]) == offJoint_;
    if (off) {
        Call(offJoint_, "SetConstraintReferencePosition").set<uint8_t>("Frame", 0).set("RefPosition", on ? FVector{} : *o).invoke();
        gripLocked_[1] = nullptr;   // HoldOffhand again: the switched hands lock the slide along the shaft
    }
    switchOffJoint_ = off ? offJoint_ : nullptr;
    HSM_LOG("rig: %s: hands %s along the shaft%s", ObjectName(willie_).c_str(), on ? "switched" : "switched back",
            off ? "" : " (right hand only)");
}

// The alt grip, mordhau and reverse grip move the weapon in the right hand: the game re-seats it in its ReceiveTick
// (Willie_BP.cpp:18042-18066), which a stand-in does not run, so writing the flags alone (0.5.0) left the hand where it
// was. The rig re-seats it as the game does whenever the owner's toggles differ from what this joint was seated for:
// the weapon goes to the hand socket, turned by the reverse grip, then so the chosen attach point sits in the hand; the
// joint is made again there and the weapon goes back, for the locked joint to carry it to its new seat.
bool FighterRig::ApplySeat(const sync::GripState& g, UObject* weaponR) {
    UObject* const r = FieldAt(gripOffset_[0]);
    if (r != seatJoint_) { seatJoint_ = r; seatKey_ = 0; }   // the game makes a new joint at the weapon's default seat
    const uint8_t key = sync::SeatKey(g.altGrip, g.mordhau, g.reverseR);
    if (!r || !weaponR || key == seatKey_ || !GripToLock(0)) return false;
    if (!RestoreTwoHandPose()) return false;
    seatKey_ = key;
    const auto* base = reinterpret_cast<const uint8_t*>(willie_);
    const uint8_t gripR = gripTypeOffset_[0] >= 0 ? base[gripTypeOffset_[0]] : 0;
    const uint8_t* revType = Field<uint8_t>(willie_, "R Weapon Reverse Grip Type");
    const sync::GripSeat seat = sync::SeatFor(gripR, offJoint_ != nullptr, g.altGrip, g.mordhau, g.reverseR, revType ? *revType : 3);
    const char* const pointName = seat.point == sync::SeatPoint::Reverse ? "Grip R Hand Default Pos Reverse"
                                  : seat.point == sync::SeatPoint::Alt   ? "Grip R Hand Alt Pos"
                                                                         : "Grip R Hand Default Pos";
    UObject* const root = ObjField(weaponR, "BaseMesh");
    UObject* const point = ObjField(weaponR, pointName);
    Call par(point, "GetAttachParent");
    UObject* const parent = point && par.invoke() ? par.get<UObject*>("ReturnValue") : nullptr;
    const FVector* const relLoc = point ? Field<FVector>(point, "RelativeLocation") : nullptr;
    const FRotator* const relRot = point ? Field<FRotator>(point, "RelativeRotation") : nullptr;
    const char* const socket = seat.hammerSocket ? "weapon_rSocket_Hammer" : "weapon_rSocket";
    Call sock(mesh_, "GetSocketTransform");
    sock.setName("InSocketName", MakeName(socket)).set<uint8_t>("TransformSpace", kWorldSpace);
    FTransform keep;
    if (!root || !parent || !relLoc || !relRot || !sock.invoke() || !WorldTransform(root, keep)) {
        HSM_LOG("rig: %s: re-seat FAILED (no %s on %s)", ObjectName(willie_).c_str(), pointName, ObjectName(weaponR).c_str());
        return false;
    }
    const FTransform s = sock.get<FTransform>("ReturnValue");
    const Q4 flip = seat.flip == sync::SeatFlip::Yaw180     ? RotatorQuat(0, 180, 0)
                    : seat.flip == sync::SeatFlip::Pitch180 ? RotatorQuat(180, 0, 0)
                                                            : Q4{};
    Call(root, "SetSimulatePhysics").set<bool>("bSimulate", false).invoke();
    FTransform t = keep;
    t.translation = s.translation;
    t.rotation = ToFQuat(sync::QuatMul(ToQ4(s.rotation), flip));
    SetWorldTransform(root, t);
    FTransform p;
    if (WorldTransform(parent, p)) {   // turned so the point's rotation is the hand's ...
        t.rotation = ToFQuat(sync::QuatMul(ToQ4(p.rotation), RotatorQuat(-relRot->pitch, -relRot->yaw, -relRot->roll)));
        SetWorldTransform(root, t);
    }
    if (WorldTransform(parent, p)) {   // ... and moved so the point sits in it
        const V3 off = sync::QuatRotateVec(ToQ4(p.rotation), V3{-relLoc->x * p.scale.x, -relLoc->y * p.scale.y, -relLoc->z * p.scale.z});
        t.translation = FVector{p.translation.x + off.x, p.translation.y + off.y, p.translation.z + off.z};
        SetWorldTransform(root, t);
    }
    const bool ok = MakeGripJoint(r, root, socket, seat.lowerArm);
    SetWorldTransform(root, keep);
    Call(root, "SetSimulatePhysics").set<bool>("bSimulate", true).invoke();
    RearmChain(weapons_[0]);
    ChainPolicy(weapons_[0]);   // the chain's bodies were made again
    gripLocked_[0] = nullptr;   // locked again, and the polearm switch starts from the new seat
    switchOn_ = false;
    HSM_LOG("rig: %s: weapon re-seated in the right hand at %s%s%s", ObjectName(willie_).c_str(), pointName,
            seat.flip == sync::SeatFlip::None ? "" : ", turned over", ok ? "" : " (joint NOT remade)");
    return true;
}

// The right hand's joint made again with the weapon where it now is: the joint keeps that seat.
bool FighterRig::MakeGripJoint(UObject* joint, UObject* root, const char* socket, bool lowerArm) {
    Call(joint, "K2_AttachToComponent").set<UObject*>("Parent", mesh_).setName("SocketName", MakeName(socket))
        .set<uint8_t>("LocationRule", 0).set<uint8_t>("RotationRule", 0).set<uint8_t>("ScaleRule", 0)
        .set<bool>("bWeldSimulatedBodies", false).invoke();
    Call cc(joint, "SetConstrainedComponents");
    cc.set<UObject*>("Component1", root).setName("BoneName1", FName{}).set<UObject*>("Component2", mesh_)
        .setName("BoneName2", MakeName(lowerArm ? "lowerarm_r" : "hand_r"));
    return cc.invoke();
}

bool FighterRig::HasTwoHandPose() const {
    return twoHandJoints_[0].changed || twoHandJoints_[1].changed;
}

// A reference belongs to the saved constraint bodies, even if the Willie no longer points to that constraint.
// Failed reflection does not prove the constraint was destroyed/rebound: keep the native snapshot for retry.
sync::GripJointState FighterRig::TwoHandJointState(TwoHandJoint& saved) {
    if (ObjectCount() <= 0) return sync::GripJointState::Unavailable;
    if (!Live(saved.joint, saved.index) || !Live(saved.root, saved.rootIndex) || !Live(saved.mesh, saved.meshIndex))
        return sync::GripJointState::Gone;
    if (!saved.components.invoke(saved.joint)) return sync::GripJointState::Unavailable;
    UObject* component[2] = {saved.components.take<UObject*>(0), saved.components.take<UObject*>(2)};
    FName bone[2] = {saved.components.take<FName>(1), saved.components.take<FName>(3)};
    const int w = saved.weaponFrame, h = 1 - w;
    const bool same = component[w] == saved.root && bone[w].index == 0 && bone[w].number == 0 && component[h] == saved.mesh &&
                      bone[h].index == saved.handBone.index && bone[h].number == saved.handBone.number;
    return same ? sync::GripJointState::Same : sync::GripJointState::Gone;
}

bool FighterRig::TwoHandJointMatches(int hand, TwoHandJoint& saved) {
    return StillValid() && FieldAt(gripOffset_[hand]) == saved.joint &&
           TwoHandJointState(saved) == sync::GripJointState::Same;
}

bool FighterRig::BindTwoHandJoint(int hand, TwoHandJoint& out, const char*& failure) {
    failure = "joint-unavailable";
    UObject* const joint = FieldAt(gripOffset_[hand]);
    const Weapon& wp = weapons_[0];
    if (!joint || !Live(wp.root, wp.rootIndex)) return false;
    out = TwoHandJoint{};
    out.joint = joint; out.index = ObjectIndex(joint);
    out.root = wp.root; out.rootIndex = wp.rootIndex;
    out.mesh = mesh_; out.meshIndex = meshIndex_; out.handBone = bones_[kHandBone[hand]];
    const UClass* cls = ClassOf(joint);
    out.components = PreparedCall(cls, "GetConstrainedComponents", {"OutComponent1", "OutBoneName1", "OutComponent2", "OutBoneName2"});
    out.broken = PreparedCall(cls, "IsBroken", {"ReturnValue"});
    out.setFrame = PreparedCall(cls, "SetConstraintReferenceFrame", {"Frame", "RefFrame"});
    out.toWorld = PreparedCall(cls, "K2_GetComponentToWorld", {"ReturnValue"});
    out.rebind = PreparedCall(cls, "SetConstrainedComponents", {"Component1", "BoneName1", "Component2", "BoneName2"});
    if (!out.components.valid() || !out.broken.valid() || !out.setFrame.valid() || !out.toWorld.valid() || !out.rebind.valid() ||
        !ParamSize(out.components.function(), "OutComponent1", sizeof(UObject*)) ||
        !ParamSize(out.components.function(), "OutComponent2", sizeof(UObject*)) ||
        !ParamSize(out.components.function(), "OutBoneName1", sizeof(FName)) ||
        !ParamSize(out.components.function(), "OutBoneName2", sizeof(FName)) ||
        !ParamSize(out.broken.function(), "ReturnValue", sizeof(bool)) ||
        !ParamSize(out.setFrame.function(), "Frame", sizeof(uint8_t)) ||
        !ParamSize(out.setFrame.function(), "RefFrame", sizeof(FTransform)) ||
        !ParamSize(out.toWorld.function(), "ReturnValue", sizeof(FTransform)) ||
        !ParamSize(out.rebind.function(), "Component1", sizeof(UObject*)) ||
        !ParamSize(out.rebind.function(), "Component2", sizeof(UObject*)) ||
        !ParamSize(out.rebind.function(), "BoneName1", sizeof(FName)) ||
        !ParamSize(out.rebind.function(), "BoneName2", sizeof(FName)) || !out.components.invoke(joint)) return false;
    out.weaponFrame = out.components.take<UObject*>(0) == wp.root ? 0 : 1;
    if (!TwoHandJointMatches(hand, out) || !out.broken.invoke(joint) || out.broken.take<bool>(0)) return false;
    if (!out.toWorld.invoke(joint)) return false;
    out.scale = ToV3(out.toWorld.take<FTransform>(0).scale);
    if (!UniformGripScale(out.scale)) { failure = "unsupported-scale"; return false; }
    std::array<sync::GripFrame, 2> frames;
    // Validate reflection and frame contents without changing either joint. The caller prepares both hands before
    // the first rebind, so missing scale/metadata cannot leave only one hand taking the new path.
    return ReadGripFrames(joint, frames);
}

bool FighterRig::TwoHandScaleMatches(TwoHandJoint& saved) {
    if (!saved.toWorld.invoke(saved.joint)) return false;
    const V3 current = ToV3(saved.toWorld.take<FTransform>(0).scale);
    return UniformGripScale(current) && std::fabs(current.x - saved.scale.x) <= 1e-6 &&
           std::fabs(current.y - saved.scale.y) <= 1e-6 && std::fabs(current.z - saved.scale.z) <= 1e-6;
}

bool FighterRig::RebaseTwoHandJoint(int hand, TwoHandJoint& out) {
    if (!TwoHandJointMatches(hand, out) || !TwoHandScaleMatches(out) ||
        !out.broken.invoke(out.joint) || out.broken.take<bool>(0)) return false;
    UObject* component[2] = {out.root, out.mesh};
    FName bone[2] = {FName{}, out.handBone};
    if (out.weaponFrame == 1) { std::swap(component[0], component[1]); std::swap(bone[0], bone[1]); }
    // Same bodies/bones still rebuild the low-level physics joint. No body is moved and existing limits/drives are
    // retained, but the engine may wake bodies. This makes scale semantics unambiguous: UpdateConstraintFrames
    // divides positions by constraint scale; InitConstraint multiplies them into active world-unit local offsets.
    // Teardown restores this rebuilt baseline, not the pre-takeover native sliding/alternate-grip reference.
    out.rebind.put(0, component[0]).put(1, bone[0]).put(2, component[1]).put(3, bone[1]);
    if (!out.rebind.invoke(out.joint) || !TwoHandJointMatches(hand, out) || !TwoHandScaleMatches(out) ||
        !out.broken.invoke(out.joint) || out.broken.take<bool>(0)) return false;
    std::array<sync::GripFrame, 2> stored, frames;
    if (!ReadGripFrames(out.joint, stored) || !sync::ScaleGripFrames(stored, out.scale, frames)) return false;
    out.original = out.current = frames[out.weaponFrame];
    out.handFrame = frames[1 - out.weaponFrame];
    return true;
}

bool FighterRig::WriteTwoHandFrame(TwoHandJoint& saved, const sync::GripFrame& frame) {
    Q4 rotation;
    if (!sync::GripFrameRotation(frame, rotation)) return false;
    FTransform transform;
    transform.translation = ToF(frame.pos);
    transform.rotation = ToFQuat(rotation);
    saved.setFrame.put(0, saved.weaponFrame).put(1, transform);
    if (!saved.setFrame.invoke(saved.joint)) return false;
    saved.current = frame;
    saved.changed = true;   // even a later failed update must not lose this restoration obligation
    return true;
}

bool FighterRig::RestoreTwoHandJoint(TwoHandJoint& saved) {
    if (!saved.changed) { saved = TwoHandJoint{}; return true; }
    bool changed = saved.changed;
    const bool restored = sync::RestoreGripFrame(saved.original, changed, TwoHandJointState(saved),
        [&](const sync::GripFrame& frame) { return WriteTwoHandFrame(saved, frame); });
    saved.changed = changed;
    if (restored) saved = TwoHandJoint{};
    return restored;
}

std::vector<FighterRig::TwoHandJoint>& FighterRig::DeferredTwoHandRestores() {
    // Actor destruction can happen during process shutdown. Keep this queue alive until the process ends so it
    // cannot be destroyed before a session's actors, and never invoke engine calls from its own destructor.
    static auto* deferred = new std::vector<TwoHandJoint>;
    return *deferred;
}

bool FighterRig::RestoreDeferredTwoHandPoses(UObject* mesh, int32_t meshIndex) {
    auto& deferred = DeferredTwoHandRestores();
    bool clear = true;
    for (auto it = deferred.begin(); it != deferred.end();) {
        if (RestoreTwoHandJoint(*it)) it = deferred.erase(it);
        else {
            if (it->mesh == mesh && it->meshIndex == meshIndex) clear = false;
            ++it;
        }
    }
    return clear;
}

bool FighterRig::RestoreDeferredTwoHandPoseFor(UObject* willie, int32_t index) {
    if (DeferredTwoHandRestores().empty()) return true;
    if (ObjectCount() <= 0) return false;   // unavailable object identity cannot discharge a pending restoration
    const bool live = Live(willie, index);
    UObject* mesh = live ? ObjField(willie, "Mesh") : nullptr;
    if (live && !mesh) return false;   // do not mistake unavailable mesh reflection for an unrelated mesh
    return RestoreDeferredTwoHandPoses(mesh, mesh ? ObjectIndex(mesh) : -1);
}

bool FighterRig::RestoreTwoHandPose() {
    bool restored = true;
    for (TwoHandJoint& saved : twoHandJoints_) restored = RestoreTwoHandJoint(saved) && restored;
    return restored;
}

FighterRig::TwoHandResult FighterRig::ApplyTwoHandPose(const sync::PoseFrame& target, const sync::FighterNow& now,
                                                     uint32_t heldBones, bool stale, double nowMs) {
    TwoHandResult result;
    if (!StillValid()) { result.reason = "not-ready"; return result; }
    // Pausing must not restore a different seat into a pinned blade or grabbed arm. The caller also keeps the legacy
    // grip/re-seat writers out until the external constraint is gone. Stale streams hold their last relative pose.
    if (Pinned(0) || (heldBones & sync::kTwoHandArmBones) || stale) {
        result.reason = Pinned(0) ? "pinned" : (heldBones & sync::kTwoHandArmBones) ? "held-arm" : "stale";
        return result;
    }
    const Weapon& wp = weapons_[0];
    sync::TwoHandPosePolicy policy;
    policy.enabled = g_twoHandPose;
    policy.gripLock = g_gripLock;
    policy.localReady = wp.read && now.weaponValid[0] && offJoint_ && FieldAt(gripOffset_[1]) == offJoint_ &&
                        GripToLock(0) && GripToLock(1);
    policy.localClassHash = wp.classHash;
    policy.localFlail = wp.flail || wp.linked;
    policy.localMultipart = wp.head || !wp.parts.empty();
    policy.localLeftWeapon = FieldAt(weaponOffset_[1]) && FieldAt(weaponOffset_[1]) != wp.actor;
    policy.pinnedWeapons = pinned_; policy.heldBones = heldBones;
    for (int b = 0; b < sync::kPoseBones; ++b) if (now.valid[b]) policy.validBones |= uint32_t(1) << b;
    sync::TwoHandPoseReason reason = sync::TwoHandPoseStatus(target, policy);
    if (reason != sync::TwoHandPoseReason::Ready) {
        result.reason = sync::TwoHandPoseReasonName(reason);
        // The streamed off-hand flag can flicker during a swing. Keep the last target until ApplyGrip's existing
        // release hysteresis actually removes its joint; do not introduce a one-frame switch to a guessed seat.
        if (reason == sync::TwoHandPoseReason::NotTwoHanded && offJoint_ && target.weapons[0].present &&
            target.weapons[0].classHash == wp.classHash) return result;
        if (!RestoreTwoHandPose()) { result.reason = "restore-failed"; result.failed = true; }
        return result;
    }
    // The pose uses rigid world transforms. Positive uniform body scales include ordinary fighter heights; mirrored
    // or nonuniform bodies retain the legacy path because the packet cannot describe their altered local geometry.
    if (!UniformGripScale(wp.scale) || !UniformGripScale(handScale_[0]) || !UniformGripScale(handScale_[1])) {
        result.reason = "unsupported-scale";
        if (!RestoreTwoHandPose()) { result.reason = "restore-failed"; result.failed = true; }
        return result;
    }
    bool bound = true;
    for (int h = 0; h < 2; ++h) bound = TwoHandJointMatches(h, twoHandJoints_[h]) && bound;
    if (!bound) {
        if (!RestoreTwoHandPose()) { result.reason = "restore-failed"; result.failed = true; return result; }
        if (nowMs < twoHandRetryMs_) { result.reason = "joint-unavailable"; return result; }
        twoHandRetryMs_ = nowMs + 1000.0;
        std::array<TwoHandJoint, 2> fresh;
        const char* failure;
        if (!BindTwoHandJoint(0, fresh[0], failure) || !BindTwoHandJoint(1, fresh[1], failure)) {
            result.reason = failure; result.failed = true;
            return result;   // neither frame was changed
        }
        if (!RebaseTwoHandJoint(0, fresh[0]) || !RebaseTwoHandJoint(1, fresh[1])) {
            result.reason = "baseline-failed"; result.failed = true;
            return result;   // no pose override was applied; any rebuilt joint retains its native regenerated frames
        }
        twoHandJoints_ = std::move(fresh);
        twoHandRetryMs_ = 0;
    }
    // A runtime resize invalidates the captured conversion. Keep the last reference instead of guessing a new scale;
    // a new native grip starts a fresh baseline. The normal lifecycle still owns and restores the saved offsets.
    if (!TwoHandScaleMatches(twoHandJoints_[0]) || !TwoHandScaleMatches(twoHandJoints_[1])) {
        result.reason = "scale-changed";
        return result;
    }
    const std::array<sync::GripFrame, 2> handFrames = {twoHandJoints_[0].handFrame, twoHandJoints_[1].handFrame};
    std::array<sync::GripFrame, 2> desired;
    if (!sync::BuildTwoHandPose(target, policy, handFrames, desired, &reason)) {
        result.reason = sync::TwoHandPoseReasonName(reason);
        return result;   // a malformed target must never replace a previously valid frame
    }
    std::array<sync::GripFrame, 2> current = {twoHandJoints_[0].current, twoHandJoints_[1].current};
    const auto applied = sync::UpdateTwoHandFrames(current, desired, [&](size_t hand, const sync::GripFrame& frame) {
        TwoHandJoint& saved = twoHandJoints_[hand];
        return TwoHandJointMatches(static_cast<int>(hand), saved) && saved.broken.invoke(saved.joint) &&
               !saved.broken.take<bool>(0) && WriteTwoHandFrame(saved, frame);
    });
    result.active = result.updated = applied.applied;
    result.failed = !applied.applied;
    result.reason = applied.applied ? "pose" : applied.rollbackFailed ? "rollback-failed" : "write-failed";
    return result;
}

bool FighterRig::TwoHandErrors(const sync::PoseFrame& target, const sync::FighterNow& now, std::array<double, 5>& out) const {
    const Weapon& wp = weapons_[0];
    return wp.read && wp.haveTip && !wp.flail && !wp.linked && sync::TwoHandWeaponAccepted(wp.head || !wp.parts.empty(), wp.classHash) &&
           sync::MeasureTwoHandPose(target, now, wp.classHash, wp.tipPos, out);
}

std::string FighterRig::HeldWeaponName() const {
    UObject* const actor = weapons_[0].actor;
    return actor && Live(actor, weapons_[0].index) ? ObjectName(reinterpret_cast<UObject*>(ClassOf(actor))) : std::string("none");
}

// ApplySeat guesses the seat from the owner's toggles; the owner's game does not always re-seat (an alt grip on an axe
// slides the hand and keeps the weapon's turn), and the guess held the axe upside down (hsm_held.h). The pose says where
// the owner's weapon sits in the owner's hand: when this stand-in's differs for long enough, the weapon goes there and
// the joint is made again, as ApplySeat does. The hand switch moves the joints' reference positions (ApplySwitch) and a
// shield (grips 6, 7) is held by the forearm: neither is re-seated here.
void FighterRig::ApplyHeld(const sync::PoseFrame& target, const sync::FighterNow& now, double nowMs) {
    if (!StillValid()) return;
    if (HasTwoHandPose()) { heldCheck_.Reset(); return; }   // active or paused: the pose owns these references
    UObject* const r = FieldAt(gripOffset_[0]);
    UObject* const weaponR = FieldAt(weaponOffset_[0]);
    if (r != heldJoint_ || weaponR != heldWeapon_) { heldJoint_ = r; heldWeapon_ = weaponR; heldCheck_.Forget(); }
    const Weapon& wp = weapons_[0];
    const sync::WeaponSample& ow = target.weapons[0];
    const uint8_t gripR = gripTypeOffset_[0] >= 0 ? reinterpret_cast<const uint8_t*>(willie_)[gripTypeOffset_[0]] : 0;
    const bool usable = r && weaponR && wp.actor == weaponR && wp.root && r == gripLocked_[0] && ow.present && ow.hand == 0 &&
                        (!ow.classHash || !wp.classHash || ow.classHash == wp.classHash) && now.weaponValid[0] &&
                        now.valid[sync::kBoneHandR] && !target.grip.switched2H && gripR != 6 && gripR != 7;
    if (!usable) { heldCheck_.Reset(); return; }
    const double steadyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (steadyMs - offWantSinceMs_ < kOffhandSettleMs) { heldCheck_.Reset(); return; }   // ApplyGrip's clock
    const sync::BoneSample& oh = target.bones[sync::kBoneHandR];
    const sync::BodyNow& sh = now.bones[sync::kBoneHandR];
    const sync::HeldPose owner = sync::HeldIn(oh.pos, oh.rot, ow.pos, ow.rot);
    const sync::HeldPose mine = sync::HeldIn(sh.pos, sh.rot, now.weapons[0].pos, now.weapons[0].rot);
    // A spear or staff held mid-shaft may sit slid along the shaft (hsm_held.h HeldToleranceForShaft).
    const sync::HeldTolerance tol = sync::HeldToleranceForShaft(sync::HeldToleranceFor(wp.flail != nullptr), owner, oh.rot, ow.base, ow.tip);
    sync::HeldTolerance twoHand = tol;
    twoHand.deg = kHeldTwoHandDeg; twoHand.uu = kHeldTwoHandUu;
    if (offJoint_ && !sync::HeldDiffers(owner, mine, twoHand)) {
        heldCheck_.Reset();
        return;
    }
    if (!heldCheck_.Update(nowMs, owner, mine, tol)) return;
    FTransform keep;
    if (!WorldTransform(wp.root, keep)) return;
    V3 pos;
    Q4 rot;
    sync::HeldWorld(sh.pos, sh.rot, owner, &pos, &rot);
    const sync::GripSeat seat = sync::SeatFor(gripR, offJoint_ != nullptr, false, false, false, 3);   // socket and bone only
    Call(wp.root, "SetSimulatePhysics").set<bool>("bSimulate", false).invoke();
    FTransform t = keep;
    t.translation = ToF(pos);
    t.rotation = ToFQuat(rot);
    SetWorldTransform(wp.root, t);
    const bool ok = MakeGripJoint(r, wp.root, seat.hammerSocket ? "weapon_rSocket_Hammer" : "weapon_rSocket", seat.lowerArm);
    SetWorldTransform(wp.root, keep);
    Call(wp.root, "SetSimulatePhysics").set<bool>("bSimulate", true).invoke();
    RearmChain(wp);
    ChainPolicy(wp);            // the chain's bodies were made again
    gripLocked_[0] = nullptr;   // locked again on the next Drive
    switchOn_ = false;
    // The off hand's joint stays (0.8.0-pre7): it holds hand_l to the shaft, which the right hand's new joint now carries
    // to the owner's seat; breaking and re-attaching it (pre6) swung a polearm by the length between the hands (#74, #79).
    HSM_LOG("rig: %s: weapon re-seated in the right hand as the owner holds it (%.0f deg, %.1f uu off%s)%s",
            ObjectName(willie_).c_str(), sync::QuatAngleDeg(owner.rot, mine.rot), sync::Vec3Dist(owner.pos, mine.pos),
            tol.slideUu > 0.0 ? ", mid-shaft" : "", ok ? "" : " (joint NOT remade)");
}

void FighterRig::SetBodyPolicy(UObject* body) {
    // Gravity off and world geometry ignored: the servo carries every body, and a floor or step must not snag it.
    // Pawn, body (armour: PhysicsBody) and weapon (custom channel 14) contacts stay, so blades and fists still meet it.
    Call(body, "SetEnableGravity").set<bool>("bGravityEnabled", false).invoke();
    Call(body, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldStatic).set<uint8_t>("NewResponse", kIgnore).invoke();
    Call(body, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldDynamic).set<uint8_t>("NewResponse", kIgnore).invoke();
}

// A flail chain on a stand-in, as SetBodyPolicy, and passing through fighters' bodies and armour: its head is driven by
// velocity (no force limit), so a chain wrapped round the local fighter dragged them after the owner's swing ("flails
// tug on you"). Hits are judged on the owner's game, so nothing is lost; weapons still meet it (parries, binds).
void FighterRig::SetChainPolicy(UObject* chain) {
    SetBodyPolicy(chain);
    Call(chain, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kPawn).set<uint8_t>("NewResponse", kIgnore).invoke();
    Call(chain, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kPhysicsBody).set<uint8_t>("NewResponse", kIgnore).invoke();
}

void FighterRig::NeutraliseFull() {
    // Joint motors and physical animation: the physics asset's own drives pull the stand-in towards its animation
    // pose (HSMP measured 97 -> 9 deg head error with the motors zeroed). Every value is zero, so the float/double
    // width of these parameters does not matter.
    Call(mesh_, "SetAllMotorsAngularDriveParams").set<float>("InSpring", 0.f).set<float>("InDamping", 0.f)
        .set<float>("InForceLimit", 0.f).set<bool>("bSkipCustomPhysicsType", false).invoke();
    if (UObject* pa = ObjField(willie_, "PhysicalAnimation"))
        Call(pa, "SetStrengthMultiplyer").set<float>("InStrengthMultiplyer", 0.f).invoke();
    if (FObjArray* arr = Field<FObjArray>(willie_, "Phys Anim Array"); arr && arr->num > 0 && arr->num <= 64)
        for (int32_t i = 0; i < arr->num; ++i)
            if (arr->data[i]) Call(arr->data[i], "SetStrengthMultiplyer").set<float>("InStrengthMultiplyer", 0.f).invoke();
    // The BP's own physics handles (get-up, body pulls) let go.
    for (const char* f : kHandles) {
        UObject* h = ObjField(willie_, f);
        if (!h) continue;
        Call g(h, "GetGrabbedComponent");
        if (g.invoke() && g.get<UObject*>("ReturnValue")) Call(h, "ReleaseComponent").invoke();
    }
    // The pelvis-to-capsule constraints: broken once, the capsule is placed separately.
    for (const char* f : kRootConstraints) {
        UObject* c = ObjField(willie_, f);
        if (!c || std::find(brokenRoots_.begin(), brokenRoots_.end(), c) != brokenRoots_.end()) continue;
        Call(c, "BreakConstraint").invoke();
        brokenRoots_.push_back(c);
    }
    if (!savedPolicy_) {
        savedPolicy_ = true;
        for (int i = 0; i < 2; ++i) {
            Call g(mesh_, "GetCollisionResponseToChannel");
            g.set<uint8_t>("Channel", i == 0 ? kWorldStatic : kWorldDynamic);
            if (g.invoke()) savedMeshResponse_[i] = g.get<uint8_t>("ReturnValue");
        }
        Call grav(mesh_, "IsGravityEnabled");
        if (grav.invoke()) savedGravity_ = grav.get<bool>("ReturnValue");
        auto enabled = [](UObject* c, uint8_t& out) {
            Call g(c, "GetCollisionEnabled");
            if (c && g.invoke()) out = g.get<uint8_t>("ReturnValue");
        };
        enabled(ObjField(willie_, "BoneCore"), savedCore_);
        Call root(willie_, "K2_GetRootComponent");
        enabled(root.invoke() ? root.get<UObject*>("ReturnValue") : nullptr, savedCapsule_);
        for (size_t i = 0; i < std::size(kHiddenParts); ++i) enabled(ObjField(willie_, kHiddenParts[i]), savedHidden_[i]);
        if (UObject* box = ObjField(willie_, "Box1")) {
            Call g(box, "GetCollisionResponseToChannel");
            g.set<uint8_t>("Channel", kBodyCollision);
            if (g.invoke()) savedBox_ = g.get<uint8_t>("ReturnValue");
        }
    }
    SetBodyPolicy(mesh_);
    for (Weapon& wp : weapons_)   // as of the last Read (Neutralise runs before this frame's): checked live
        if (Live(wp.actor, wp.index) && Live(wp.root, wp.rootIndex)) {
            // 0.8.0-pre7: a held weapon passes through fighters' bodies and armour like a chain (SetChainPolicy): its root
            // is velocity-driven with no force limit, and a polearm against the local fighter dragged them after the
            // owner's swing (#68 "dragged by a poleaxe", #70/#76 "got flung"). Blows are judged on the owner's game and
            // weapons still meet it (parries, binds); a dropped one gets its responses back (ObjectSync::RestorePhysics).
            SetChainPolicy(wp.root);
            // The flail chain follows the handle's policy (every body of the component): gravity and world geometry left
            // on sagged it and snagged it on the floor while the handle and body ignored both.
            ChainPolicy(wp);
        }
    CollisionOff(ObjField(willie_, "BoneCore"));   // HSMP main.lua:4673
    Call rc(willie_, "K2_GetRootComponent");
    CollisionOff(rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr);   // the capsule only marks the position
    if (g_partsOff) {
        for (const char* f : kHiddenParts) CollisionOff(ObjField(willie_, f));
        // Box1 has a joint to spine_03: a response, not its collision, so its physics stay as they were.
        if (UObject* box = ObjField(willie_, "Box1"))
            Call(box, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kBodyCollision).set<uint8_t>("NewResponse", kIgnore).invoke();
    }
    Call mv(willie_, "GetMovementComponent");
    if (UObject* m = mv.invoke() ? mv.get<UObject*>("ReturnValue") : nullptr) Call(m, "DisableMovement").invoke();
    // Bones refreshed from physics even off screen, or the servo reads a stale pose (HSMP main.lua:4672).
    if (FProperty* p = FindProperty(ClassOf(mesh_), "VisibilityBasedAnimTickOption"); p && PropertySize(p) == 1)
        *(reinterpret_cast<uint8_t*>(mesh_) + PropertyOffset(p)) = kAlwaysTickPose;
    gripLocked_[0] = gripLocked_[1] = nullptr;   // and locked again: a new grip joint after a re-grip
    gripFreed_[0] = gripFreed_[1] = nullptr;   // the grips are freed again on the next Drive: the BP may re-lock them
    slots_.Maintain(willie_);   // the sheathed weapons: diagnosis line (standin_slots.h)
}

bool FighterRig::Revive() {
    // No native fields or control resume until the remote pose's references have been restored. A transient read
    // or write failure leaves this bound rig intact for the next hand-off attempt.
    if (!RestoreTwoHandPose()) return false;
    if (!StillValid()) { Unbind(); return true; }
    auto* base = reinterpret_cast<uint8_t*>(willie_);
    if (savedFields_) {
        for (size_t i = 0; i < zeroDoubles_.size() && i < savedDoubles_.size(); ++i) *reinterpret_cast<double*>(base + zeroDoubles_[i]) = savedDoubles_[i];
        for (size_t i = 0; i < trueBools_.size() && i < savedTrue_.size(); ++i) SetBoolValue(trueBools_[i], base + PropertyOffset(trueBools_[i]), savedTrue_[i]);
        for (size_t i = 0; i < falseBools_.size() && i < savedFalse_.size(); ++i) SetBoolValue(falseBools_[i], base + PropertyOffset(falseBools_[i]), savedFalse_[i]);
    }
    Call(mesh_, "SetConstraintProfileForAll").setName("ProfileName", MakeName("Motor")).set<bool>("bDefaultIfNotFound", false).invoke();
    if (UObject* pa = ObjField(willie_, "PhysicalAnimation"))
        Call(pa, "SetStrengthMultiplyer").set<float>("InStrengthMultiplyer", 1.f).invoke();
    if (FObjArray* arr = Field<FObjArray>(willie_, "Phys Anim Array"); arr && arr->num > 0 && arr->num <= 64)
        for (int32_t i = 0; i < arr->num; ++i)
            if (arr->data[i]) Call(arr->data[i], "SetStrengthMultiplyer").set<float>("InStrengthMultiplyer", 1.f).invoke();
    Call(mesh_, "SetEnableGravity").set<bool>("bGravityEnabled", savedGravity_).invoke();
    Call(mesh_, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldStatic).set<uint8_t>("NewResponse", savedMeshResponse_[0]).invoke();
    Call(mesh_, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldDynamic).set<uint8_t>("NewResponse", savedMeshResponse_[1]).invoke();
    // Held weapons: the roots and chains back to a dropped weapon's responses (ObjectSync::RestorePhysics), then the
    // weapon's own re-arm for its parts.
    for (Weapon& wp : weapons_) RearmDropped(wp);
    if (UObject* core = ObjField(willie_, "BoneCore")) Call(core, "SetCollisionEnabled").set<uint8_t>("NewType", savedCore_).invoke();
    Call rc(willie_, "K2_GetRootComponent");
    UObject* capsule = rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr;
    if (capsule) Call(capsule, "SetCollisionEnabled").set<uint8_t>("NewType", savedCapsule_).invoke();
    if (g_partsOff)
        for (size_t i = 0; i < std::size(kHiddenParts); ++i)
            if (UObject* part = savedHidden_[i] != 0xff ? ObjField(willie_, kHiddenParts[i]) : nullptr)
                Call(part, "SetCollisionEnabled").set<uint8_t>("NewType", savedHidden_[i]).invoke();
    if (UObject* box = g_partsOff ? ObjField(willie_, "Box1") : nullptr)
        Call(box, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kBodyCollision).set<uint8_t>("NewResponse", savedBox_).invoke();
    Call mv(willie_, "GetMovementComponent");
    if (UObject* m = mv.invoke() ? mv.get<UObject*>("ReturnValue") : nullptr)
        Call(m, "SetMovementMode").set<uint8_t>("NewMovementMode", 1).set<uint8_t>("NewCustomMode", 0).invoke();   // MOVE_Walking
    int joined = 0;
    for (UObject* c : brokenRoots_)
        if (c && capsule && Call(c, "SetConstrainedComponents").set<UObject*>("Component1", capsule).setName("BoneName1", MakeName("None"))
                 .set<UObject*>("Component2", mesh_).setName("BoneName2", MakeName("pelvis")).invoke()) ++joined;
    HSM_LOG("rig: %s revived (fields %s, %d root constraint(s) made again, capsule collision %u)", ObjectName(willie_).c_str(),
            savedFields_ ? "restored" : "never neutralised", joined, unsigned(savedCapsule_));
    Unbind();
    return true;
}

// A weapon's bodies back to a dropped weapon's physics (ObjectSync::RestorePhysics): gravity, every channel blocked, then
// the weapon's own re-arm for its parts. For the weapons of a revived fighter and one a neutralised fighter let go.
void FighterRig::RearmDropped(const Weapon& wp) {
    if (!Live(wp.actor, wp.index) || wp.fists) return;
    constexpr uint8_t kBlock = 2;
    std::vector<UObject*> bodies;
    if (Live(wp.root, wp.rootIndex)) bodies.push_back(wp.root);
    for (size_t i = 0; i < wp.parts.size(); ++i) if (Live(wp.parts[i], wp.partIndex[i])) bodies.push_back(wp.parts[i]);
    if (Live(wp.flail, wp.flailIndex)) bodies.push_back(wp.flail);
    for (UObject* b : bodies) {
        Call(b, "SetEnableGravity").set<bool>("bGravityEnabled", true).invoke();
        for (uint8_t ch : {kWorldStatic, kWorldDynamic, kPawn, kPhysicsBody})
            Call(b, "SetCollisionResponseToChannel").set<uint8_t>("Channel", ch).set<uint8_t>("NewResponse", kBlock).invoke();
    }
    Call(wp.actor, "Turn On Collision").invoke();
    HSM_LOG("rig: %s: %s re-armed as a dropped weapon (%zu bodies)", ObjectName(willie_).c_str(), ObjectName(wp.actor).c_str(), bodies.size());
}

void SetStandInGripLock(bool on) { g_gripLock = on; }
void SetStandInPartsOff(bool on) { g_partsOff = on; }
void SetStandInTwoHandPose(bool on) { g_twoHandPose = on; }
void SetStandInTwoHandLock(bool on) { g_twoHandLock = on; }
void SetStandInTwoHandAudit(bool on) { g_twoHandAudit = on; }

}  // namespace hsm::game
