// Owner authority (protocol rev 9, session/hsm_authority.h). Every machine simulates the fighters it owns natively,
// never corrected, and streams them (OwnerStreams); every fighter owned elsewhere is a neutralised stand-in servoed to
// that stream (StandIns), the network form of the mirror bench (mirror.h). Game thread only.
//
// The stand-in flow follows Cyrex0/HalfSword-MP mods/HSMPAvatars/Scripts/main.lua (drive_root, snap_mesh, the 60 Hz
// pose stream), Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md).
#pragma once
#include "gameplay/adapter.h"
#include "gameplay/standin_rig.h"
#include "hsm_owner.h"
#include "sync/hsm_poseplay.h"
#include "sync/hsm_roottrack.h"
#include "sync/hsm_slowsend.h"
#include "sync/hsm_standin.h"
#include "sync/hsm_track.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hsm::game {

double SteadyNowMs();

// The fighters this machine owns: pose at 60 Hz (bots 30 Hz) and vitals at 15 Hz, both unreliable. One clock for
// every pose and every hit claim this machine makes, so a receiver can relate a claim to the poses it has seen.
class OwnerStreams {
public:
    using Send = std::function<void(const std::vector<uint8_t>&)>;
    void Start(double nowMs);                         // a new network session: a new clock
    void Clear();
    bool Started() const { return startMs_ > 0.0; }
    uint32_t ClockMs(double nowMs) const;             // this machine's stream clock (also every claim's attackerTsMs)
    // Once per frame: reads and sends every owned fighter that is due. owned: (entity, its live Willie).
    void Tick(Adapter& adapter, double nowMs, float dt, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned, const Send& send);
    bool PelvisOf(uint8_t entity, sync::V3& out) const;   // as of the latest read (the snap clearance)
    // The hand-off epoch each Pose and Vitals carries for its entity (sync/hsm_handoff.h); 0 when unset.
    void SetEpochOf(std::function<uint8_t(uint8_t)> epochOf) { epochOf_ = std::move(epochOf); }
    uint64_t PosesSent() const { return posesSent_; }
    uint64_t VitalsSent() const { return vitalsSent_; }

private:
    struct Owned {
        FighterRig rig;
        ue::UObject* willie = nullptr;
        double nextPoseMs = 0, nextVitalsMs = 0;
        sync::V3 pelvis{};
        bool havePelvis = false, failLogged = false;
    };
    std::array<std::unique_ptr<Owned>, kMaxWillies + 1> owned_{};
    double startMs_ = 0, prevDtS_ = 0;
    sync::SlowFrameMeter slowFrames_;   // item 7a: this machine's frames past 25 ms (a pose still goes each one)
    std::function<uint8_t(uint8_t)> epochOf_;
    uint64_t posesSent_ = 0, vitalsSent_ = 0, readFailed_ = 0;
};

// Fighters owned elsewhere. The feed (jitter buffer, root track, vitals) is kept per entity and survives the actor being
// replaced (respawn); the actor binding (rig, servo) is per stand-in actor.
class StandIns {
public:
    void OnPose(const PoseMsg& m, const sync::PoseFrame& frame, double nowMs);
    void OnVitals(const VitalsMsg& v);
    // The session spawned willie as entity's stand-in. replica: an exact replica of the host's stream, whatever
    // SetReplica says (hand-off: this client's own fighter while the host simulates it).
    void Attach(uint8_t entity, ue::UObject* willie, bool replica = false);
    void Detach(uint8_t entity);                        // unbinds; the session destroys the actor
    // Hand-off: the stand-in becomes a live fighter again (FighterRig::Revive) and is unbound; the actor stays. False
    // keeps the stand-in attached for retry while a saved grip reference cannot be restored. No bound rig is success.
    bool Release(uint8_t entity);
    // Hand-off: entity's stream comes from another machine now (its clock and its jitter differ): the feed starts again
    // from that source's next pose. The actor stays as it is until then.
    void ResetFeed(uint8_t entity);
    void Forget(uint8_t entity);                        // the entity left: its feed goes too
    void Clear();

    bool IsStandIn(const ue::UObject* o) const;         // the Willie of a bound stand-in
    uint8_t EntityOf(const ue::UObject* willie) const;  // 0 if none
    ue::UObject* ActorOf(uint8_t entity) const;
    const VitalsMsg* Vitals(uint8_t entity) const;      // the owner's latest vitals, nullptr before any
    // The weapon class (sync::WeaponSample::classHash) the owner's newest pose shows in hand (0 right, 1 left), 0 when
    // that hand is empty. False before any pose, or when the hand holds a weapon of unknown class (prop_sync.cpp).
    bool PoseHand(uint8_t entity, int hand, uint32_t& classHash) const;
    // The same hand in more detail (prop_sync's hand matching, sync/hsm_handmatch.h): its class as the pose carries it
    // (0 empty, 1 unknown class), the class it showed before that (0 when none), the local time that class first showed,
    // and the weapon's root in the newest pose and when that class first showed (both unset for an empty hand). False
    // before any pose.
    struct PoseHandView {
        uint32_t classHash = 0, prevClassHash = 0;
        double sinceMs = 0;
        sync::V3 pos{}, firstPos{};
    };
    bool PoseHandInfo(uint8_t entity, int hand, PoseHandView& out) const;
    // The owner's clock now (local time minus the minimum transit offset seen): how old a claim from that owner is.
    bool OwnerClockNow(uint8_t entity, double nowMs, int64_t& out) const;
    // The owner time the stand-in currently shows (its last sample), for the parry window.
    bool ShownOwnerTs(uint8_t entity, uint32_t& out) const;

    // Pose bones (bit per sync::kPoseBones index) an entity's stand-in has no bodies for any more (gore): never driven.
    void SetDeadBones(std::function<uint32_t(uint8_t)> deadBones) { deadBones_ = std::move(deadBones); }
    // Weapon slots (bit 0 "Weapon R", bit 1 "Weapon L") of an entity's stand-in that a stuck-blade joint holds to a local
    // fighter: the servo leaves them to the joint (FighterRig::SetPinnedWeapons).
    void SetHeldWeapons(std::function<uint32_t(uint8_t)> heldWeapons) { heldWeapons_ = std::move(heldWeapons); }
    // Pose bones of an entity's stand-in a fighter simulated here holds natively (GrabSync::HeldBones): not servoed while
    // held, so the servo and the grab constraint do not pull them two ways.
    void SetHeldBones(std::function<uint32_t(uint8_t)> heldBones) { heldBones_ = std::move(heldBones); }
    void SetStuckBones(std::function<uint32_t(uint8_t)> stuckBones) { stuckBones_ = std::move(stuckBones); }   // bones my blades are stuck in
    void SetGrabNoSnap(bool on) { grabNoSnap_ = on; }   // grab_drag: a stand-in my fighter holds is not snapped (sync::GrabBlocksSnap)
    // Called after every driven step of a stand-in with what the servo read, the target and the rig (the mirror bench's
    // tracking measures, mirror.h). Optional.
    using StepObserver = std::function<void(uint8_t entity, const sync::DriverStep& step, const sync::FighterNow& now,
                                            const sync::PoseFrame& target, const FighterRig& rig)>;
    void SetStepObserver(StepObserver observer) { stepObserver_ = std::move(observer); }
    // Once per frame, before the physics step. avoid: local pelvises a stand-in must never be snapped onto.
    void Tick(Adapter& adapter, double nowMs, float dt, const std::vector<sync::V3>& avoid);
    // Host physics, client (0.9.0): stand-ins attached from now on are exact replicas of the host's fighters
    // (sync::ReplicaServoParams: full gain, no caps, no contact yield). Off: the owner-sync servo.
    void SetReplica(bool on) { replica_ = on; }
    void SetAudit(bool on);   // opt-in field audit and two-handed motion measurements (help-improve)
    bool Replica() const { return replica_; }
    std::string HealthLine();                            // since the previous call; empty with no stand-in
    int Count() const;

private:
    struct Feed {
        sync::PosePlayer player;
        sync::RootTrack root;
        VitalsMsg vitals{};
        bool haveVitals = false, vitalsFresh = false;   // fresh: not yet applied to the stand-in
        std::array<uint32_t, sync::kPoseWeapons> handClass{};   // newest pose's weapon class per hand (0 = empty, 1 = unknown)
        std::array<uint32_t, sync::kPoseWeapons> handPrevClass{};   // the class each hand showed before that
        std::array<double, sync::kPoseWeapons> handSinceMs{};   // local time each hand's class last changed
        std::array<sync::V3, sync::kPoseWeapons> handPos{}, handFirstPos{};   // its weapon's root: newest, and as the class first showed
        uint32_t handTs = 0;
        bool haveHands = false;
        uint64_t poses = 0, rejected = 0;
        sync::SlowSenderMeter slow;   // item 7a: why this owner's poses come slowly (set on the first pose)
        bool slowSet = false;
    };
    struct Actor {
        ~Actor() { rig.Unbind(); }
        ue::UObject* willie = nullptr;
        int32_t index = -1;
        FighterRig rig;
        sync::StandInDriver driver;
        double bindDeadlineMs = 0, nextBindMs = 0;
        bool bindLogged = false, haveTarget = false, releaseBlockedLogged = false;
        bool deadShown = false;   // DED set on the stand-in from the owner's vitals
        bool downShown = false;   // the owner is dead, fallen, knocked out or at 0 health: the capsule follows the body, the beacon is off
        sync::V3 lastTargetPelvis{};
        double lastPelvisErrUu = -1.0;   // the last step's, -1 unknown
        uint32_t shownTs = 0;
        bool haveShown = false;
        VitalsMsg applied{};   // T2: the owner's vitals as last applied, and what local code changed since
        bool haveApplied = false;
        std::array<uint32_t, 9> drift{};
        double nextDriftLogMs = 0;
        const char* twoHandReason = nullptr;
        double nextTwoHandLogMs = 0;
    };
    struct Window {
        sync::ErrorStats pelvis, hands;
        uint64_t frames = 0, targeted = 0, extrapolated = 0, stale = 0, cuts = 0, snaps = 0, placed = 0, struck = 0, grabNoSnap = 0;
        double delaySum = 0, j95Max = 0;
        // Complete same-frame samples only; mean/max suffice, so this adds no per-metric histogram or allocation.
        struct TwoHand {
            std::array<double, 5> sum{}, max{};   // R hand, L hand, weapon root, weapon tip, weapon angle
            uint64_t samples = 0, active = 0, suspended = 0, updates = 0, failures = 0;
        } twoHand;
    };
    struct Slot {
        std::unique_ptr<Feed> feed;
        std::unique_ptr<Actor> actor;
        Window window;
    };
    void TickOne(Adapter& adapter, uint8_t entity, Slot& s, double nowMs, float dt, const std::vector<sync::V3>& avoid);
    void NoteDrift(Adapter& adapter, uint8_t entity, Actor& a, double nowMs);
    void Record(Slot& s, const sync::DriverStep& step, const sync::FighterNow& now, const sync::PoseFrame& target);
    std::array<Slot, kMaxWillies + 1> slots_{};
    std::function<uint32_t(uint8_t)> deadBones_;
    std::function<uint32_t(uint8_t)> heldWeapons_;
    std::function<uint32_t(uint8_t)> heldBones_;
    std::function<uint32_t(uint8_t)> stuckBones_;
    bool grabNoSnap_ = false;
    StepObserver stepObserver_;
    double costSumMs_ = 0, costMaxMs_ = 0;
    uint64_t ticks_ = 0;
    bool audit_ = false;
    bool replica_ = false;
};

}  // namespace hsm::game
