// Generic object identity and announcement (protocol rev 26, plan M0). Game-independent, unit-tested.
//
// Three id kinds share the 64-bit space of session/hsm_lease.h and never collide (told apart by bits 63 and 62):
//   weapon drop   bits 62-63 clear, bits 48-61 clear      MakeWeaponPropId / MakeDropPropId
//   level prop    bit 63 set                              MakeLevelPropId (the host's census, hsm_prop_census.h)
//   spawned       bit 62 set, bit 63 clear                MakeSpawnedId(slot, seq): bits 56-61 the spawning slot,
//                 bits 32-55 clear, low 32 bits a per-slot sequence (SpawnedIdMinter). Anything that appears mid-match.
// ValidObjectId is ValidPropId (the one validity check every decoder uses). An id names an OBJECT; each simulating body
// under it has an index (BodyIndex: root 0, then the actor's components in their order), and BodyState carries both.
//
// A spawned object is announced by the machine that made it appear (ObjectAnnounce). A client's announce is only a
// PROPOSAL (kAnnounceProposal): it carries a provisional id minted in the proposer's own slot. The host reconciles
// proposals (ObjectReconciler): the same object proposed by two clients, or already known, gets ONE id; otherwise the
// host mints a canonical id in its own slot. Only the host's announce (kAnnounceCanonical) binds anyone; its
// proposalId field names the provisional id it answers, so the proposer can re-key its body table. Every other
// machine matches its own actor to a canonical announce by class and place (MatchCensus) or, if it has none, keeps a
// ghost (GhostTable) and streams the object as a transform until its own actor exists.
//
// classHash is a uint32 (sync::WeaponClassHash of the actor's class name, as in CensusEntry); 0 is invalid.
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_lease.h"
#include "session/hsm_prop_census.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

inline bool ValidObjectId(uint64_t id) { return ValidPropId(id); }

constexpr uint8_t kMaxAnnounceBodies = kMaxObjectBodies;     // sane cap on bodies per object (64)
constexpr size_t kMaxKnownObjects = 4096;                    // the host's table; far above any arena's loose objects
constexpr size_t kMaxGhosts = 256;
constexpr double kGhostTtlMs = 10000.0;

// ---------------------------------------------------------------- minting
// One per spawning slot, on that slot's machine. Ids are slot << 56 | seq, seq starting at 1 (so no spawned id is just
// the kind bit). Observe() advances past an id seen from this slot (a machine that reconnected in the same slot, a host
// that took over and heard the old ids), so none is minted twice. Mint() returns 0 once the 32-bit sequence is spent.
class SpawnedIdMinter {
public:
    explicit SpawnedIdMinter(uint8_t slot = kHostSlot, uint32_t firstSeq = 1) : slot_(slot), next_(firstSeq ? firstSeq : 1) {}
    uint8_t Slot() const { return slot_; }
    uint32_t NextSeq() const { return next_; }
    uint64_t Mint() {
        if (slot_ >= kMaxFighters || spent_) return 0;
        const uint64_t id = MakeSpawnedId(slot_, next_);
        if (next_ == 0xFFFFFFFFu) spent_ = true; else ++next_;
        return id;
    }
    void Observe(uint64_t id) {
        if (!ValidSpawnedId(id) || SpawnedIdSlot(id) != slot_) return;
        const uint32_t seq = SpawnedIdSeq(id);
        if (seq >= next_) { if (seq == 0xFFFFFFFFu) spent_ = true; else next_ = seq + 1; }
    }
private:
    uint8_t slot_;
    uint32_t next_;
    bool spent_ = false;
};

// ---------------------------------------------------------------- body index
// The bodies of one object in index order: the root first (index 0), then its components in the actor's component order
// (which every machine shares). A key is whatever the client identifies a component by (a pointer, a name hash). A key
// seen twice keeps its first place; bodies past kMaxObjectBodies are left out.
template <class Key>
class BodyIndex {
public:
    void Build(const Key& root, const std::vector<Key>& components) {
        keys_.clear();
        keys_.push_back(root);
        for (const Key& k : components) {
            if (keys_.size() >= kMaxObjectBodies) break;
            if (std::find(keys_.begin(), keys_.end(), k) == keys_.end()) keys_.push_back(k);
        }
    }
    int IndexOf(const Key& k) const {
        for (size_t i = 0; i < keys_.size(); ++i) if (keys_[i] == k) return int(i);
        return -1;
    }
    const Key* KeyAt(uint8_t body) const { return body < keys_.size() ? &keys_[body] : nullptr; }
    size_t Count() const { return keys_.size(); }
private:
    std::vector<Key> keys_;
};

// ---------------------------------------------------------------- ObjectAnnounce (reliable)
constexpr uint8_t kAnnounceProposal = 1;     // a client's proposal: binds nobody
constexpr uint8_t kAnnounceCanonical = 2;    // the host's: the one id everyone uses
constexpr uint8_t kAnnounceFlagMask = kAnnounceProposal | kAnnounceCanonical;

// Wire: 4 header + 8 objId + 1 flags + 4 classHash + 12 pos + 1 bodyCount + 8 parentId + 8 proposalId = 46 bytes.
struct ObjectAnnounceMsg {
    uint64_t objId = 0;          // proposal: the provisional id (spawned, in the proposer's slot); canonical: the object's id
    uint8_t flags = kAnnounceProposal;   // exactly one of kAnnounceProposal / kAnnounceCanonical
    uint32_t classHash = 0;      // never 0
    float px = 0, py = 0, pz = 0;
    uint8_t bodyCount = 1;       // 1..kMaxAnnounceBodies
    uint64_t parentId = 0;       // 0: none; else the object it came from (armour off a fighter's weapon, a plank off a bridge)
    uint64_t proposalId = 0;     // canonical only: the provisional id this answers (0: not an answer; a host-made object)
};

inline std::vector<uint8_t> Encode(const ObjectAnnounceMsg& m) {
    Writer w; w.header(MsgType::ObjectAnnounce);
    w.u64(m.objId); w.u8(m.flags); w.u32(m.classHash);
    w.f32(m.px); w.f32(m.py); w.f32(m.pz);
    w.u8(m.bodyCount); w.u64(m.parentId); w.u64(m.proposalId);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ObjectAnnounceMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::ObjectAnnounce) return false;
    m.objId = r.u64(); m.flags = r.u8(); m.classHash = r.u32();
    m.px = r.f32(); m.py = r.f32(); m.pz = r.f32();     // Reader::f32 rejects NaN and infinity
    m.bodyCount = r.u8(); m.parentId = r.u64(); m.proposalId = r.u64();
    if (!r.done() || !ValidObjectId(m.objId)) return false;
    if ((m.flags & ~kAnnounceFlagMask) != 0 || (m.flags != kAnnounceProposal && m.flags != kAnnounceCanonical)) return false;
    if (m.classHash == 0 || !CensusPlaceOk(m.px, m.py, m.pz)) return false;
    if (m.bodyCount == 0 || m.bodyCount > kMaxAnnounceBodies) return false;
    if (m.parentId != 0 && (!ValidObjectId(m.parentId) || m.parentId == m.objId)) return false;
    if (m.proposalId != 0 && (m.flags != kAnnounceCanonical || !ValidSpawnedId(m.proposalId) || m.proposalId == m.objId)) return false;
    if (m.flags == kAnnounceProposal && !ValidSpawnedId(m.objId)) return false;   // a proposal's id is always a provisional spawned one
    return true;
}

// ---------------------------------------------------------------- moving objects (0.8.0-pre2)
// A proposal is made from the proposer's view of a moving object, and reaches the host a network trip later; the host's own
// copy of the object has moved on by then. The announce carries no time (rev 26 is fixed), so the host judges the proposal by
// when it RECEIVES it: it moves the known object's place on by its last velocity to that moment (bounded, a body does not
// fly straight for long) and widens the match tolerance with the object's speed (capped), covering the trip itself.
constexpr double kExtrapolateMaxMs = 600.0;     // a known place is carried forward by its velocity for at most this long
constexpr float kMotionSlackS = 0.4f;           // seconds of travel two machines' views of one moving object may differ by
constexpr float kMotionTolCapUu = 500.0f;       // the most the tolerance grows by, cm

inline float SpeedOf(float vx, float vy, float vz) {
    const double s = std::sqrt(double(vx) * vx + double(vy) * vy + double(vz) * vz);
    if (!(s >= 0.0)) return 0.f;                 // NaN
    return s > double(kMaxPropSpeed) ? kMaxPropSpeed : float(s);
}
// The match tolerance for an object moving at speedUuS: the base, plus the distance it covers in kMotionSlackS, capped.
inline float ProposalTolUu(float baseUu, float speedUuS) {
    if (!(speedUuS > 0.f)) return baseUu;
    const float extra = speedUuS * kMotionSlackS;
    return baseUu + (extra > kMotionTolCapUu ? kMotionTolCapUu : extra);
}
// Where a body last seen at (x, y, z) moving at v is ageMs later: a straight line for at most kExtrapolateMaxMs. A result
// no census can carry leaves the place as it was.
inline void ExtrapolatePlace(float& x, float& y, float& z, float vx, float vy, float vz, double ageMs) {
    if (!(ageMs > 0.0)) return;
    const double dt = (ageMs > kExtrapolateMaxMs ? kExtrapolateMaxMs : ageMs) / 1000.0;
    const float nx = float(double(x) + double(vx) * dt), ny = float(double(y) + double(vy) * dt), nz = float(double(z) + double(vz) * dt);
    if (CensusPlaceOk(nx, ny, nz)) { x = nx; y = ny; z = nz; }
}

// ---------------------------------------------------------------- the host reconciles proposals
// What the host knows of every spawned (or adopted) object: class and last known place. A proposal that matches a
// known object (same class, within tolUu of its place: the census's rule, MatchCensus) is that object; otherwise the host
// mints a canonical id in its own slot. A retransmitted proposal (same provisional id) gets the same answer.
class ObjectReconciler {
public:
    struct Result {
        uint64_t objId = 0;          // the canonical id, 0 when the proposal was refused
        bool created = false;        // a new object was minted (announce it); false: an existing one answered
        ObjectAnnounceMsg announce;  // the canonical announce to send everywhere (when objId != 0)
    };
    explicit ObjectReconciler(uint8_t hostSlot = kHostSlot, float tolUu = kCensusMatchUu) : minter_(hostSlot), tol_(tolUu) {}

    // fromSlot: the roster slot the proposal arrived from. A proposal must carry a provisional id minted in that slot.
    // nowMs: the host's clock when the proposal arrived (negative: unknown, no motion is allowed for). A known object with a
    // velocity (UpdateMotion) is first carried forward to nowMs and matched with a tolerance that grows with its speed.
    Result Propose(const ObjectAnnounceMsg& p, uint8_t fromSlot, double nowMs = -1.0) {
        Result out;
        if (p.flags != kAnnounceProposal || !ValidSpawnedId(p.objId) || SpawnedIdSlot(p.objId) != fromSlot) return out;
        if (p.classHash == 0 || p.bodyCount == 0 || p.bodyCount > kMaxAnnounceBodies || !CensusPlaceOk(p.px, p.py, p.pz)) return out;
        if (p.parentId != 0 && !ValidObjectId(p.parentId)) return out;
        if (const auto answered = answers_.find(p.objId); answered != answers_.end()) {
            out.objId = answered->second; out.announce = Announce(answered->second, p.objId); return out;
        }
        // Same object, seen earlier (by another machine's proposal or the host's own spawn)? The nearest of the same class
        // within its own tolerance, as MatchCensus picks for one entry (ties: the lower id).
        uint64_t found = 0;
        float bestD2 = 0.f;
        for (const auto& [id, k] : known_) {
            if (k.classHash != p.classHash) continue;
            float kx = k.px, ky = k.py, kz = k.pz;
            if (nowMs >= 0.0 && k.atMs >= 0.0) ExtrapolatePlace(kx, ky, kz, k.vx, k.vy, k.vz, nowMs - k.atMs);
            const float tol = nowMs >= 0.0 ? ProposalTolUu(tol_, SpeedOf(k.vx, k.vy, k.vz)) : tol_;
            const float dx = p.px - kx, dy = p.py - ky, dz = p.pz - kz;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= tol * tol && (!found || d2 < bestD2)) { found = id; bestD2 = d2; }
        }
        if (found) {
            if (answers_.size() < 4 * kMaxKnownObjects) answers_[p.objId] = found;
            out.objId = found; out.announce = Announce(found, p.objId); return out;
        }
        if (known_.size() >= kMaxKnownObjects) return out;
        const uint64_t id = minter_.Mint();
        if (!id) return out;
        known_[id] = Known{p.classHash, p.px, p.py, p.pz, p.bodyCount, p.parentId, 0.f, 0.f, 0.f, nowMs};
        if (answers_.size() < 4 * kMaxKnownObjects) answers_[p.objId] = id;
        out.objId = id; out.created = true; out.announce = Announce(id, p.objId);
        return out;
    }

    // Whether this proposal would make a new object, rather than answer for one already known. Propose decides the same way.
    bool WouldCreate(const ObjectAnnounceMsg& p, double nowMs) const {
        if (answers_.count(p.objId)) return false;
        for (const auto& [id, k] : known_) {
            if (k.classHash != p.classHash) continue;
            float kx = k.px, ky = k.py, kz = k.pz;
            if (nowMs >= 0.0 && k.atMs >= 0.0) ExtrapolatePlace(kx, ky, kz, k.vx, k.vy, k.vz, nowMs - k.atMs);
            const float tol = nowMs >= 0.0 ? ProposalTolUu(tol_, SpeedOf(k.vx, k.vy, k.vz)) : tol_;
            const float dx = p.px - kx, dy = p.py - ky, dz = p.pz - kz;
            if (dx * dx + dy * dy + dz * dz <= tol * tol) return false;
        }
        return true;
    }

    // The host's own spawn (or an object it adopts by migration): a canonical id in the host's slot. 0 when the table is full.
    uint64_t SpawnLocal(uint32_t classHash, float px, float py, float pz, uint8_t bodyCount, uint64_t parentId = 0) {
        if (classHash == 0 || bodyCount == 0 || bodyCount > kMaxAnnounceBodies || !CensusPlaceOk(px, py, pz) || known_.size() >= kMaxKnownObjects) return 0;
        const uint64_t id = minter_.Mint();
        if (id) known_[id] = Known{classHash, px, py, pz, bodyCount, parentId};
        return id;
    }
    // The host's own body count of an object a client proposed (the proposal carries the proposer's): what later announces
    // say, so every machine compares its copy with the host's.
    void SetBodyCount(uint64_t objId, uint8_t bodyCount) {
        auto it = known_.find(objId);
        if (it != known_.end() && bodyCount >= 1 && bodyCount <= kMaxAnnounceBodies) it->second.bodyCount = bodyCount;
    }
    // Keep a known object's place current (from its BodyState stream), so a late proposal still finds it where it moved to.
    // This form carries no velocity: the place is exact and nothing is carried forward from it.
    void UpdatePlace(uint64_t objId, float px, float py, float pz) {
        auto it = known_.find(objId);
        if (it == known_.end() || !CensusPlaceOk(px, py, pz)) return;
        Known& k = it->second;
        k.px = px; k.py = py; k.pz = pz; k.vx = k.vy = k.vz = 0.f; k.atMs = -1.0;
    }
    // The same with the body's velocity and the host's clock when it was read: Propose carries the place forward from atMs.
    void UpdateMotion(uint64_t objId, float px, float py, float pz, float vx, float vy, float vz, double atMs) {
        auto it = known_.find(objId);
        if (it == known_.end() || !CensusPlaceOk(px, py, pz) || !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) return;
        Known& k = it->second;
        k.px = px; k.py = py; k.pz = pz; k.vx = vx; k.vy = vy; k.vz = vz; k.atMs = atMs;
    }
    // Host migration: the new host seeds its table from an announce every machine already heard (the canonical ids), instead
    // of minting fresh ones. Only a canonical announce of a valid spawned id is taken; the minter moves past the id when it
    // is one of this host's slot, so a later mint never repeats it. An announce for an id already known replaces it. False
    // when it is refused (not canonical, malformed, or the table is full).
    bool Adopt(const ObjectAnnounceMsg& a) {
        if (a.flags != kAnnounceCanonical || !ValidSpawnedId(a.objId) || a.classHash == 0 || a.bodyCount == 0 || a.bodyCount > kMaxAnnounceBodies) return false;
        if (!CensusPlaceOk(a.px, a.py, a.pz) || (a.parentId != 0 && (!ValidObjectId(a.parentId) || a.parentId == a.objId))) return false;
        if (!known_.count(a.objId) && known_.size() >= kMaxKnownObjects) return false;
        minter_.Observe(a.objId);
        known_[a.objId] = Known{a.classHash, a.px, a.py, a.pz, a.bodyCount, a.parentId, 0.f, 0.f, 0.f, -1.0};
        if (a.proposalId != 0 && ValidSpawnedId(a.proposalId) && answers_.size() < 4 * kMaxKnownObjects) answers_[a.proposalId] = a.objId;
        return true;
    }
    void Remove(uint64_t objId) {
        known_.erase(objId);
        for (auto it = answers_.begin(); it != answers_.end();) it = it->second == objId ? answers_.erase(it) : std::next(it);
    }
    bool Knows(uint64_t objId) const { return known_.count(objId) != 0; }
    size_t Size() const { return known_.size(); }
    // The canonical announce of a known object, for a late joiner (proposalId 0).
    bool AnnounceOf(uint64_t objId, ObjectAnnounceMsg& out) const {
        if (!known_.count(objId)) return false;
        out = Announce(objId, 0); return true;
    }
    std::vector<ObjectAnnounceMsg> AnnounceAll() const {
        std::vector<ObjectAnnounceMsg> v;
        for (const auto& [id, k] : known_) v.push_back(Announce(id, 0));
        return v;
    }
    void Clear() { known_.clear(); answers_.clear(); }
    SpawnedIdMinter& Minter() { return minter_; }

private:
    struct Known {
        uint32_t classHash = 0; float px = 0, py = 0, pz = 0; uint8_t bodyCount = 1; uint64_t parentId = 0;
        float vx = 0, vy = 0, vz = 0; double atMs = -1.0;   // the last velocity and the host clock it was read at (negative: none)
    };
    ObjectAnnounceMsg Announce(uint64_t id, uint64_t proposalId) const {
        const Known& k = known_.at(id);
        ObjectAnnounceMsg a; a.objId = id; a.flags = kAnnounceCanonical; a.classHash = k.classHash;
        a.px = k.px; a.py = k.py; a.pz = k.pz; a.bodyCount = k.bodyCount; a.parentId = k.parentId; a.proposalId = proposalId;
        return a;
    }
    SpawnedIdMinter minter_;
    float tol_;
    std::map<uint64_t, Known> known_;
    std::map<uint64_t, uint64_t> answers_;      // provisional id -> canonical id (a retransmitted proposal gets the same answer)
};

// ---------------------------------------------------------------- ghosts
// A canonical object this machine has no actor for yet: it is streamed as a ghost transform until the actor exists (a mod
// that spawns on every machine, a drop every stand-in makes). Entries expire after kGhostTtlMs without being touched.
class GhostTable {
public:
    // False when the table is full (a new id is refused); an id already present is refreshed.
    bool Note(uint64_t objId, double nowMs) {
        auto it = ghosts_.find(objId);
        if (it == ghosts_.end() && ghosts_.size() >= kMaxGhosts) return false;
        ghosts_[objId] = nowMs;
        return true;
    }
    bool IsGhost(uint64_t objId) const { return ghosts_.count(objId) != 0; }
    // The machine found or made its own actor: the ghost ends. True if there was one.
    bool Resolve(uint64_t objId) { return ghosts_.erase(objId) != 0; }
    // Drop ghosts not refreshed for kGhostTtlMs; returns their ids.
    std::vector<uint64_t> Expire(double nowMs) {
        std::vector<uint64_t> gone;
        for (auto it = ghosts_.begin(); it != ghosts_.end();) {
            if (nowMs - it->second >= kGhostTtlMs) { gone.push_back(it->first); it = ghosts_.erase(it); } else ++it;
        }
        return gone;
    }
    size_t Size() const { return ghosts_.size(); }
    void Clear() { ghosts_.clear(); }
private:
    std::map<uint64_t, double> ghosts_;
};

}  // namespace hsm
