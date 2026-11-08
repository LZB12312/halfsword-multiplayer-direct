// What this machine accepts from other players, whatever they're running. Nothing on the sender's PC can be trusted to hold a limit, so
// each receiver holds its own: a custom build gets no further than an honest one. These sit above anything normal play reaches, because
// going over drops the thing without telling anyone but the log. Game-independent.
#pragma once
#include "combat/hsm_claim.h"
#include "session/hsm_budget.h"
#include <cmath>
#include <cstdint>
#include <map>

namespace hsm {

// Objects one player can make appear for everyone. The host checks these before it announces one.
constexpr double kSpawnsPerSecond = 5.0, kSpawnBurst = 20.0;
constexpr int kMaxLiveSpawnsPerSlot = 128;

class SpawnGate {
public:
    enum class Verdict { Allowed, TooFast, TooMany };
    Verdict Check(uint8_t slot, double nowMs) {
        Slot& s = slots_[slot];
        if (s.live >= kMaxLiveSpawnsPerSlot) return Verdict::TooMany;
        if (!s.rate.Allow(nowMs)) return Verdict::TooFast;
        return Verdict::Allowed;
    }
    // A new object that came from this slot, and that object going away.
    void OnCreated(uint64_t objId, uint8_t slot) { owner_[objId] = slot; ++slots_[slot].live; }
    void OnGone(uint64_t objId) {
        const auto it = owner_.find(objId);
        if (it == owner_.end()) return;
        if (slots_[it->second].live > 0) --slots_[it->second].live;
        owner_.erase(it);
    }
    void Reset() { slots_.clear(); owner_.clear(); }
    int Live(uint8_t slot) { return slots_[slot].live; }

private:
    struct Slot { TokenBucket rate{kSpawnsPerSecond, kSpawnBurst}; int live = 0; };
    std::map<uint8_t, Slot> slots_;
    std::map<uint64_t, uint8_t> owner_;
};

// A blow from another player. The decoder has already refused anything that isn't a real number, but a real number can still be absurd,
// and the game's own damage code was never written to expect that.
constexpr double kMaxClaimScalar = 1e9;       // any power or rate, far above HS Enhancer's highest settings on a heavy weapon
constexpr double kMaxClaimVector = 1e8;       // velocity and impulse
constexpr double kMaxClaimPlace = 1e5;        // the hit's place, in the hit bone's frame: a kilometre away isn't a hit on that bone
constexpr double kBlowsPerSecond = 60.0, kBlowBurst = 120.0;

inline bool ClaimValuesSane(const combat::HitClaim& c) {
    const auto small = [](double v, double max) { return std::isfinite(v) && std::fabs(v) <= max; };
    const auto vec = [&](const auto& v, double max) { return small(v.x, max) && small(v.y, max) && small(v.z, max); };
    return small(c.cuttingPower, kMaxClaimScalar) && small(c.stabRate, kMaxClaimScalar) && small(c.rigidity, kMaxClaimScalar) && small(c.kickPower, kMaxClaimScalar) &&
           small(c.drawCut, kMaxClaimScalar) && small(c.rawDamage, kMaxClaimScalar) && small(double(c.bluntDestruction), kMaxClaimScalar) &&
           vec(c.hitVelocity, kMaxClaimVector) && vec(c.hitImpulse, kMaxClaimVector) && vec(c.propVelocity, kMaxClaimVector) && vec(c.location, kMaxClaimPlace);
}

// One bucket per attacker, so one player's flood doesn't cost anyone else's blows.
class BlowGate {
public:
    bool Allow(uint8_t attacker, double nowMs) { return buckets_.try_emplace(attacker, kBlowsPerSecond, kBlowBurst).first->second.Allow(nowMs); }
    void Reset() { buckets_.clear(); }

private:
    std::map<uint8_t, TokenBucket> buckets_;
};

}  // namespace hsm
