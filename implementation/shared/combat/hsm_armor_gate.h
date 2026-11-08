// The victim's own armour verdict on a blow, used to refuse stuck-blade damage its armour would not have let in.
//
// Stock (decomp): Deal Complex Damage (DCD) returns Cutting Rate Out, the cut left after armour (Willie_BP.cpp
// 3217-3236; 0 when the armour takes the whole cut). The weapon lodges only if that rate times the impulse clears
// 10000..25000 (ModularWeaponBP.cpp Collision_Hit), so a fully stopped edge never lodges. A lodged blade's damage is
// Get Damage with Inside set (Constraint_Weapon_Stuck_BP.cpp:767), which has no armour term.
//
// Owner sync: the lodge is decided on the attacker's machine against a stand-in that lags the victim, so a blade can
// lodge there where the victim's armour stopped the edge. The victim then replayed every Stuck claim as Inside damage.
// Here the victim's replay of the blow (its own DCD, its own armour) leaves a verdict per (attacker, victim, source,
// bone), and a newer blow on that bone replaces it.
//
// Each lodged blade is decided once. Its Stuck claims are a stream: the first is refused when a recent verdict near it
// says the armour held, and the rest of the stream keep that answer. A blade already being replayed stays replayed when
// it scrapes plate on the same bone (most of a stab's damage is its Stuck claims). A refused blade stays refused however
// long it sits there and however its entry point moves, until a newer blow on that bone gets through. A stream with no
// recent verdict (a bolt, a blow coalesced away) is replayed as before.
#pragma once
#include "combat/hsm_claim.h"
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>

namespace hsm::combat {

// A stuck claim within this distance (uu, in the bone's frame) of the stopped blow's point is the same blade. The
// constraint's entry point (In Location) is re-traced each tick along the blade, so it stays near the impact point;
// the DCD trace radius is 5-15 uu.
constexpr double kArmorHeldMatchUu = 25.0;
// A blade lodges in the blow that struck, so its first Stuck claim closely follows that blow's verdict. An older verdict
// is another blow's, and starts no refusal. As long as a claim may be (kClaimMaxAgeMs).
constexpr double kArmorHeldMaxAgeMs = 1000.0;
// Stuck claims this close together are one blade: kStuckClaimEveryMs is per (attacker, victim), so two blades in one
// victim share it, and a hitch can hold a few back.
constexpr double kStuckStreamGapMs = 500.0;

class ArmorGate {
public:
    // A replayed blow (not Stuck). cutOut: DCD's Cutting Rate Out; densOut: its Material Density Out, which DCD sets
    // only at its end (0: DCD returned early, no verdict). A blow without a cutting edge (Cutting Power 0) leaves the
    // verdict as it was.
    void NoteBlow(const HitClaim& c, double cutOut, double densOut, double nowMs) {
        if (c.striker == StrikerKind::Stuck || !(densOut > 0.0) || !(c.cuttingPower > 0.0) || !std::isfinite(cutOut)) return;
        Entry& e = entries_[KeyOf(c)];
        e.held = !(cutOut > 0.0);
        e.at = c.location;
        e.atMs = nowMs;
    }
    // True: a Stuck claim the victim's armour stopped (do not replay it). Call only for a bone the fighter has: each
    // key is kept until Forget.
    bool StuckHeld(const HitClaim& stuck, double nowMs) {
        if (stuck.striker != StrikerKind::Stuck) return false;
        Entry& e = entries_[KeyOf(stuck)];
        const bool running = e.stuckMs >= 0.0 && nowMs - e.stuckMs < kStuckStreamGapMs;
        e.stuckMs = nowMs;
        if (running) {
            if (e.refusing && !e.held) e.refusing = false;
        } else {
            // A gap in the claims (a hitch, a stalled frame, lost packets) does not let a refused blade in: with no newer
            // blow that got through (held still set), the refusal stands whatever the gap and wherever the entry point
            // moved. Only a stream with no refusal behind it is judged on a fresh verdict near its first claim.
            const V3 d = Sub(stuck.location, e.at);
            const bool fresh = e.held && nowMs - e.atMs <= kArmorHeldMaxAgeMs &&
                               std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) <= kArmorHeldMatchUu;
            e.refusing = fresh || (e.refusing && e.held);
        }
        return e.refusing;
    }
    void Forget(uint8_t entity) {   // left, respawned or rejoined: its armour and its blades are new
        for (auto it = entries_.begin(); it != entries_.end();)
            it = std::get<0>(it->first) == entity || std::get<1>(it->first) == entity ? entries_.erase(it) : std::next(it);
    }
    size_t Size() const { return entries_.size(); }

private:
    using Key = std::tuple<uint8_t, uint8_t, uint8_t, std::string>;   // attacker, victim, source, bone
    struct Entry {
        bool held = false;        // the latest verdict: the armour took the whole cut
        V3 at;
        double atMs = 0;
        bool refusing = false;    // the stream's answer
        double stuckMs = -1;      // the stream's latest claim; -1: none yet
    };
    static Key KeyOf(const HitClaim& c) { return Key{c.attacker, c.victim, uint8_t(c.source), c.hitBone}; }
    std::map<Key, Entry> entries_;
};

}  // namespace hsm::combat
