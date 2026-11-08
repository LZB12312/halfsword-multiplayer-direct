// A per-peer receive budget (security audit M3): a token bucket per message class, so one peer cannot flood the host
// (which applies and relays most of what it gets to N-1 others) or a hostile host flood a client. Each message the
// session's receive path is about to handle asks Allow(type, nowMs); a message over its class's budget is dropped
// before it is decoded further, applied or relayed, and counted. Pure state, no engine headers.
//
// The numbers are one machine's own legitimate traffic times a margin of 2 or more. A machine owns one fighter, so
// toward the host a client sends at most: Pose + Vitals about 60 Hz each, BodyState for the bodies it leases (up to ~20
// at 60 Hz), claims bounded by physics contacts, PaintFx at most 6 kinds x 30/s (the sender's own FxLimiter). The host
// relays every fighter's streams to a client (8 fighters + up to 10 bots, kMaxWillies), so a client's budget for what
// the HOST sends is the per-peer one times kMaxWillies (MsgBudget(kMaxWillies)). Classes that carry setup or are
// already bounded elsewhere (Hello's own repeat gate, Chat's ChatRateLimiter, Travel, Spawn, MatchInfo, Event, ...) are
// not budgeted here (BudgetClass::Free). Normal play, a full 8-fighter lobby with 10 bots and many props, stays far
// below every limit; only a flood reaches one.
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace hsm {

enum class BudgetClass : uint8_t { Free, Stream, Prop, Lease, Combat, Fx, Blob, Control, Detour, Count };

inline BudgetClass BudgetClassOf(MsgType t) {
    switch (t) {
        case MsgType::Input: case MsgType::Snapshot: case MsgType::RelayInput:
        case MsgType::OwnerState: case MsgType::Pose: case MsgType::Vitals:
            return BudgetClass::Stream;
        case MsgType::BodyState: return BudgetClass::Prop;
        case MsgType::PropLease: case MsgType::ObjectAnnounce: case MsgType::ObjectEvent: return BudgetClass::Lease;
        case MsgType::CombatEvent: case MsgType::HitClaim: case MsgType::HitFx: case MsgType::SeverClaim:
        case MsgType::SeverNotice: case MsgType::StuckBlade: case MsgType::GrabState: case MsgType::Equip:
        case MsgType::DeathReport: case MsgType::GiveUp: case MsgType::RemoteWrite:   // rev 31: at most a few pushes a second per pair
            return BudgetClass::Combat;
        case MsgType::PaintFx: return BudgetClass::Fx;
        case MsgType::BlobChunk: case MsgType::LoadoutChange: return BudgetClass::Blob;   // rev 36: LoadoutChange is a BlobChunk set, client -> host
        case MsgType::Ready: case MsgType::ReportShare: case MsgType::MeshHeard: case MsgType::PingTable:
            return BudgetClass::Control;
        case MsgType::ModMap: return BudgetClass::Control;   // rev 27: a host sends one per map pick (and one to each joiner)
        case MsgType::LobbySecret: return BudgetClass::Control;   // rev 35: host -> client only, one per password change (and one to each joiner)
        case MsgType::ProgressionVote: return BudgetClass::Control;   // rev 36: the Tavern's next-fight vote: a state change or a second, a pick per click
        case MsgType::ProgressionResult: return BudgetClass::Control;   // rev 41: host -> client only, one per Progression fight
        case MsgType::LevelSeed: return BudgetClass::Control;   // rev 28: host -> client only, one per travel (and one to each joiner)
        case MsgType::ModList: case MsgType::ModAnswer:          // rev 40: one per mod change / per code challenge (60..180 s)
        case MsgType::SessionMods: case MsgType::ModChallenge:
            return BudgetClass::Control;
        case MsgType::HandOff: case MsgType::HandOffAsk:          // rev 32: a few per grab or stab (an ask every kAskEveryMs while unanswered)
            return BudgetClass::Combat;
        case MsgType::Forward: return BudgetClass::Detour;
        default: return BudgetClass::Free;
    }
}

struct BudgetRate { double perSec; double burst; };

// Per class, for ONE peer's own traffic: sustained messages per second and the burst the bucket holds.
//   Stream  300/s burst 600   (legit ~120/s: Pose + Vitals at 60 Hz)
//   Prop   1500/s burst 3000  (legit up to ~1200/s: 20 leased props at 60 Hz)
//   Lease   300/s burst 600   (legit a few per prop per second: requests, 1 s renewals, releases)
//   Combat  600/s burst 1200  (a pile-up of weapon contacts; the claim senders have their own limits)
//   Fx      360/s burst 720   (legit at most 180/s: 6 kinds x kFxMaxPerSecond)
//   Blob    200/s burst 600   (one loadout is 32 chunks; a joiner gets every fighter's: ~200 in one burst)
//   Control  20/s burst 60    (Ready, ReportShare, MeshHeard, PingTable: a few per join, level load or 10 s)
//   Detour 1200/s burst 2400  (a relay passes on up to 7 other clients' Pose + Vitals: ~840/s; hsm_detour.h caps each pair)
constexpr BudgetRate kBudgetRates[size_t(BudgetClass::Count)] = {
    {0, 0},          // Free: never budgeted
    {300, 600}, {1500, 3000}, {300, 600}, {600, 1200}, {360, 720}, {200, 600}, {20, 60}, {1200, 2400},
};

class MsgBudget {
public:
    // scale multiplies every rate and burst: 1 for one peer's own traffic; kMaxWillies for a client counting what the
    // host sends it (everyone's streams on one link).
    MsgBudget() : scale_(1.0) {}
    explicit MsgBudget(double scale) : scale_(scale > 0 ? scale : 1.0) {}

    // True if a message of type t may be handled now. A message over its class's budget is dropped: the caller returns.
    bool Allow(MsgType t, double nowMs) {
        const BudgetClass c = BudgetClassOf(t);
        if (c == BudgetClass::Free) return true;
        Bucket& b = buckets_[size_t(c)];
        const BudgetRate& r = kBudgetRates[size_t(c)];
        const double burst = r.burst * scale_;
        if (!b.started) { b.started = true; b.tokens = burst; b.lastMs = nowMs; }
        const double dt = nowMs - b.lastMs;   // a clock that went backwards refills nothing
        if (dt > 0) { b.tokens = std::min(burst, b.tokens + dt * r.perSec * scale_ / 1000.0); b.lastMs = nowMs; }
        else if (dt < 0) b.lastMs = nowMs;
        if (b.tokens < 1.0) { ++b.dropped; ++droppedTotal_; return false; }
        b.tokens -= 1.0;
        return true;
    }

    uint64_t Dropped(BudgetClass c) const { return buckets_[size_t(c)].dropped; }
    uint64_t DroppedTotal() const { return droppedTotal_; }

    // Whether the n-th drop of a class is worth a log line: the first three, then every 2000th (a flood is one line a
    // second or less, not one per message).
    static bool ShouldLog(uint64_t n) { return n >= 1 && (n <= 3 || n % 2000 == 0); }

    static const char* ClassName(BudgetClass c) {
        static const char* const kNames[] = {"free", "stream", "prop", "lease", "combat", "fx", "blob", "control", "detour"};
        return kNames[size_t(c) < size_t(BudgetClass::Count) ? size_t(c) : 0];
    }

private:
    struct Bucket { double tokens = 0, lastMs = 0; bool started = false; uint64_t dropped = 0; };
    std::array<Bucket, size_t(BudgetClass::Count)> buckets_{};
    double scale_;
    uint64_t droppedTotal_ = 0;
};

// A plain token bucket for a caller with one budget of its own (e.g. how many object scans per second).
class TokenBucket {
public:
    TokenBucket(double perSec, double burst) : perSec_(perSec), burst_(burst), tokens_(burst) {}
    bool Allow(double nowMs) {
        if (!started_) { started_ = true; lastMs_ = nowMs; }
        const double dt = nowMs - lastMs_;
        if (dt > 0) { tokens_ = std::min(burst_, tokens_ + dt * perSec_ / 1000.0); lastMs_ = nowMs; }
        else if (dt < 0) lastMs_ = nowMs;
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

private:
    double perSec_, burst_, tokens_, lastMs_ = 0;
    bool started_ = false;
};

}  // namespace hsm
