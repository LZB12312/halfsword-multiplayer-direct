// Mesh (protocol rev 19), pure parts. Clients stream Pose/Vitals to each other as well as to the host; each client
// reports which slots it hears directly (MeshHeard) and the host stops relaying those slots' streams to it. A mask of 0
// is the 0.6.5 behaviour (the host relays everything), so a lost report or a dead direct path only costs bandwidth.
// Mesh v2 (rev 22): a client also sends its blows, severs, stuck blades, paint, equip, grab and prop messages straight
// to the other clients. The host still gets every one, checks it, scores it and relays it as before, so a receiver can
// get a message twice (direct and relayed): MeshDedupe keeps the first copy. A dead direct path costs nothing but time.
#pragma once
#include "session/hsm_authority.h"
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace hsm {

static_assert(kMaxFighters <= 16, "MeshHeard and the mesh masks carry one bit per slot in 16 bits");

constexpr uint32_t kMeshHoldMs = 2000;        // a slot counts as heard this long after its last direct packet
constexpr uint32_t kMeshReportGapMs = 1000;   // at most one MeshHeard per second

// Client side: when each other slot last arrived directly, and when the mask last changed.
class MeshHearing {
public:
    void Note(uint8_t slot, uint32_t nowMs) { if (slot < kMaxFighters) { at_[slot] = nowMs; seen_[slot] = true; } }
    bool Heard(uint8_t slot, uint32_t nowMs) const { return slot < kMaxFighters && seen_[slot] && nowMs - at_[slot] <= kMeshHoldMs; }
    uint16_t Mask(uint32_t nowMs) const {
        uint16_t m = 0;
        for (uint8_t s = 0; s < kMaxFighters; ++s) if (Heard(s, nowMs)) m |= uint16_t(1u << s);
        return m;
    }
    // True when the mask differs from the last one sent and the report gap has passed; *mask is then what to send.
    bool TakeChange(uint32_t nowMs, uint16_t* mask) {
        const uint16_t m = Mask(nowMs);
        if (!resend_ && (m == lastSent_ || (sentAny_ && nowMs - lastSentAt_ < kMeshReportGapMs))) return false;
        lastSent_ = m; lastSentAt_ = nowMs; sentAny_ = true; resend_ = false; *mask = m;
        return true;
    }
    // The host's table for this client is new (a Welcome, a PeerMap): the next TakeChange reports the mask again.
    void Invalidate() { sentAny_ = false; resend_ = true; }
    void Clear() { *this = MeshHearing{}; }
private:
    uint32_t at_[kMaxFighters] = {};
    bool seen_[kMaxFighters] = {};
    uint16_t lastSent_ = 0;
    uint32_t lastSentAt_ = 0;
    bool sentAny_ = false;
    bool resend_ = false;   // Invalidate: report even an unchanged mask (every bit is a valid mask; no sentinel)
};

// Host side: per slot, the slots that client says it hears directly.
class MeshMasks {
public:
    void Set(uint8_t slot, uint16_t mask) {
        if (slot >= kMaxFighters) return;
        m_[slot] = uint16_t(mask & ((1u << kMaxFighters) - 1) & ~(1u << slot));
    }
    // A slot joined or left: its own report is void, and nobody hears it any more.
    void Reset(uint8_t slot) {
        if (slot >= kMaxFighters) return;
        m_[slot] = 0;
        for (uint16_t& x : m_) x = uint16_t(x & ~(1u << slot));
    }
    void Clear() { for (uint16_t& x : m_) x = 0; }
    bool Hears(uint8_t listener, uint8_t from) const { return listener < kMaxFighters && from < kMaxFighters && (m_[listener] >> from & 1); }
private:
    uint16_t m_[kMaxFighters] = {};
};

// RelayTargets for a Pose or Vitals: skips the slots that already hear fromSlot directly.
inline std::vector<uint8_t> RelayTargets(uint8_t fromSlot, const std::vector<uint8_t>& connectedSlots, const MeshMasks& masks) {
    std::vector<uint8_t> out;
    for (uint8_t s : RelayTargets(fromSlot, connectedSlots))
        if (!masks.Hears(s, fromSlot)) out.push_back(s);
    return out;
}

// Host side: the PeerMap for the clients. all = every joined client's (slot, SteamID); entries that cannot be a mesh peer
// (the host slot, a slot past the roster, no SteamID, a slot named twice) are left out, the rest go out in slot order.
inline PeerMapMsg MakePeerMap(const std::vector<PeerMapEntry>& all) {
    PeerMapMsg m;
    for (const PeerMapEntry& e : all) {
        if (e.slot == kHostSlot || e.slot >= kMaxPeerMap || e.steamId == 0 || m.peers.size() >= kMaxPeerMap) continue;
        const bool dup = std::any_of(m.peers.begin(), m.peers.end(), [&](const PeerMapEntry& x) { return x.slot == e.slot || x.steamId == e.steamId; });
        if (!dup) m.peers.push_back(e);
    }
    std::sort(m.peers.begin(), m.peers.end(), [](const PeerMapEntry& a, const PeerMapEntry& b) { return a.slot < b.slot; });
    return m;
}

// Client side: the other clients this machine exchanges Pose and Vitals with, as the host's last PeerMap named them, and
// which transport peer each one is. The host is never in it (it has its own peer), and neither is this machine.
class MeshPeers {
public:
    struct Change {
        std::vector<uint64_t> want;   // everyone the map names for this machine: allow each one (allowing twice is harmless)
        std::vector<uint64_t> drop;   // named before, not now: close their sessions
    };
    Change Apply(const PeerMapMsg& m, uint8_t mySlot, uint64_t mySteam, uint64_t hostSteam) {
        Change c;
        std::vector<PeerMapEntry> next;
        for (const PeerMapEntry& e : m.peers) {
            if (e.slot == mySlot || e.slot == kHostSlot || e.slot >= kMaxPeerMap || !e.steamId || e.steamId == mySteam || e.steamId == hostSteam) continue;
            if (std::any_of(next.begin(), next.end(), [&](const PeerMapEntry& x) { return x.slot == e.slot || x.steamId == e.steamId; })) continue;
            next.push_back(e); c.want.push_back(e.steamId);
        }
        for (const PeerMapEntry& o : map_)
            if (std::none_of(next.begin(), next.end(), [&](const PeerMapEntry& x) { return x.steamId == o.steamId; })) c.drop.push_back(o.steamId);
        map_ = std::move(next);
        std::erase_if(bound_, [&](const Link& b) { return !Has(b.steam); });
        return c;
    }
    // The transport's peer id for a SteamID Apply named (after AllowPeer).
    void Bind(uint64_t steamId, uint32_t peer) {
        if (!peer || !Has(steamId)) return;
        for (Link& b : bound_) if (b.steam == steamId) { b.peer = peer; return; }
        bound_.push_back({steamId, peer});
    }
    // The transport closed this peer (its Disconnected event): it names nobody any more, but the map still lists the member.
    void ForgetPeer(uint32_t peer) { std::erase_if(bound_, [&](const Link& b) { return b.peer == peer; }); }
    // A lobby member left: the transport closed that session, so a later map has to open it again.
    void Forget(uint64_t steamId) { std::erase_if(bound_, [&](const Link& b) { return b.steam == steamId; }); }
    // The transport peer of the mesh peer holding slot; 0 when that slot is not a bound mesh peer.
    uint32_t PeerOfSlot(uint8_t slot) const {
        for (const PeerMapEntry& e : map_)
            if (e.slot == slot)
                for (const Link& b : bound_) if (b.steam == e.steamId) return b.peer;
        return 0;
    }
    // The slot of a player the map names, by SteamID; 0xFF for anyone else, the host and this machine included.
    uint8_t SlotOfSteam(uint64_t steamId) const {
        for (const PeerMapEntry& e : map_) if (e.steamId == steamId) return e.slot;
        return 0xFF;
    }
    // The slot a named mesh peer holds; 0xFF for any other peer, the host included.
    uint8_t SlotOfPeer(uint32_t peer) const {
        for (const Link& b : bound_)
            if (b.peer == peer)
                for (const PeerMapEntry& e : map_) if (e.steamId == b.steam) return e.slot;
        return 0xFF;
    }
    bool Has(uint64_t steamId) const { return std::any_of(map_.begin(), map_.end(), [&](const PeerMapEntry& x) { return x.steamId == steamId; }); }
    bool IsBound(uint64_t steamId) const { return std::any_of(bound_.begin(), bound_.end(), [&](const Link& b) { return b.steam == steamId; }); }
    size_t Named() const { return map_.size(); }
    // The slots of every bound mesh peer, in map order (Mesh v2 direct targets, per-player pings).
    std::vector<uint8_t> Slots() const {
        std::vector<uint8_t> out;
        for (const PeerMapEntry& e : map_)
            if (std::any_of(bound_.begin(), bound_.end(), [&](const Link& b) { return b.steam == e.steamId; })) out.push_back(e.slot);
        return out;
    }
    std::vector<uint32_t> Peers() const {
        std::vector<uint32_t> out;
        for (const Link& b : bound_) out.push_back(b.peer);
        return out;
    }
    void Clear() { map_.clear(); bound_.clear(); }
private:
    struct Link { uint64_t steam; uint32_t peer; };
    std::vector<PeerMapEntry> map_;
    std::vector<Link> bound_;
};

// ---------------------------------------------------------------- Mesh v2 (rev 22)

// The messages a client also sends straight to the other clients when the mesh is on (besides Pose and Vitals).
inline bool MeshDirectType(MsgType t) {
    switch (t) {
        case MsgType::HitClaim: case MsgType::HitFx: case MsgType::SeverClaim: case MsgType::SeverNotice:
        case MsgType::StuckBlade: case MsgType::PaintFx: case MsgType::Equip: case MsgType::GrabState: case MsgType::BodyState:
        case MsgType::ObjectEvent:
            return true;
        default: return false;
    }
}

// What a direct message claims to speak for, as the receiver decoded it.
struct MeshMsgFacts {
    uint8_t actor = 0;          // the entity it speaks for: the attacker of a blow, sever or stuck blade; the painted,
                                // equipped, grabbing or notified fighter; for a BodyState, the lessee's fighter entity; for an ObjectEvent, the sender's own
    uint8_t victim = 0;         // HitClaim, HitFx, SeverClaim, StuckBlade: the fighter struck; else 0
    bool stripRequest = false;  // a PaintFx strip request (for another machine's fighter): those go through the host
};

// The host's sender checks, made by the receiver of a message that came straight from mesh peer fromSlot (the host makes
// them for every relayed copy): the sender speaks only for its own fighter, a blow has a real victim that is not the
// attacker, a claim the victim's owner replays (HitClaim, StuckBlade) only reaches that owner, and a HitFx (the cosmetic
// copy of a blow) only reaches a machine that does not own the victim. myEntity: this machine's fighter.
inline bool MeshSenderOk(MsgType t, uint8_t fromSlot, const MeshMsgFacts& f, uint8_t myEntity, const Holders& h = Holders()) {
    const bool stream = t == MsgType::Pose || t == MsgType::Vitals;
    if ((!stream && !MeshDirectType(t)) || fromSlot == kHostSlot || fromSlot >= kMaxFighters) return false;
    if (!MayStream(fromSlot, f.actor, h) || f.stripRequest) return false;
    const bool blow = t == MsgType::HitClaim || t == MsgType::HitFx || t == MsgType::SeverClaim || t == MsgType::StuckBlade;
    if (!blow) return true;
    if (!ValidEntity(f.victim) || f.victim == f.actor) return false;
    // The victim is mine to replay: my own fighter, or one held here (a hand-off).
    const bool victimMine = ValidEntity(myEntity) && myEntity <= kMaxFighters && OwnerSlotOf(f.victim, h) == uint8_t(myEntity - 1);
    if (t == MsgType::HitClaim || t == MsgType::StuckBlade) return victimMine;
    if (t == MsgType::HitFx) return !victimMine;
    return true;
}

// Which mesh peers get a direct copy of a message this client sends: a HitClaim or StuckBlade goes to the victim's owner
// alone (nobody else replays it), a HitFx to every peer but the victim's owner, the rest to every peer. Slots, in
// peerSlots order; the host is never one (it gets its own copy).
inline std::vector<uint8_t> MeshDirectSlots(MsgType t, uint8_t victim, const std::vector<uint8_t>& peerSlots, const Holders& h = Holders()) {
    std::vector<uint8_t> out;
    const uint8_t victimSlot = ValidEntity(victim) ? OwnerSlotOf(victim, h) : uint8_t(0xFF);
    for (uint8_t s : peerSlots) {
        if (s == kHostSlot || s >= kMaxFighters) continue;
        if (t == MsgType::HitClaim || t == MsgType::StuckBlade) { if (s == victimSlot) out.push_back(s); }
        else if (t == MsgType::HitFx) { if (s != victimSlot) out.push_back(s); }
        else if (MeshDirectType(t)) out.push_back(s);
    }
    return out;
}

// First copy wins: per (stream, entity) a high-water id and a bitmap of the kMeshDedupeWindow ids below it, as
// combat::ClaimDedupe. A stream is a message kind with its own numbering (MeshStream); 16-bit ids (paint, equip, stuck
// blades) wrap at 65536, 32-bit ones (claim ids) at 2^32.
constexpr size_t kMeshDedupeWindow = 512;
enum class MeshStream : uint8_t { HitClaim, HitFx, SeverClaim, SeverNotice, StuckAttach, StuckRelease, PaintFx, Equip };
inline bool MeshStreamWide(MeshStream s) {
    return s == MeshStream::HitClaim || s == MeshStream::HitFx || s == MeshStream::SeverClaim || s == MeshStream::SeverNotice;
}

class MeshDedupe {
public:
    // True iff (stream, entity, id) was not seen before. An id too far below the newest to tell is refused.
    bool Accept(MeshStream stream, uint8_t entity, uint32_t id) {
        const bool wide = MeshStreamWide(stream);
        if (!wide) id &= 0xFFFF;
        State& s = state_[{uint8_t(stream), entity}];
        if (!s.have) { s.have = true; s.highest = id; s.bits.reset(); s.bits.set(0); return true; }
        if (id == s.highest) return false;
        const int32_t delta = wide ? int32_t(id - s.highest) : int32_t(int16_t(uint16_t(id - s.highest)));
        if (delta > 0) {
            const size_t shift = size_t(delta);
            if (shift >= kMeshDedupeWindow) s.bits.reset(); else s.bits <<= shift;
            s.highest = id; s.bits.set(0);
            return true;
        }
        const size_t back = size_t(-int64_t(delta));
        if (back >= kMeshDedupeWindow || s.bits.test(back)) return false;
        s.bits.set(back);
        return true;
    }
    // The machine behind entity left or rejoined: its numbering may start again from 1.
    void Forget(uint8_t entity) {
        for (auto it = state_.begin(); it != state_.end();) it = it->first.second == entity ? state_.erase(it) : std::next(it);
    }
    void Clear() { state_.clear(); }
    size_t Tracked() const { return state_.size(); }

private:
    struct State { bool have = false; uint32_t highest = 0; std::bitset<kMeshDedupeWindow> bits; };
    std::map<std::pair<uint8_t, uint8_t>, State> state_;   // at most 8 streams x kMaxWillies entities
};

}  // namespace hsm
