// Severs and stuck blades in owner sync (protocol rev 9, MsgType::SeverClaim and MsgType::StuckBlade; rev 12,
// MsgType::SeverNotice).
//
// A sever starts natively on the attacker's screen: Constraint_Weapon_Stuck_BP::Call_Dismember (the only caller) runs
// the struck Willie's "Dismember Function Initiate" and, 0.1 s later, "Dismember Function Delayed" with the Willie's
// mesh, the part (Enum_DismembermentPart 0..14), the farthest overlapped marker, every overlapped marker, two vertex
// paint boxes and the weapon. When that Willie is a stand-in the attacker captures the call as a SeverClaim: marker
// and component names (stable Willie_BP component names, the same on every machine), and the two boxes in one bone's
// frame. The victim's owner rebuilds the call on its own fighter and runs the game's own functions; everyone else shows
// it on their stand-in once the owner's vitals confirm the part is gone (SeverPending). A sever the owner's fighter takes
// from its own blade (or, on the host, from a bot) runs natively there; the owner sends the same claim as a SeverNotice,
// naming its fighter twice, and the others show it the same way.
//
// A blade stuck in a stand-in is reported on/off (StuckBladeMsg); the victim's owner constrains the attacker's
// stand-in weapon to its own bone while it is stuck (StuckBlades). Pure types, no engine headers.
#pragma once
#include "combat/hsm_claim.h"
#include "hsm_protocol.h"
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hsm::combat {

constexpr uint8_t kSeverParts = 15;              // Enum_DismembermentPart 0..14
constexpr size_t kSeverMaxMarkers = 16;          // overlapped markers carried per claim (blood spots)
constexpr double kSeverMaxExtent = 2000.0;       // uu; the game's boxes are (100, 500, 100)
constexpr double kSeverMaxBoxOffset = 2000.0;    // uu from the frame bone; the game's boxes sit about 500 away
constexpr double kSeverRepeatMs = 500.0;         // the attacker sends one claim per (victim, part) per this
constexpr double kSeverPendingMs = 3000.0;       // a claim waits this long for the owner's vitals to confirm it
// Limbs can use the hide-bone fallback after 400 ms; the pelvis cannot be hidden
// without hiding the whole fighter. Keep native pelvis geometry recoverable for
// the bounded pending lifetime instead of permanently leaving an intact torso.
inline bool NativeSeverDisplayFresh(uint8_t part, double ageMs) {
    return part < kSeverParts && ageMs >= 0 && ageMs <= (part == 2 ? kSeverPendingMs : 300.0);
}
constexpr uint32_t kSeverDelayedMs = 100;        // Constraint_Weapon_Stuck_BP.cpp:666: Delay 0.1 s, then Delayed
constexpr double kStuckBladeMaxMs = 10000.0;     // a stuck blade never reported released lets go after this

// Willie_BP.cpp:3269, Dismember Function Delayed: Spawn Bone = switch(Currently Dismembered Part){...}.
inline const char* SeverSpawnBone(uint8_t part) {
    static constexpr const char* kSpawnBone[kSeverParts] = {
        "neck_01", "neck_01", "pelvis", "upperarm_r", "lowerarm_r", "hand_r", "upperarm_l", "lowerarm_l",
        "hand_l", "thigh_r", "calf_r", "foot_r", "thigh_l", "calf_l", "foot_l"};
    return part < kSeverParts ? kSpawnBone[part] : nullptr;
}

struct SeverBox {
    Xform local;   // the box's world transform in the frame bone's frame
    V3 extent;     // half-extents
};

struct SeverClaim {
    uint32_t claimId = 0;
    uint8_t attacker = 0, victim = 0;
    uint8_t victimLife = 0;                 // rev 19: the Spawn.life of the victim body the attacker saw (0 = unknown)
    uint32_t attackerTsMs = 0;              // the attacker's stream clock
    uint8_t part = 0;                       // Enum_DismembermentPart
    std::string masterMesh;                 // the victim mesh component's name; empty: its Mesh
    std::string attachMarker;               // "Farthest Overlap Marker", a component of the victim
    std::vector<std::string> markers;       // "Overlapped Markers", components of the victim
    std::string frameBone;                  // the victim bone both boxes are expressed in
    SeverBox box1, box2;                    // "VP Box Collision 1/2"
    StrikeSource source = StrikeSource::Body;
    std::string weaponComponent;            // "Dismember Weapon" on the attacker; empty: its weapon root
};

// What the attacker's pre-hook reads off the call, in world space.
struct SeverCapture {
    uint32_t claimId = 0;
    uint8_t attacker = 0, victim = 0;
    uint32_t attackerTsMs = 0;
    uint8_t part = 0;
    std::string masterMesh, attachMarker;
    std::vector<std::string> markers;
    std::string frameBone;
    Xform boneWorld;                        // frameBone on the stand-in, now
    Xform box1World, box2World;
    V3 box1Extent, box2Extent;
    StrikeSource source = StrikeSource::Body;
    std::string weaponComponent;
};

namespace detail {
inline bool FitsWire(const std::string& s) { return s.size() <= kMaxClaimString; }
inline bool ExtentOk(const V3& e) {
    return std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z) && e.x >= 0 && e.y >= 0 && e.z >= 0 &&
           e.x <= kSeverMaxExtent && e.y <= kSeverMaxExtent && e.z <= kSeverMaxExtent;
}
inline bool OffsetOk(const V3& p) {
    return std::fabs(p.x) <= kSeverMaxBoxOffset && std::fabs(p.y) <= kSeverMaxBoxOffset && std::fabs(p.z) <= kSeverMaxBoxOffset;
}
// A local transform the victim can place: bounded offset, a unit rotation, a positive non-degenerate scale.
inline bool LocalXformOk(const Xform& x) {
    if (!OffsetOk(x.pos)) return false;
    const double n = x.rot.x * x.rot.x + x.rot.y * x.rot.y + x.rot.z * x.rot.z + x.rot.w * x.rot.w;
    if (!(std::fabs(n - 1.0) <= 0.01)) return false;
    for (double s : {x.scale.x, x.scale.y, x.scale.z})
        if (!(s > 1e-3 && s <= 1e3)) return false;
    return true;
}
inline SeverBox ToBoneFrame(const Xform& bone, const Xform& boxWorld, const V3& extent) {
    SeverBox b;
    b.local.pos = bone.ToLocal(boxWorld.pos);
    b.local.rot = combat::QuatMul(Conjugate(bone.rot), boxWorld.rot);
    b.local.scale = boxWorld.scale;   // carried as-is, as the hit claim's Hit Box
    b.extent = extent;
    return b;
}
// The body a claim and a notice share; the caller checks the part and who struck whom.
inline bool BuildSever(const SeverCapture& in, SeverClaim& out) {
    if (in.attachMarker.empty() || in.frameBone.empty()) return false;
    if (!detail::FitsWire(in.attachMarker) || !detail::FitsWire(in.frameBone) || !detail::FitsWire(in.masterMesh) ||
        !detail::FitsWire(in.weaponComponent))
        return false;
    if (!detail::ExtentOk(in.box1Extent) || !detail::ExtentOk(in.box2Extent)) return false;
    SeverClaim c;
    c.claimId = in.claimId;
    c.attacker = in.attacker;
    c.victim = in.victim;
    c.attackerTsMs = in.attackerTsMs;
    c.part = in.part;
    c.masterMesh = in.masterMesh;
    c.attachMarker = in.attachMarker;
    for (const std::string& m : in.markers) {
        if (c.markers.size() >= kSeverMaxMarkers) break;
        if (!m.empty() && detail::FitsWire(m)) c.markers.push_back(m);
    }
    c.frameBone = in.frameBone;
    c.box1 = detail::ToBoneFrame(in.boneWorld, in.box1World, in.box1Extent);
    c.box2 = detail::ToBoneFrame(in.boneWorld, in.box2World, in.box2Extent);
    if (!detail::LocalXformOk(c.box1.local) || !detail::LocalXformOk(c.box2.local)) return false;
    c.source = in.source;
    c.weaponComponent = in.weaponComponent;
    out = std::move(c);
    return true;
}
}  // namespace detail

// The claim for a captured call, or false when it cannot travel (no part, no attach marker, no frame bone, a name the
// wire would truncate, a box out of bounds). Markers that cannot travel are dropped; at most kSeverMaxMarkers are kept.
inline bool MakeSeverClaim(const SeverCapture& in, SeverClaim& out) {
    if (in.part >= kSeverParts || in.attacker == in.victim) return false;
    return detail::BuildSever(in, out);
}

// The notice for an own cut: the capture names the owner's fighter twice (attacker == victim); same rules otherwise.
inline bool MakeSeverNotice(const SeverCapture& in, SeverClaim& out) {
    if (in.part >= kSeverParts || in.attacker != in.victim) return false;
    return detail::BuildSever(in, out);
}

// A box of a claim placed on the victim's own bone.
inline Xform SeverBoxWorld(const SeverBox& b, const Xform& boneWorld) {
    Xform w;
    w.pos = boneWorld.FromLocal(b.local.pos);
    w.rot = combat::QuatMul(boneWorld.rot, b.local.rot);
    w.scale = b.local.scale;
    return w;
}

// ---------------------------------------------------------------- SeverClaim on the wire
struct SeverClaimMsg {
    SeverClaim claim;
};

namespace detail {
// The wire body a SeverClaim and a SeverNotice share (after the header).
inline void PutSeverBody(Writer& w, const SeverClaim& c) {
    w.u32(c.claimId);
    w.u8(c.attacker);
    w.u8(c.victim);
    w.u8(c.victimLife);
    w.u32(c.attackerTsMs);
    w.u8(c.part);
    w.str(c.masterMesh, kMaxClaimString);
    w.str(c.attachMarker, kMaxClaimString);
    w.u8(uint8_t(c.markers.size() < 255 ? c.markers.size() : 255));
    for (size_t i = 0; i < c.markers.size() && i < 255; ++i) w.str(c.markers[i], kMaxClaimString);
    w.str(c.frameBone, kMaxClaimString);
    PutXform(w, c.box1.local);
    PutV3(w, c.box1.extent);
    PutXform(w, c.box2.local);
    PutV3(w, c.box2.extent);
    w.u8(uint8_t(c.source));
    w.str(c.weaponComponent, kMaxClaimString);
}

// Reads that body with every check that does not depend on who struck whom; the caller checks the two entities.
inline bool GetSeverBody(Reader& r, SeverClaim& c) {
    c.claimId = r.u32();
    c.attacker = r.u8();
    c.victim = r.u8();
    c.victimLife = r.u8();
    c.attackerTsMs = r.u32();
    c.part = r.u8();
    c.masterMesh = r.str(kMaxClaimString);
    c.attachMarker = r.str(kMaxClaimString);
    const size_t count = r.u8();
    if (!r.ok() || count > kSeverMaxMarkers) return false;
    for (size_t i = 0; i < count; ++i) {
        c.markers.push_back(r.str(kMaxClaimString));
        if (c.markers.back().empty()) return false;
    }
    c.frameBone = r.str(kMaxClaimString);
    bool numOk = true;
    numOk &= GetXform(r, c.box1.local);
    numOk &= GetV3(r, c.box1.extent);
    numOk &= GetXform(r, c.box2.local);
    numOk &= GetV3(r, c.box2.extent);
    const uint8_t src = r.u8();
    c.weaponComponent = r.str(kMaxClaimString);
    if (!r.done() || !numOk) return false;
    if (c.part >= kSeverParts || c.attachMarker.empty() || c.frameBone.empty()) return false;
    if (src > uint8_t(StrikeSource::WeaponLeft)) return false;
    if (!detail::ExtentOk(c.box1.extent) || !detail::ExtentOk(c.box2.extent)) return false;
    if (!detail::LocalXformOk(c.box1.local) || !detail::LocalXformOk(c.box2.local)) return false;
    c.source = StrikeSource(src);
    return true;
}
}  // namespace detail

inline std::vector<uint8_t> Encode(const SeverClaimMsg& m) {
    Writer w;
    w.header(MsgType::SeverClaim);
    detail::PutSeverBody(w, m.claim);
    return w.buf;
}

inline bool Decode(const uint8_t* d, size_t n, SeverClaimMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n);
    MsgType t;
    if (!r.header(t) || t != MsgType::SeverClaim) return false;
    SeverClaim c;
    if (!detail::GetSeverBody(r, c)) return false;
    if (c.attacker < 1 || c.attacker > kMaxWillies || c.victim < 1 || c.victim > kMaxWillies || c.attacker == c.victim)
        return false;
    m.claim = std::move(c);
    return true;
}

// ---------------------------------------------------------------- SeverNotice on the wire
// A sever the owner's own fighter took natively: the claim body with that fighter named twice. Nobody replays it; the
// others keep it like a claim (SeverPending) and show it once the owner's vitals confirm the part.
struct SeverNoticeMsg {
    SeverClaim claim;
};

inline std::vector<uint8_t> Encode(const SeverNoticeMsg& m) {
    Writer w;
    w.header(MsgType::SeverNotice);
    detail::PutSeverBody(w, m.claim);
    return w.buf;
}

inline bool Decode(const uint8_t* d, size_t n, SeverNoticeMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n);
    MsgType t;
    if (!r.header(t) || t != MsgType::SeverNotice) return false;
    SeverClaim c;
    if (!detail::GetSeverBody(r, c)) return false;
    if (c.victim < 1 || c.victim > kMaxWillies || c.attacker != c.victim) return false;
    m.claim = std::move(c);
    return true;
}

// ---------------------------------------------------------------- attacker side
// The stand-in's own markers keep re-triggering Call Dismember once their HP tags run out; one claim per (victim, part)
// per kSeverRepeatMs is enough (the victim's Initiate refuses a part that is already gone).
class SeverLimiter {
public:
    bool Allow(uint8_t victim, uint8_t part, double nowMs) {
        if (part >= kSeverParts) return false;
        auto key = std::make_pair(victim, part);
        auto it = last_.find(key);
        if (it != last_.end() && nowMs - it->second < kSeverRepeatMs) return false;
        last_[key] = nowMs;
        return true;
    }
    void Forget(uint8_t victim) {
        for (auto it = last_.begin(); it != last_.end();) it = it->first.first == victim ? last_.erase(it) : std::next(it);
    }

private:
    std::map<std::pair<uint8_t, uint8_t>, double> last_;   // at most 256 x kSeverParts entries
};

// ---------------------------------------------------------------- victim side
// Delayed only follows an Initiate that started this sever: Initiate's gate (Willie_BP.cpp:3651) sets Dismemberment In
// Process and Currently Dismembered Part only when it passes, and refuses while another sever is in process.
inline bool InitiateAccepted(bool inProcessBefore, bool inProcessAfter, uint8_t currentPartAfter, uint8_t part) {
    return !inProcessBefore && inProcessAfter && currentPartAfter == part;
}

// ---------------------------------------------------------------- everyone else: claims waiting for the vitals
// A machine that does not own the victim keeps the newest claim per (victim, part) until the owner's vitals report the
// part severed (Take: show it natively on the stand-in) or kSeverPendingMs pass.
class SeverPending {
public:
    void Put(const SeverClaim& c, double nowMs) {
        if (c.part >= kSeverParts) return;
        pending_[std::make_pair(c.victim, c.part)] = Entry{c, nowMs};
    }
    bool Take(uint8_t victim, uint8_t part, double nowMs, SeverClaim& out) {
        auto it = pending_.find(std::make_pair(victim, part));
        if (it == pending_.end()) return false;
        const bool fresh = nowMs - it->second.atMs < kSeverPendingMs;
        if (fresh) out = it->second.claim;
        pending_.erase(it);
        return fresh;
    }
    bool Has(uint8_t victim, uint8_t part) const { return pending_.count(std::make_pair(victim, part)) != 0; }
    void Expire(double nowMs) {
        for (auto it = pending_.begin(); it != pending_.end();)
            it = nowMs - it->second.atMs >= kSeverPendingMs ? pending_.erase(it) : std::next(it);
    }
    void Forget(uint8_t victim) {
        for (auto it = pending_.begin(); it != pending_.end();) it = it->first.first == victim ? pending_.erase(it) : std::next(it);
    }
    void Clear() { pending_.clear(); }
    size_t Size() const { return pending_.size(); }

private:
    struct Entry {
        SeverClaim claim;
        double atMs = 0;
    };
    std::map<std::pair<uint8_t, uint8_t>, Entry> pending_;   // at most 256 x kSeverParts entries
};

// ---------------------------------------------------------------- StuckBlade
// The attacker numbers each sticking (seq, per machine); the release carries the same seq.
struct StuckBladeMsg {
    uint8_t attacker = 0, victim = 0;
    StrikeSource source = StrikeSource::WeaponRight;   // the hand holding the stuck weapon
    bool stuck = false;
    uint16_t seq = 0;
    std::string bone;                                  // the victim bone it is stuck in; empty on release
    std::string weaponComponent;                       // the stuck component on the attacker's weapon
    Xform weaponInBone;                                // that component's transform in the bone's frame
};

inline std::vector<uint8_t> Encode(const StuckBladeMsg& m) {
    Writer w;
    w.header(MsgType::StuckBlade);
    w.u8(m.attacker);
    w.u8(m.victim);
    w.u8(uint8_t(m.source));
    w.u8(m.stuck ? 1 : 0);
    w.u16(m.seq);
    w.str(m.bone, kMaxClaimString);
    w.str(m.weaponComponent, kMaxClaimString);
    PutXform(w, m.weaponInBone);
    return w.buf;
}

inline bool Decode(const uint8_t* d, size_t n, StuckBladeMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n);
    MsgType t;
    if (!r.header(t) || t != MsgType::StuckBlade) return false;
    StuckBladeMsg o;
    o.attacker = r.u8();
    o.victim = r.u8();
    const uint8_t src = r.u8();
    const uint8_t stuck = r.u8();
    o.seq = r.u16();
    o.bone = r.str(kMaxClaimString);
    o.weaponComponent = r.str(kMaxClaimString);
    const bool numOk = GetXform(r, o.weaponInBone);
    if (!r.done() || !numOk) return false;
    if (o.attacker < 1 || o.attacker > kMaxWillies || o.victim < 1 || o.victim > kMaxWillies || o.attacker == o.victim)
        return false;
    if (src != uint8_t(StrikeSource::WeaponRight) && src != uint8_t(StrikeSource::WeaponLeft)) return false;
    if (stuck > 1) return false;
    o.source = StrikeSource(src);
    o.stuck = stuck == 1;
    if (o.stuck && (o.bone.empty() || !detail::LocalXformOk(o.weaponInBone))) return false;
    m = std::move(o);
    return true;
}

// The victim's view of which attacker hands are stuck in it. One sticking per (attacker, hand); a newer seq replaces it.
class StuckBlades {
public:
    enum class Change { None, Attach, Reattach, Release };
    struct Key {
        uint8_t attacker;
        StrikeSource source;
    };

    Change On(const StuckBladeMsg& m, double nowMs) {
        auto& e = entries_[KeyOf(m.attacker, m.source)];
        const bool newer = !e.have || int16_t(uint16_t(m.seq - e.seq)) > 0;   // wrap-aware, as hsm::SeqNewer
        if (m.stuck) {
            if (!newer) return Change::None;
            const Change c = e.stuck ? Change::Reattach : Change::Attach;
            e = Entry{true, m.seq, true, m.victim, nowMs};
            return c;
        }
        if (e.have && m.seq == e.seq && e.stuck) {
            e.stuck = false;
            return Change::Release;
        }
        if (!newer) return Change::None;
        const bool wasStuck = e.stuck;   // a release of a sticking we never saw: whatever this hand held is over
        e = Entry{true, m.seq, false, m.victim, nowMs};
        return wasStuck ? Change::Release : Change::None;
    }
    // Releases every sticking older than kStuckBladeMaxMs.
    std::vector<Key> Expire(double nowMs) {
        std::vector<Key> out;
        for (auto& [k, e] : entries_)
            if (e.stuck && nowMs - e.sinceMs >= kStuckBladeMaxMs) {
                e.stuck = false;
                out.push_back(KeyFrom(k));
            }
        return out;
    }
    // The entity left or respawned: drops everything it is part of; returns the stickings that were live.
    std::vector<Key> Forget(uint8_t entity) {
        std::vector<Key> out;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->first.first == entity || it->second.victim == entity) {
                if (it->second.stuck) out.push_back(KeyFrom(it->first));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }
    size_t Count() const {
        size_t n = 0;
        for (const auto& kv : entries_) n += kv.second.stuck ? 1 : 0;
        return n;
    }

private:
    struct Entry {
        bool have = false;
        uint16_t seq = 0;
        bool stuck = false;
        uint8_t victim = 0;
        double sinceMs = 0;
    };
    using MapKey = std::pair<uint8_t, uint8_t>;
    static MapKey KeyOf(uint8_t attacker, StrikeSource s) { return {attacker, uint8_t(s)}; }
    static Key KeyFrom(const MapKey& k) { return Key{k.first, StrikeSource(k.second)}; }
    std::map<MapKey, Entry> entries_;   // at most 256 x 3 entries
};

}  // namespace hsm::combat
