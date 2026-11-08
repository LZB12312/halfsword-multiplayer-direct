// One fighter's physics bodies bound for per-frame reading and driving: the 23 pose bones of its Mesh and the held
// weapons' root bodies, with every per-frame engine call resolved once (ue::PreparedCall). The owner side reads a
// fighter into the pose stream (hsm_pose.h); the stand-in side reads its own bodies for the servo (hsm_standin.h
// FighterNow), applies the servo's velocity commands, moves itself rigidly on a placement, and keeps the Willie's own
// drives switched off (Neutralise). Game thread only.
//
// The stand-in policy follows Cyrex0/HalfSword-MP mods/HSMPAvatars/Scripts/main.lua (neutralise_bp, the grip
// constraints, snap_mesh, drive_root, the weapon-root gravity/collision policy), Copyright (c) 2026 Cyrex0 and the
// HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md). Differences: the stand-in's own Willie ReceiveTick is
// blocked outright (session.cpp), so nothing re-arms what Neutralise switched off between passes; the pelvis-to-capsule
// root constraints are broken instead of having their drive powers zeroed (the powers only reach the constraint through
// the blocked tick); a freed grip is never restored (its template limits live in the native ConstraintInstance, which
// this layer does not read), so a held weapon that stops being driven after its grip was freed is held still instead.
#pragma once
#include "gameplay/standin_slots.h"
#include "sync/hsm_held.h"
#include "sync/hsm_standin.h"
#include "sync/hsm_two_hand_pose.h"
#include "sync/hsm_two_hand_update.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm::game {

class FighterRig {
public:
    static constexpr int kSheathSlots = 5;   // "Weapon Slot R 1" ... "Weapon Slot L 2"
    // Binds the fighter's Mesh and bones (a different or respawned fighter rebinds everything). False when it has no
    // Mesh, a pose bone is missing or a per-frame call does not resolve; the rig is then unbound.
    bool Bind(ue::UObject* willie);
    void Unbind();
    // A replacement Actor may be unbound while an earlier rig still owes this mesh its native grip references.
    // Hand-off must wait for that deferred restoration too, before native input or simulation resumes.
    static bool RestoreDeferredTwoHandPoseFor(ue::UObject* willie, int32_t index);
    bool Bound() const { return willie_ != nullptr; }
    ue::UObject* Willie() const { return willie_; }
    bool StillValid() const;                       // the bound Willie and its Mesh are still the live objects they were

    // Reads this frame's physics state. frame (optional): every bone with velocities and the held weapons (slot 0 =
    // "Weapon R", hand 0; slot 1 = "Weapon L", hand 1), as the owner streams them. now (optional): the bodies the
    // servo drives (bone transforms, weapon roots, centres of mass, the contact bodies' velocities), slot for slot.
    // False if the rig is not valid.
    bool Read(uint32_t tsMs, uint16_t stepUs, sync::PoseFrame* frame, sync::FighterNow* now);
    // Blade tip of the weapon in slot w as of the last Read (false when that slot was not read or has no tip).
    bool WeaponTip(int w, sync::V3& out) const;

    // Sets every commanded body's linear and angular velocity. Frees a hand's grip constraint once the weapon it holds
    // is driven (or the hand holds fists or nothing), as HSMP does; a held weapon whose grip is free but which is not
    // driven this frame is held still. A flail head is driven whenever commanded and not pinned, also while a locked
    // hand carries its weapon: the chain between them is free.
    void Drive(const sync::FighterCommand& cmd);
    // Weapon slots (bit w = slot w) another fighter holds with a stuck-blade joint (Severs::Hold): neither driven, held
    // still nor moved by Shift, so only the joint and the struck body move them. Set every frame before Shift and Drive.
    void SetPinnedWeapons(uint32_t slots) { pinned_ = slots; }
    // Moves the whole fighter rigidly so that the Mesh and the held weapon roots read this frame end where they were
    // read plus d (teleported, all velocities zeroed), whatever moved them since the Read. Call after PlaceCapsule.
    void Shift(const sync::V3& d);
    // Keeps the (collision-free) capsule with the body for gameplay queries (HSMP drive_root). Teleports only for jumps
    // over 150 uu, which may carry the Mesh along: on a placement call it before Shift, as HSMP does. yawDeg (optional)
    // also turns the capsule (pitch and roll 0). Unlike HSMP the turn never teleports, so it cannot swing the
    // simulating bodies around the capsule (Shift undoes a carried translation, not a rotation).
    void PlaceCapsule(const sync::V3& at, const double* yawDeg = nullptr);

    // Switches the Willie's own pulls off: tonus, muscles, pain reactions and damage every call (field writes); joint
    // motors, physical animation, physics handles, root constraints, gravity, world collision, BoneCore and capsule
    // collision on a schedule (every 0.5 s for the first 6 s after binding, then every 2 s, and after a weapon change).
    void Neutralise(double nowMs);
    // Hand-off (sync/hsm_handoff.h): undoes Neutralise, as the game's own start-up arms a fighter (Willie_BP BeginPlay,
    // W:14190-14212): the fields as Neutralise first found them, the joint motors' profile, physical animation, gravity
    // and world collision, the held weapons' collision (ModularWeaponBP Turn On Collision), BoneCore and capsule
    // collision, walking, and the pelvis-to-capsule constraints made again where the capsule stands. The session stops
    // blocking the Willie's tick, which takes over from the next frame. Then Unbind: nothing here drives it any more.
    // False leaves the rig bound and neutralised until a saved grip reference can be restored safely.
    bool Revive();

    // Stand-in: writes the owner's grip state into this Willie, so its fingers and animation follow the owner's hands (the
    // left hand of a two-handed grip never holds a Weapon L; the game marks it with L Hand In Offhand Attached). The two
    // grip types go through sync::GripToApply: a hand that holds a weapon here never gets 0, which would destroy the
    // weapon. Call every frame after Neutralise and Drive. Logs one line per change.
    void ApplyGrip(const sync::GripState& g);
    void ApplySwitch(bool on, ue::UObject* weaponR);   // the polearm hand switch's joint positions
    bool ApplySeat(const sync::GripState& g, ue::UObject* weaponR);   // the right hand's seat on the weapon (hsm_grip.h)
    // Stand-in: when this weapon has sat in the right hand differently from the owner's for a while (hsm_held.h), the joint
    // is made again at the owner's seat, read from the target pose's hand_r and weapon. Call after ApplyGrip.
    void ApplyHeld(const sync::PoseFrame& target, const sync::FighterNow& now, double nowMs);
    // Uses the sampled owner's hand/weapon relationship for both existing grip joints. The right hand still carries
    // the weapon, and the weapon carries the off hand without competing velocity drives. Takeover rebuilds both
    // existing joints once to capture scaled baseline frames. A suspended grip keeps its last target while locally
    // grabbed, pinned or stale. Legacy seat/switch changes restore that regenerated baseline first.
    struct TwoHandResult {
        const char* reason = "not-two-handed";
        bool active = false, updated = false, failed = false;
    };
    TwoHandResult ApplyTwoHandPose(const sync::PoseFrame& target, const sync::FighterNow& now,
                                  uint32_t heldBones, bool stale, double nowMs);
    bool HasTwoHandPose() const;
    std::string HeldWeaponName() const;   // the class in the main hand, for the log
    bool TwoHandErrors(const sync::PoseFrame& target, const sync::FighterNow& now, std::array<double, 5>& out) const;

private:
    sync::GripState ReadGrip() const;             // the bound Willie's own grip variables (the owner's side)
    struct Weapon {
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        ue::UObject* root = nullptr;           // the actor's root component: the simulating body
        int32_t rootIndex = -1;
        ue::UObject* base = nullptr;           // "Root Scene"
        int32_t baseIndex = -1;
        ue::UObject* tip = nullptr;            // "TippyTipScene"
        int32_t tipIndex = -1;
        bool fists = false;
        bool comMeasured = false;
        int comTries = 0;
        uint32_t classHash = 0;
        sync::V3 comLocal{};
        ue::PreparedCall toWorld, simulating, linVelAt, angVel, setLin, setAng, com, baseLoc, tipLoc;
        // A flail's chain: the SkeletalMeshComponent tagged "Flail" (ModularWeaponBP), whose bodies below Joint1 simulate.
        // The head is the chain's last simulating body, looked up once the chain simulates.
        ue::UObject* flail = nullptr;
        int32_t flailIndex = -1;
        ue::FName headBone{};
        bool haveHeadBone = false;
        int headTries = 0;
        ue::PreparedCall headSocket, headSim, headLinVelAt, headAngVel, headSetLin, headSetAng;
        // 0.8.0-pre7: the game's flails (Flail_A..D) have no skeletal chain. Their links and head are the actor's own
        // simulating primitive components (the head is 'Blade1': claims log "by weapon R 'Blade1'"), joined by
        // constraints; neither lookup above ever matched ("flail no" on every machine), so the chain was never streamed,
        // never shifted with the body and never made to pass through fighters. Found once they simulate (a sheathed
        // chain is kinematic): the head is the 'Blade*' part, else the part farthest from the handle, driven through
        // headSim/headLinVelAt/headAngVel/headSetLin/headSetAng with no bone name.
        bool linked = false;                   // a part named Link: a real chain of joined bodies (ScanParts)
        std::vector<ue::UObject*> parts;
        std::vector<int32_t> partIndex;
        ue::UObject* head = nullptr;
        int32_t headIndex = -1;
        ue::PreparedCall headToWorld;
        // this frame
        bool read = false, haveTip = false, driven = false;
        bool wasHeld = false, farFromHand = false;   // read last frame / this frame's read failed for being out of the hand's reach
        sync::V3 pos{}, basePos{}, tipPos{}, scale{1, 1, 1};
        sync::Q4 rot{};
    };
    ue::UObject* FieldAt(int32_t offset) const;   // UObject* field of the bound Willie at a cached offset
    void RefreshWeapon(int w);                    // re-resolves slot w when its field changed
    bool ReadWeapon(int w, const sync::V3& handPos);
    void CheckHandLoss(int w, const Weapon& wp);  // #249: logs the own live fighter's weapon leaving its hand unasked
    bool ReadHead(Weapon& wp, sync::BoneSample& out);   // the flail head of a held weapon, world
    void ScanParts(Weapon& wp);                         // a component chain's simulating parts and head (Weapon::parts)
    ue::UObject* HeadBody(const Weapon& wp) const;      // the component the head is read from and driven on, or nullptr
    ue::UObject* ChainBody(const Weapon& wp) const;     // one body of the chain, to check its collision policy, or nullptr
    void ChainPolicy(const Weapon& wp);                 // SetChainPolicy on every body of the chain
    void RearmChain(const Weapon& wp);                  // RearmFlail, and a component chain simulating again and still
    void RearmDropped(const Weapon& wp);                // gravity and every channel back: a dropped weapon's physics
    bool GripWanted(int hand) const;
    bool GripLoose(int hand) const;
    void FreeGrip(int hand);
    bool GripToLock(int hand) const;              // standin_grip=lock: this hand holds a real weapon (or the off hand of a two-handed grip)
    void LockGrip(int hand);                      // the game's firm grip: every limit of the hand's grip joint locked
    bool HoldOffhand(ue::UObject* joint);         // the off hand's firm grip: the game's own limits, sliding along the shaft
    void HoldStill(Weapon& wp);
    bool Pinned(int w) const { return (pinned_ >> w) & 1u; }
    void NeutraliseFull();
    void SetBodyPolicy(ue::UObject* body);
    void SetChainPolicy(ue::UObject* chain);   // SetBodyPolicy, and through fighters' bodies (a flail's chain)
    struct TwoHandJoint {
        ue::UObject* joint = nullptr;
        int32_t index = -1;
        ue::UObject* root = nullptr;
        int32_t rootIndex = -1;
        ue::UObject* mesh = nullptr;
        int32_t meshIndex = -1;
        ue::FName handBone{};
        uint8_t weaponFrame = 0;
        sync::GripFrame original, handFrame, current;
        sync::V3 scale{};
        ue::PreparedCall components, broken, setFrame, toWorld, rebind;
        bool changed = false;
    };
    bool BindTwoHandJoint(int hand, TwoHandJoint& out, const char*& failure);
    bool RebaseTwoHandJoint(int hand, TwoHandJoint& joint);
    static bool TwoHandScaleMatches(TwoHandJoint& joint);
    bool TwoHandJointMatches(int hand, TwoHandJoint& joint);
    static sync::GripJointState TwoHandJointState(TwoHandJoint& joint);
    static bool WriteTwoHandFrame(TwoHandJoint& joint, const sync::GripFrame& frame);
    static bool RestoreTwoHandJoint(TwoHandJoint& joint);
    static std::vector<TwoHandJoint>& DeferredTwoHandRestores();
    static bool RestoreDeferredTwoHandPoses(ue::UObject* mesh, int32_t meshIndex);
    bool RestoreTwoHandPose();
    std::array<TwoHandJoint, 2> twoHandJoints_{};
    std::array<sync::V3, 2> handScale_{};
    double twoHandRetryMs_ = 0;

    ue::UObject* willie_ = nullptr;
    int32_t willieIndex_ = -1;
    ue::UObject* mesh_ = nullptr;
    int32_t meshIndex_ = -1;
    std::array<ue::FName, sync::kPoseBones> bones_{};
    std::array<sync::V3, sync::kPoseBones> comLocal_{};
    bool comMeasured_ = false;
    int comTries_ = 0;
    ue::PreparedCall socket_, linVelAt_, linVel_, angVel_, setLin_, setAng_, com_;
    ue::PreparedCall placeActor_, actorLoc_, turnActor_;
    std::array<Weapon, sync::kPoseWeapons> weapons_{};
    int32_t weaponOffset_[sync::kPoseWeapons] = {-1, -1};   // "Weapon R" / "Weapon L" in the Willie
    int32_t gripOffset_[2] = {-1, -1};                       // "PhysicsConstraint R Hand" / "L Hand"
    int32_t slotOffset_[kSheathSlots] = {-1, -1, -1, -1, -1};
    std::array<ue::UObject*, kSheathSlots> slotSeen_{};      // what each slot held at the last Neutralise
    ue::UObject* gripFreed_[2] = {nullptr, nullptr};         // freed per hand since the last full pass (null: free it again)
    ue::UObject* gripLocked_[2] = {nullptr, nullptr};        // locked per hand since the last full pass (null: lock it again)
    bool poseHeld_[2] = {false, false};                      // the two-hand pose owned this hand's joint at the last Drive
    ue::UObject* gripLoose_[2] = {nullptr, nullptr};         // freed at all (limits never restored), until a weapon change
    int32_t gripLooseIndex_[2] = {-1, -1};
    ue::UObject* gripLogged_[2] = {nullptr, nullptr};
    std::vector<int32_t> zeroDoubles_;                       // offsets of the zeroed double fields
    std::vector<ue::FProperty*> trueBools_, falseBools_;
    int32_t gripTypeOffset_[2] = {-1, -1};                   // "R_GripType_Current" / "L_GripType_Current" (ByteProperty)
    ue::FProperty* gripFlag_[5] = {};                        // offhand, alt, mordhau, switched, reverse (BoolProperty)
    sync::GripState gripStateLogged_{};                      // the grip state ApplyGrip last logged
    bool gripStateKnown_ = false;
    int ownToggles_ = -1;                                    // the owner's grip toggles last logged when sending (-1 none)
    ue::UObject* offJoint_ = nullptr;                        // the off hand's joint Attach L Hand To Offhand Position made
    double offTryMs_ = -1e18;                                // its last attempt (kOffhandRetryMs apart)
    ue::UObject* switchJoint_ = nullptr;                     // the right hand joint ApplySwitch last set
    bool switchOn_ = false;                                  // ... and whether it set it switched
    ue::UObject* switchOffJoint_ = nullptr;                  // the off-hand joint ApplySwitch last moved
    bool offWant_ = false;                                   // the owner's offhand grip as last seen ...
    double offWantSinceMs_ = 0;                              // ... and since when (kOffhandAttachMs / kOffhandReleaseMs)
    ue::UObject* seatJoint_ = nullptr;                       // the right hand joint ApplySeat last seated
    uint8_t seatKey_ = 0;                                    // ... and the owner's toggles it was seated for (SeatKey)
    sync::HeldCheck heldCheck_;                              // ApplyHeld: the stand-in's seat against the owner's
    ue::UObject* heldJoint_ = nullptr;                       // ... for this right hand joint
    ue::UObject* heldWeapon_ = nullptr;                      // ... and this right hand weapon
    bool MakeGripJoint(ue::UObject* joint, ue::UObject* root, const char* socket, bool lowerArm);   // right hand, at the socket
    double boundAtMs_ = -1.0, nextFullMs_ = 0.0;
    bool weaponChanged_ = false;
    bool sendsFrames_ = false;                               // the last Read streamed a pose: the player's own live fighter
    double lastReadMs_ = -1.0, readGapMs_ = 0.0;             // steady clock of the last Read / the gap before it
    uint32_t pinned_ = 0;                                    // SetPinnedWeapons
    std::vector<ue::UObject*> brokenRoots_;                  // root constraints already broken
    // What Neutralise found before its first write (Revive puts it back).
    std::vector<double> savedDoubles_;
    std::vector<bool> savedTrue_, savedFalse_;
    bool savedFields_ = false, savedPolicy_ = false;
    uint8_t savedMeshResponse_[2] = {2, 2};                  // WorldStatic, WorldDynamic (ECR_Block)
    uint8_t savedCore_ = 3, savedCapsule_ = 1;               // ECollisionEnabled: BoneCore QueryAndPhysics, the capsule QueryOnly
    uint8_t savedHidden_[2] = {0xff, 0xff};                  // kHiddenParts, as they were (standin_parts_off); 0xff not read
    uint8_t savedBox_ = 2;                                   // Box1's BodyCollision response, ECR_Block in the game's template
    bool savedGravity_ = true;
    bool typesLogged_ = false;
    sync::V3 pelvis_{};                                      // this frame's Read
    bool pelvisRead_ = false;
    ue::UObject* failLogged_ = nullptr;                      // the fighter whose bind failure was logged
    ue::UClass* primitiveClass_ = nullptr;                   // "PrimitiveComponent", for ScanParts
    StandInSlots slots_;                                     // the sheathed weapons (standin_slots.h)
};

// standin_grip in HSMMod.ini (config.h): true locks the stand-ins' grip joints, false frees them (pre7). Set once at start.
void SetStandInGripLock(bool on);
// standin_parts_off under [net] in HSMMod.ini (config.h): a stand-in's Box1 stops blocking bodies, and its DriverSkeleton and head mesh lose collision.
void SetStandInPartsOff(bool on);
void SetStandInTwoHandPose(bool on);
// standin_two_hand_lock under [net] (config.h): both hand joints lock while the two-hand pose owns them. Set once at start.
void SetStandInTwoHandLock(bool on);
void SetStandInTwoHandAudit(bool on);   // Help improve consent; off by default

}  // namespace hsm::game
