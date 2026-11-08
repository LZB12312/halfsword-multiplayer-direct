// Grabs across machines in owner sync (sync/hsm_grab.h). A fighter simulated here that grabs a stand-in does so
// natively (Willie_BP's "Grabbed R/L", "Grab Component R/L", "Grab Bone R/L"); only this screen sees the stand-in
// held. So each owned fighter's holds on stand-ins are streamed as GrabState (grip = its hand in the held bone's
// frame), and every GrabState naming a fighter simulated here pulls that fighter's bone toward the grabber's hand as
// its stand-in shows it (a soft, force-limited handle: sync::GrabPullDv). The held fighter is marked as natively
// held ("Being Grabbed", "Grabbed By Willie", "Event Is Grabbed") while pulled. Stand-ins are only ever read.
// Inert until Start (owner sync only). Game thread only.
//
// The design (grip in the bone frame, anchor = the grabber's stand-in hand, lease, break-free) follows
// Cyrex0/HalfSword-MP's HSMPInteract, Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see
// THIRD_PARTY_NOTICES.md).
#pragma once
#include "gameplay/adapter.h"
#include "sync/hsm_grab.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hsm::game {

class GrabSync {
public:
    struct Env {
        std::function<uint8_t(const ue::UObject*)> standInEntity;          // the entity a stand-in Willie shows, 0 if none
        std::function<ue::UObject*(uint8_t)> standInActor;                 // an entity's stand-in, nullptr if none
        std::function<bool(uint8_t)> dead;                                  // a remote entity's latest vitals say dead
        std::function<bool(uint8_t)> grappler;                              // that entity's class has the Grappler perk
        std::function<bool(uint8_t)> down;                                  // ... say dead, fallen, knocked out or 0 health
        std::function<void(const std::vector<uint8_t>&, bool reliable)> send;   // a GrabState made here (the session routes it)
        std::function<void(uint8_t held, uint8_t grabber, uint8_t hand)> sendBreak;   // my fighter broke free: the grabber's owner lets go (item 7)
        bool drag = true;                                                   // grab_drag: sync::DragYieldBones and GrabSpread
    };
    void OnBreak(ue::UObject* grabber, uint8_t held, uint8_t hand);       // my fighter's hold on a stand-in it broke free of: let go
    void Start(Adapter& adapter, Env env);
    void Stop();                                                            // unmarks my held fighters
    bool Active() const { return active_; }
    // Once per frame, after the stand-ins' Tick. owned: the fighters simulated here (entity, Willie).
    void Tick(double nowMs, float dt, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned);
    void OnGrabState(const GrabStateMsg& m, double nowMs);                  // another machine's grabber
    void Forget(uint8_t entity);                                            // the entity left or respawned: holds by it, and on it, end
    void Clear();                                                           // level change: actors are gone, nothing touched
    std::string HealthLine();                                               // since the previous call
    // The pose bones of an entity's stand-in my fighters hold natively (sync::YieldBones): left to the hold, not servoed.
    uint32_t HeldBones(uint8_t entity) const { return entity <= kMaxWillies ? heldBones_[entity] : 0u; }

private:
    struct Grabber { ue::UObject* willie = nullptr; bool live = false; sync::GrabSender sender; };
    struct Mark { ue::UObject* willie = nullptr; int32_t index = -1; ue::UObject* by = nullptr; bool native = false; };   // a fighter of mine marked held; native: a local grab owns its flags
    struct Counters { uint64_t changes = 0, refreshes = 0, heard = 0, stale = 0, pulls = 0, grabs = 0, released = 0, broke = 0, farSkips = 0,
                      noAnchor = 0, noBone = 0, letGo = 0, noCollide = 0; };
    // Item 6: a hold on my fighter, its body's collision with the grabber's hand and forearm off (Willie_BP W:17454).
    struct NoCollide { ue::UObject* mine = nullptr; int32_t mineIndex = -1; ue::UObject* hand = nullptr; int32_t handIndex = -1; uint8_t side = 0; bool seen = false; };
    void SendFor(uint8_t entity, const std::array<GrabHand, kGrabHands>& reads, double nowMs);
    std::array<GrabHand, kGrabHands> ReadHands(ue::UObject* willie);
    bool Pull(const sync::GrabPull& p, ue::UObject* me, bool down, double nowMs, float dt);   // true while the hold pulls
    bool DownHere(uint8_t entity, ue::UObject* me);                         // my fighter is dead, fallen, knocked out or at 0 health
    void SetMark(uint8_t entity, ue::UObject* willie, ue::UObject* by, bool newGrab, const std::string& bone, uint8_t hand);
    void SetLimbFlags(ue::UObject* willie, const std::string& bone, uint8_t hand);
    void KeepNoCollide(const sync::GrabPull& p, ue::UObject* me);
    bool Collide(NoCollide& c, bool off);
    void RestoreUnseen();
    void Unmark(uint8_t entity);
    bool Ready(ue::UObject* mesh);                                          // the per-frame calls, resolved on first use
    bool BoneWorld(ue::UObject* mesh, ue::FName bone, ue::FTransform& out);
    ue::FName NameOf(const std::string& bone);

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    Env env_;
    ue::PreparedCall socket_, boneIndex_, velAt_, vel_, addVel_;
    std::array<ue::FName, kGrabHands> hands_{};
    bool ready_ = false, readyFailed_ = false;
    std::map<std::string, ue::FName> names_;
    std::array<Grabber, kMaxWillies + 1> grabbers_{};
    sync::GrabInbox inbox_;
    std::array<std::array<uint32_t, kGrabHands>, kMaxWillies + 1> lastGrab_{};   // per remote grabber and hand: last grab pulled
    std::map<uint8_t, Mark> marks_;
    std::map<uint16_t, NoCollide> noCollide_;                               // by grabber << 8 | hand
    ue::UObject* collisionLib_ = nullptr;
    std::array<uint32_t, kMaxWillies + 1> heldBones_{};
    Counters n_, reported_;
};

}  // namespace hsm::game
