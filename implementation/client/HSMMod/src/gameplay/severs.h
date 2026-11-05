// Phase 4 severs and stuck blades in owner sync (combat/hsm_sever.h). Game thread only.
//
// A sever starts natively where the blade is: Constraint_Weapon_Stuck_BP::Call_Dismember runs the struck Willie's
// "Dismember Function Initiate" and, 0.1 s later, "Dismember Function Delayed". When that Willie is a stand-in hit by a
// fighter simulated here, a script-VM pre-hook captures the call as a SeverClaim and blocks it (the stand-in must not
// lose a limb its owner keeps). The victim's owner rebuilds the call on its own fighter, with fresh vertex-paint boxes
// placed on its own bone, and runs the game's own two functions, so the limb comes off there exactly as in a solo fight.
// Everyone else keeps the claim until the owner's vitals report the part severed, then runs the same two functions on
// their stand-in (dismemberment briefly allowed for that one call). Without a claim the gore fallback shows it.
// A sever a fighter simulated here takes natively (its own blade, another fighter simulated here such as a bot on the
// host, a trap) runs as the game runs it; a post-hook on its accepted Initiate sends the same arguments as a SeverNotice
// (attacker == victim, combat/hsm_sever.h), which everyone else keeps like a claim. The notice does not name the striker:
// unless the fighter cut itself, a stand-in's Delayed gets the fresh VP box as its weapon, so the limb does not ignore
// the striker's blade there.
//
// A blade stuck in a stand-in (the weapon's "Stuck Constraints Array") is reported on/off; the victim's owner holds the
// attacker's stand-in weapon to its own bone with a PhysicsConstraintComponent while it is stuck, as the game's own
// Constraint_Weapon_Stuck_BP joins blade and body: angular motion locked, linear motion limited to 2 uu (the game's limited
// axis on a cut; which axes it locks depends on the thrust flag, which the report does not carry), and the blade's
// collision with that Willie turned off through the game's CollisionDisablerFunctionLibrary: against its SK_Skeleton
// "Rigid Bones", and against its Mesh bones near the struck one (Disable Nearest Collisions, read from the class default:
// no spawn site sets it). The game also ignores bones a sphere trace finds around the impact point; the report carries no
// impact point, so those are not. No break thresholds are set (the game's scale with the impact; untested here): the hold
// ends when the attacker frees the blade, after the 10 s expiry, when the physics joint breaks, or when anything in it
// dies. While held, the stand-in rig stops driving that weapon (HeldWeapons), so its servo does not drag my fighter.
//
// Severs follow Cyrex0/HalfSword-MP's vitals-driven sever design (the owner decides, stand-ins follow its vitals),
// Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md); the capture and replay
// of the game's own dismember calls are ours.
#pragma once
#include "combat/hsm_claim.h"
#include "combat/hsm_sever.h"
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hsm::ue { class ScriptCall; }

namespace hsm::game {

class Severs {
public:
    struct Env {
        std::function<uint8_t(const ue::UObject*)> ownedEntity;          // the entity of a fighter simulated here, 0 if none
        std::function<ue::UObject*(uint8_t)> ownedActor;                 // the live Willie of an entity simulated here
        std::function<void(const combat::SeverClaim&)> sendSever;        // a sever captured here (the session routes it)
        std::function<void(const combat::SeverClaim&)> sendNotice;       // an own cut of a fighter simulated here
        std::function<void(const combat::StuckBladeMsg&)> sendStuck;     // a blade of ours stuck in / freed from a stand-in
        std::function<bool(uint8_t attacker, uint8_t victim)> allowed;   // the friendly-fire rule
        std::function<void(uint8_t entity, uint8_t part)> nativeShown;   // a stand-in lost that part natively (gore)
        std::function<void(uint8_t entity, uint8_t part)> nativeLost;    // ... but the cut's Delayed half never ran there
    };
    using Owned = std::vector<std::pair<uint8_t, ue::UObject*>>;

    // Installs the hooks (owner "severs"; they resolve on every level load). False when the script VM hook is unavailable.
    bool Start(Adapter& adapter, StandIns& standIns, OwnerStreams& streams, Env env);
    void Stop();
    bool Active() const { return active_; }
    void Tick(double nowMs, const Owned& owned);   // due Delayed calls, pending expiry, our stuck blades, stuck expiry

    void Replay(const combat::SeverClaim& c, double nowMs);     // a sever of a fighter simulated here
    void Remember(const combat::SeverClaim& c, double nowMs);   // a sever of a fighter simulated elsewhere
    void OnVitals(const VitalsMsg& v, double nowMs);            // a stand-in's owner vitals (newly severed parts)
    void OnStuck(const combat::StuckBladeMsg& m, double nowMs); // a blade stuck in / freed from a fighter simulated here
    void Forget(uint8_t entity);                                // left, respawned or despawned
    void LetGo(uint8_t victim);                                 // my fighter is simulated elsewhere now (a hand-off): blades held to it let go
    uint32_t HeldWeapons(uint8_t attacker) const;   // weapon slots of that stand-in held to my fighter (bit 0 R, bit 1 L)
    uint32_t StuckBones(uint8_t victim) const;      // pose bones of that stand-in a blade of a fighter here is stuck in (and their parents)
    std::string HealthLine();                                   // since the previous call

private:
    struct Ref {
        ue::UObject* obj = nullptr;
        int32_t index = -1;
    };
    // The rebuilt arguments of one Initiate/Delayed pair on one Willie.
    struct Rebuilt {
        Ref willie, mesh, attach, box1, box2, weapon;
        std::vector<Ref> markers;
        uint8_t part = 0;
    };
    struct DueDelayed {
        double dueMs = 0;
        Rebuilt args;
        uint8_t entity = 0;
        bool standIn = false;   // dismemberment was allowed for this call: forbid it again afterwards
        uint32_t claimId = 0;
        combat::SeverClaim claim; // accepted geometry, retained for owner confirmation after Delayed
    };
    struct Retry {              // refused while another sever on that fighter was in process: tried once more after it
        combat::SeverClaim claim;
        double untilMs = 0;
        bool standIn = false;
        bool late = false;
    };
    struct BlockedOnMine {      // a stand-in's Initiate blocked on a fighter simulated here: its Delayed is blocked too
        ue::UObject* willie = nullptr;
        ue::UObject* weapon = nullptr;   // compared, never dereferenced
        double atMs = 0;
    };
    struct Hand {               // one hand of a fighter simulated here: its blade stuck in a stand-in
        bool stuck = false;
        bool bad = false;       // its sticking could not be reported: not retried until it changes
        uint16_t seq = 0;
        uint8_t victim = 0;
        Ref actor;              // the Constraint_Weapon_Stuck_BP_C reported
        std::string bone;       // the stand-in bone it is stuck in ("Bone Name 2"), while stuck
    };
    struct Joint {              // a stand-in blade held to a bone of a fighter simulated here
        Ref constraint, owner, blade, skeleton, mesh;
        std::vector<ue::FName> rigid, near;   // the bone lists its collision is turned off for (on again when let go)
        bool rigidOff = false, nearOff = false;   // that list's collision is off
    };
    struct Counters {
        uint64_t captured = 0, limited = 0, friendly = 0, invalid = 0, blockedStandIn = 0, blockedOnMine = 0,
                 replayed = 0, refused = 0, retried = 0, retryLost = 0, dupes = 0, stale = 0, noVictim = 0, noMarker = 0,
                 noBone = 0, noBox = 0, delayed = 0, delayedLost = 0, remembered = 0, shown = 0, shownLate = 0,
                 showRefused = 0, noClaim = 0, stuckSent = 0, stuckFreed = 0, stuckBad = 0, held = 0, released = 0,
                 holdFailed = 0, alreadyStuck = 0, callFailed = 0, broken = 0, collisionKept = 0, noticed = 0,
                 noticeInvalid = 0, badBone = 0;
    };
    enum class Run { Accepted, Refused, InProcess, Failed };
    enum class Read { Ok, NoBoxes, NoBone };
    struct OwnCut {             // a native Initiate on a fighter simulated here, read before its body runs
        ue::UObject* willie = nullptr;   // the call's self: alive until its post-hook
        bool before = false;             // "Dismemberment In Process" before the body (the game refuses it then)
        Read read = Read::NoBoxes;
        combat::SeverCapture in;
    };

    bool Classify(ue::ScriptCall& c, bool initiate);
    void Capture(ue::ScriptCall& c, ue::UObject* standIn, uint8_t victim, uint8_t attacker, ue::UObject* attackerWillie,
                 ue::UObject* weaponComp, uint8_t part, double nowMs);
    Read ReadSever(ue::ScriptCall& c, ue::UObject* willie, combat::SeverCapture& in);   // the call's arguments on willie
    void SourceOf(ue::UObject* fighter, ue::UObject* weaponComp, combat::SeverCapture& in) const;
    void NoteOwnCut(ue::ScriptCall& c, ue::UObject* self, uint8_t entity, ue::UObject* striker, ue::UObject* weaponComp);
    void OwnCutDone(ue::ScriptCall& c);   // the Initiate's post-hook: a notice when the game accepted it
    Run Begin(ue::UObject* willie, const combat::SeverClaim& c, double nowMs, bool standIn);   // Rebuild + Initiate
    bool Rebuild(ue::UObject* willie, const combat::SeverClaim& c, Rebuilt& out);
    bool Invoke(const char* function, const Rebuilt& a, bool standIn);
    void CallDelayed(DueDelayed& d);
    void DestroyBoxes(const Rebuilt& a);
    void Show(const combat::SeverClaim& c, double nowMs, bool late, bool retry = false);
    void Mine(const combat::SeverClaim& c, double nowMs, bool retry);
    void RunRetries(double nowMs);
    void ScanOurBlades(uint8_t entity, ue::UObject* willie);
    void SendStuck(uint8_t entity, combat::StrikeSource source, Hand& h, ue::UObject* constraint, uint8_t victim);
    void SendFreed(uint8_t entity, combat::StrikeSource source, Hand& h);
    void Hold(const combat::StuckBladeMsg& m);
    void Let(uint8_t attacker, combat::StrikeSource source);
    void Collide(Joint& j, bool off);   // the blade's collision with the fighter's skeleton and near bones
    std::vector<ue::FName> NearBones(ue::UObject* mesh, ue::FName bone) const;
    ue::UObject* FighterActor(uint8_t entity) const;   // a stand-in here, or a fighter simulated here
    ue::UObject* WeaponOf(ue::UObject* fighter, combat::StrikeSource source) const;
    ue::UObject* WeaponPart(uint8_t entity, combat::StrikeSource source, const std::string& component) const;
    ue::UObject* NewBox(ue::UObject* willie, ue::UObject* mesh, const std::string& bone, const combat::Xform& world,
                        const combat::V3& extent);
    bool BoneXform(ue::UObject* mesh, const std::string& bone, combat::Xform& out) const;   // false: no such bone
    bool PeerBoneOk(const std::string& bone, uint8_t from);   // M7: a bone name off the wire is interned only if ValidBoneName
    bool ComponentXform(ue::UObject* comp, combat::Xform& out) const;
    std::vector<ue::UObject*> Components(ue::UObject* actor) const;   // its PrimitiveComponents
    Ref RefOf(ue::UObject* o) const { return Ref{o, o ? ue::ObjectIndex(o) : -1}; }
    bool Live(const Ref& r) const { return adapter_ && adapter_->StillValid(r.obj, r.index); }
    // A pointer read from a field or a call this frame, checked before it is called on: a field may still name an object
    // the game destroyed (garbage until collection), and a stale one is a jump through a bad vtable (T1).
    bool Alive(ue::UObject* o) const { return o && Live(RefOf(o)); }

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    StandIns* standIns_ = nullptr;
    OwnerStreams* streams_ = nullptr;
    Env env_;
    mutable ue::PreparedCall components_;   // Actor.K2_GetComponentsByClass
    ue::UClass* primitive_ = nullptr;
    ue::UClass* boxClass_ = nullptr;
    ue::UClass* constraintClass_ = nullptr;
    ue::UObject* collisionLib_ = nullptr;   // CollisionDisablerFunctionLibrary's default object (a native class: never unloaded)
    int nearDefault_ = -1;                  // Constraint_Weapon_Stuck_BP_C's "Disable Nearest Collisions" (-1: not read yet)
    combat::SeverLimiter limiter_;
    combat::ClaimDedupe dedupe_;
    combat::SeverPending pending_;
    combat::StuckBlades blades_;
    std::vector<DueDelayed> due_;
    std::vector<Retry> retry_;
    std::vector<BlockedOnMine> blockedOnMine_;
    std::vector<OwnCut> ownCuts_;   // a stack: pushed by the pre-hook, popped by the post-hook of the same call
    std::array<uint32_t, kMaxWillies + 1> nextClaimId_{};
    std::array<uint32_t, kMaxWillies + 1> severed_{};        // the last severed mask each owner reported
    std::array<uint32_t, kMaxWillies + 1> shownMask_{};      // parts shown natively on that stand-in this life
    std::array<std::array<double, combat::kSeverParts>, kMaxWillies + 1> bitAtMs_{};   // when each part's bit appeared
    std::map<std::pair<uint8_t, uint8_t>, Hand> hands_;     // (entity simulated here, source) -> its stuck blade
    uint16_t nextStuckSeq_ = 0;
    std::map<std::pair<uint8_t, uint8_t>, Joint> joints_;   // (attacker, source) -> the hold on my fighter
    Counters n_, reported_;
};

}  // namespace hsm::game
