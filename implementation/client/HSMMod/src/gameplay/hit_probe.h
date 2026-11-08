// Logs what my fighter and gear touch on a stand-in, and what on a stand-in can be touched at all (HELP IMPROVE THE MOD
// only). Reads only: a contact goes on as if this were not here. Game thread only.
//
// OnHit runs on the raw contact (session.cpp's HitHook), before Deal Complex Damage, so a blocker the damage code never
// hears of is named too: which component and bone, how far from the body as it is drawn, and where on the body. The
// distance is to K2_GetClosestPointOnPhysicsAsset, which follows the bones: a contact away from it hit something the
// player cannot see. combat/hsm_contact_probe.h sorts and counts.
// Census runs from the health report, one stand-in at a time: every component of the stand-in and of what is attached to
// it that has collision on, with its responses to the channels my body and weapons use. A line only when a
// component's state changes, so the log shows what collides and when that changes.
#pragma once
#include "combat/hsm_contact_probe.h"
#include "ue/engine.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace hsm::game {

class HitProbe {
public:
    void SetOn(bool on) { if (on_ && !on) tally_ = combat::ContactTally(); on_ = on; }   // consent gone: nothing kept
    bool On() const { return on_; }
    void Clear();   // a level change: the old level's functions and components

    // fn and params are the hit event's; selfIsStandIn says which side of it the stand-in is on.
    void OnHit(uint8_t entity, ue::UObject* willie, ue::UObject* mesh, bool selfIsStandIn, bool down, ue::UFunction* fn, void* params,
               double nowMs);
    std::string DrainLine() { return tally_.DrainLine(); }   // the health report's line; empty when nothing was seen
    void Census(uint8_t entity, ue::UObject* willie, ue::UObject* mesh);

private:
    struct Offsets { int32_t hitComponent = -1, otherComp = -1, impulse = -1, hit = -1; };
    const Offsets& OffsetsOf(ue::UFunction* fn);
    bool ResolveHitResult();

    bool on_ = false;
    combat::ProbeBudget budget_;
    combat::ContactTally tally_;
    std::unordered_map<ue::UFunction*, Offsets> offsets_;
    int32_t impactPoint_ = -1, boneName_ = -1, myBoneName_ = -1;   // members of FHitResult
    bool hitResultRead_ = false;
    ue::UClass* primitive_ = nullptr;
    struct Seen { std::map<std::string, std::string> state; int lines = 0; bool full = false; };
    std::map<uint8_t, Seen> census_;
};

}  // namespace hsm::game
