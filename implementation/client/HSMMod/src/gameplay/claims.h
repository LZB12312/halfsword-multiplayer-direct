// Phase 3 combat through the game's own damage code (owner authority). Every strike lands natively on the machine
// that sees it happen, against the stand-in there; the game's "Deal Complex Damage" on the stand-in is caught by a
// script-VM pre-hook (ue/script_hook.h) and becomes a HitClaim in the struck bone's frame (combat/hsm_claim.h). The
// stand-in still shows the strike (cuts, paint: it is Invulnerable, so Get Damage does nothing). The victim's owner
// replays the claim through its own "Deal Complex Damage" on its own fighter, so armour, wounds, bleeding,
// dismemberment, knock-outs and death happen there exactly as in a solo fight. Everything else that would let a
// stand-in take or deal damage natively is blocked before its body runs, outputs zeroed. Game thread only.
//
// The claim fields, the bone-frame rule, the replay's reset of "Last Complex Damage Impulse" (the claim already passed
// that gate on the attacker's screen) and the per-bone limit follow
// Cyrex0/HalfSword-MP's HSMPCombat design, Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see
// THIRD_PARTY_NOTICES.md); the hooks themselves are ours (pre-hooks that block, where HSMP undid after the fact).
// "Last Damage Taken"/"Last Damaged Bone" are left to the victim's own 0.2 s "Reset Last Damage Taken", as in a solo
// fight. Ours as well: one blow's contacts coalesce into one claim on the attacker (combat::BlowCoalescer), and the
// victim replays arriving claims from a per-victim queue, at most kReplaysPerTick per victim per frame.
// Props (test 5: rocks did no damage): a prop's contact with a fighter is decided where the prop is streamed
// (combat::DecidePropContact). A stand-in struck by a prop streamed here is a claim struck by that prop (its attacker
// the fighter simulated here that held or touched it last, else this machine's player), replayed with the victim's own
// copy of the prop as the striker; my fighter struck by a copy driven from elsewhere is blocked, the streamer's claim
// brings that blow.
#pragma once
#include "session/hsm_incoming_guard.h"
#include "sync/hsm_remote.h"
#include "combat/hsm_armor_gate.h"
#include "combat/hsm_claim.h"
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "gameplay/object_sync.h"
#include "session/hsm_authority.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace hsm::ue { class ScriptCall; }

namespace hsm::game {

class Claims {
public:
    struct Env {
        std::function<uint8_t(const ue::UObject*)> ownedEntity;     // the entity of a fighter simulated here, 0 if none
        std::function<ue::UObject*(uint8_t)> ownedActor;            // the live Willie of an entity simulated here
        std::function<void(const combat::HitClaim&)> send;          // a claim made here (the session routes it)
        std::function<bool(uint8_t attacker, uint8_t victim)> allowed;   // the friendly-fire rule
        // Props: what a prop actor is here (ObjectSync::PropOf), a prop's actor here (ObjectSync::ActorOf), and this machine's
        // player entity (a prop claim's attacker when no fighter simulated here touched the prop). Without the first two
        // a prop's contact with a stand-in is blocked and one with my fighter runs natively, as before rev 13.
        std::function<bool(ue::UObject* actor, ObjectSync::Touch& out)> prop;
        std::function<ue::UObject*(uint64_t propId)> propActor;
        std::function<uint8_t()> myEntity;
        // Sync audit T3: a write the game made here onto a stand-in (a push, a teammate helped up), for its owner.
        std::function<void(const sync::RemoteWriteMsg&)> sendRemote;
        // stuck_armor_gate (default on, 0.8.0-pre26): a stuck blade is not replayed on my fighter where my own replay of the
        // blow found that my armour stopped the whole cut (combat/hsm_armor_gate.h).
        bool armorGate = true;
    };
    // Installs the hooks (owner "claims"; they resolve on every level load). False when the script VM hook is
    // unavailable: nothing is guarded then (the host does not offer owner sync without it).
    bool Start(Adapter& adapter, StandIns& standIns, OwnerStreams& streams, Env env);
    void Stop();
    bool Active() const { return active_; }
    void Tick(double nowMs);                                         // once per frame: per-bone limit, queued replays

    void Replay(const combat::HitClaim& c, double nowMs);            // a claim against a fighter simulated here: queued
    void Effect(const combat::HitClaim& c);                          // someone else's claim: shown on the stand-in
    void NoteClash(uint8_t defender, ue::UObject* theirWeapon);   // my weapon met that weapon of defender's stand-in: its parry state
    uint8_t RecentAttacker(uint8_t victim, double nowMs) const;      // last replayed attacker within 10 s, else 0
    void Forget(uint8_t entity) {                                    // left, respawned or rejoined: its ids may restart
        dedupe_.Forget(entity);
        coalescer_.Forget(entity);
        stuckLimiter_.Forget(entity);
        armorGate_.Forget(entity);
        queue_.Forget(entity);                                       // blows queued against its old body are dropped
        if (entity <= kMaxWillies) { recent_[entity] = {}; resetDcdMs_[entity] = 0.0; }   // K4: a new body has no recent attacker
    }
    std::string HealthLine();                                        // since the previous call
    // C0 (0.8.0-pre7): the hit audit, kept only while on (help_improve). AuditLine: since the previous call; empty when
    // nothing was struck.
    void SetAudit(bool on) { if (on != audit_) { audit_ = on; strikes_.clear(); landed_.clear(); } }
    std::string AuditLine();

private:
    struct Box { ue::UObject* pawn = nullptr; int32_t pawnIndex = -1; ue::UObject* box = nullptr; int32_t boxIndex = -1; };
    struct Recent { uint8_t attacker = 0; double atMs = -1e18; };
    struct Counters {
        uint64_t sent = 0, gated = 0, limited = 0, friendly = 0, coalesced = 0, zeroImpulse = 0, capped = 0, replayed = 0,
                 refused = 0, queued = 0, dropped = 0, dupes = 0, stale = 0, ageUnknown = 0, parried = 0, noVictim = 0, noBone = 0,
                 noStriker = 0, failed = 0, blockedStandIn = 0, blockedOnMine = 0, blockedFlag = 0, getDamageBlocked = 0, effects = 0,
                 effectsSkipped = 0, clashes = 0, clashUnmatched = 0, propSent = 0, propReplayed = 0, blockedPropCopy = 0, badBone = 0,
                 stuckSent = 0, stuckLimited = 0, stuckReplayed = 0, stuckHeld = 0, armorStopped = 0, boltSent = 0, boltNoShooter = 0;
    };
    bool OnDealComplexDamage(ue::ScriptCall& c);
    bool OnGetDamage(ue::ScriptCall& c);
    void OnStuckDamage(ue::ScriptCall& c, ue::UObject* standIn, uint8_t victim);   // a lodged blade of mine, on a stand-in
    void AfterDealComplexDamage(ue::ScriptCall& c);
    void GatherPush(ue::ScriptCall& c, ue::UObject* striker, uint8_t attacker, uint8_t victim);   // item 3: its impulse, for the owner
    void OnGetUpPal(ue::ScriptCall& c);                              // item 22: my fighter helps a stand-in up
    void SendPending(ue::ScriptCall& c);                             // the claim made before the body ran, coalesced by its damage
    void MakeClaim(ue::ScriptCall& c, ue::UObject* standIn, uint8_t victim, uint8_t attacker, ue::UObject* attackerWillie, ue::UObject* comp,
                   const ObjectSync::Touch* prop);
    void ReplayNow(const combat::HitClaim& c, double nowMs);         // a queued claim, through my fighter's own DCD
    void ReplayStuck(const combat::HitClaim& c, double nowMs);       // a lodged blade's damage, through my fighter's own Get Damage
    bool BoneXform(ue::UObject* mesh, ue::FName bone, combat::Xform& out) const;
    bool PeerBoneOk(const combat::HitClaim& c);   // M7: a peer's hitBone may be interned (MakeName) only if ValidBoneName
    ue::UObject* Striker(const combat::HitClaim& c) const;                          // the claim's striking component here
    ue::UObject* StrikerOn(ue::UObject* fighter, const combat::HitClaim& c) const;   // the claim's striking component on fighter
    ue::UObject* ComponentNamed(ue::UObject* actor, const std::string& name) const;  // a primitive component of actor by name
    ue::UObject* HitBox(ue::UObject* victim, const combat::HitClaim& c, const combat::DcdInputs& in);
    bool CallGetDamage(ue::UObject* victim, ue::UObject* mesh, ue::UObject* striker, ue::FName bone, const combat::DcdInputs& in,
                       double rawDamage, ue::UObject* hitBox, double* damageOut);
    // The game's own outputs of the call: Damage Out, Cutting Rate Out (the cut left after armour) and
    // Material Density Out (set only when DCD runs to its end).
    struct DcdOut { double damage = 0, cutRate = 0, density = 0; };
    bool CallDealComplexDamage(ue::UObject* victim, ue::UObject* mesh, ue::UObject* striker, ue::FName bone, const combat::DcdInputs& in,
                               ue::UObject* hitBox, DcdOut* out);

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    StandIns* standIns_ = nullptr;
    // What every Deal Complex Damage call on this machine was, for the log: what a tester's report can't say.
    struct ContactLog { uint64_t all = 0, onStandIn = 0, onMine = 0, onOther = 0, byStandIn = 0, byMine = 0, byNobody = 0, afterZero = 0, afterSome = 0; uint32_t detailed = 0; };
    ContactLog contacts_, contactsReported_;
    std::string ContactLine();
    OwnerStreams* streams_ = nullptr;
    Env env_;
    mutable ue::PreparedCall components_;                            // Actor.K2_GetComponentsByClass (frees its last array)
    ue::UClass* primitive_ = nullptr;
    ue::UClass* boxClass_ = nullptr;
    bool boxFailLogged_ = false;
    combat::ClaimLimiter limiter_;
    combat::ClaimDedupe dedupe_;
    BlowGate blowGate_;                      // what each attacker may land here (session/hsm_incoming_guard.h)
    combat::BlowCoalescer coalescer_;
    combat::StuckDamageLimiter stuckLimiter_;
    combat::ArmorGate armorGate_;
    combat::ReplayQueue queue_;
    sync::PushGather pushes_;
    sync::RemoteGate reviveSent_;
    uint64_t frame_ = 0;
    uint64_t pushesSent_ = 0, revivesSent_ = 0;
    std::array<uint32_t, kMaxWillies + 1> nextClaimId_{};
    // The claim just made (no id yet), until its stand-in's own run of the blow returns: its Damage Out decides whether
    // the claim is sent (combat::BlowCoalescer) and ranks it.
    struct Pending { ue::UObject* standIn = nullptr; combat::HitClaim claim; combat::BlowKey key; bool prop = false; std::string audit; };
    Pending pending_;
    std::array<Recent, kMaxWillies + 1> recent_{};
    std::array<double, kMaxWillies + 1> resetDcdMs_{};                   // per stand-in victim: when its DCD gate clears (0: no reset pending)
    std::map<int, Box> boxes_;                                       // victim entity -> its private Hit Box
    Counters n_, reported_;
    // C0: blows on stand-ins by what struck them, and their fate; claims replayed on my fighter by striking component.
    struct Strike { uint32_t contacts = 0, blocked = 0, gated = 0, limited = 0, noClaim = 0, coalesced = 0, zero = 0, capped = 0, sent = 0;
                    double damage = 0; };
    struct Landed { uint32_t replayed = 0, parried = 0, missed = 0, zeroDamage = 0; double damage = 0; };
    bool audit_ = false;
    std::map<std::string, Strike> strikes_;
    std::map<std::string, Landed> landed_;
    Strike* AuditStrike(ue::UObject* comp);                          // null with the audit off
};

}  // namespace hsm::game
