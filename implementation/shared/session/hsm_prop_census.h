// Level physics props in owner sync: the host's census (MsgType::PropCensus, host -> clients, reliable).
// A level's physics props (StaticMeshActors whose mesh simulates; Blueprint props: barrels, stools, traps) have no
// identity two machines share: when 'Purge Map' is set the level manager destroys every actor tagged 'Random Delete'
// on a per-machine RandomBool, and it re-spawns the ones tagged 'Respawn' as new actors (BP_LevelManager
// LatentResume_L3a2 / LatentResume_L46). So the host names them. It walks its level and gives every physics prop an
// index (CensusBook: an actor keeps its index for as long as it lives, a new one gets the next, none is reused within a
// level), and sends the list -- index, class and home, where the host first saw the prop -- to every client, in parts
// (PropCensusMsg, SplitCensus) that a client puts back together (CensusAssembler). The client then finds its own actor
// for each index: the nearest of its own physics props of the same class within kCensusMatchUu of the home, each used
// once, nearest pairs first (MatchCensus), and each prop is placed where it lay early in the level when the client
// recorded that (CensusMatchPlace). From then on that prop is MakeLevelPropId(index) in the lease table
// (session/hsm_lease.h).
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_lease.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace hsm {

constexpr size_t kCensusEntriesPerPart = 64;                                   // 18 B each: a part is 1161 B
constexpr uint8_t kMaxCensusParts = 32;
constexpr size_t kMaxCensusProps = kCensusEntriesPerPart * kMaxCensusParts;     // 2048, far above any arena's
constexpr float kCensusMatchUu = 100.0f;      // cm; a client's prop of the same class this close to the home is it

// Census sequence numbers wrap: a is newer than b when it is less than half the space ahead.
inline bool CensusSeqNewer(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)) > 0; }

struct CensusEntry {
    uint16_t index = 0;           // MakeLevelPropId(index)
    uint32_t classHash = 0;       // sync::WeaponClassHash of the actor's class name; never 0
    float px = 0, py = 0, pz = 0; // home: where the host first saw the prop, cm
};
// A place a census can carry: finite and inside kMaxPropCoord on every axis (what BodyState accepts). NaN compares
// false and an infinity is out of range, so the bound alone refuses both.
inline bool CensusPlaceOk(float x, float y, float z) {
    return std::fabs(x) <= kMaxPropCoord && std::fabs(y) <= kMaxPropCoord && std::fabs(z) <= kMaxPropCoord;
}

// ---------------------------------------------------------------- PropCensus on the wire
struct PropCensusMsg {
    uint16_t seq = 0;                       // the census this part belongs to; a newer one replaces the whole list
    uint8_t part = 0, parts = 1;            // part < parts <= kMaxCensusParts
    std::vector<CensusEntry> entries;       // at most kCensusEntriesPerPart, no index twice
};

inline std::vector<uint8_t> Encode(const PropCensusMsg& m) {
    Writer w; w.header(MsgType::PropCensus);
    w.u16(m.seq); w.u8(m.part); w.u8(m.parts); w.u8(uint8_t(m.entries.size()));
    for (const CensusEntry& e : m.entries) { w.u16(e.index); w.u32(e.classHash); w.f32(e.px); w.f32(e.py); w.f32(e.pz); }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, PropCensusMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::PropCensus) return false;
    m.seq = r.u16(); m.part = r.u8(); m.parts = r.u8();
    const size_t count = r.u8();
    if (!r.ok() || m.part >= m.parts || m.parts > kMaxCensusParts || count > kCensusEntriesPerPart) return false;   // part < parts: never 0 parts
    m.entries.clear();
    std::set<uint16_t> seen;
    for (size_t i = 0; i < count; ++i) {
        CensusEntry e;
        e.index = r.u16(); e.classHash = r.u32(); e.px = r.f32(); e.py = r.f32(); e.pz = r.f32();
        if (!r.ok() || e.classHash == 0 || !CensusPlaceOk(e.px, e.py, e.pz) || !seen.insert(e.index).second) return false;
        m.entries.push_back(e);
    }
    return r.done();
}

// The whole census as parts (an empty census is one empty part: a client must hear that too). Nothing when it holds
// more than kMaxCensusProps entries.
inline std::vector<PropCensusMsg> SplitCensus(uint16_t seq, const std::vector<CensusEntry>& all) {
    if (all.size() > kMaxCensusProps) return {};
    const size_t parts = all.empty() ? 1 : (all.size() + kCensusEntriesPerPart - 1) / kCensusEntriesPerPart;
    std::vector<PropCensusMsg> out(parts);
    for (size_t p = 0; p < parts; ++p) {
        out[p].seq = seq; out[p].part = uint8_t(p); out[p].parts = uint8_t(parts);
        const size_t from = p * kCensusEntriesPerPart, to = std::min(all.size(), from + kCensusEntriesPerPart);
        out[p].entries.assign(all.begin() + long(from), all.begin() + long(to));
    }
    return out;
}

// ---------------------------------------------------------------- a client puts the parts back together
class CensusAssembler {
public:
    enum class Result : uint8_t { Pending, Complete, Stale, Rejected };
    // Complete once every part of the newest census is in; Census() is then that census, sorted by index. A part of a
    // newer census drops the one being assembled; a part of an older one, or of the census already complete, is Stale.
    // Rejected: a part that contradicts its census (another part count, a part twice) -- the census being assembled
    // stays -- or, on completion, an index in two parts, which drops that census.
    Result Add(const PropCensusMsg& m) {
        if (m.part >= m.parts || m.parts > kMaxCensusParts || m.entries.size() > kCensusEntriesPerPart) return Result::Rejected;
        if (have_ && !CensusSeqNewer(m.seq, seq_)) return Result::Stale;
        if (pending_ && m.seq != pendingSeq_) {
            if (!CensusSeqNewer(m.seq, pendingSeq_)) return Result::Stale;
            pending_ = false; got_.clear();                        // a newer census: the half-built one goes
        }
        if (!pending_) { pending_ = true; pendingSeq_ = m.seq; pendingParts_ = m.parts; }
        else if (m.parts != pendingParts_) return Result::Rejected;
        if (got_.count(m.part)) return Result::Rejected;
        got_[m.part] = m.entries;
        if (got_.size() < pendingParts_) return Result::Pending;
        std::vector<CensusEntry> all;
        for (const auto& [part, entries] : got_) all.insert(all.end(), entries.begin(), entries.end());
        pending_ = false; got_.clear();
        std::sort(all.begin(), all.end(), [](const CensusEntry& a, const CensusEntry& b) { return a.index < b.index; });
        for (size_t i = 1; i < all.size(); ++i)
            if (all[i].index == all[i - 1].index) return Result::Rejected;
        census_ = std::move(all); seq_ = pendingSeq_; have_ = true;
        return Result::Complete;
    }
    bool HaveCensus() const { return have_; }
    uint16_t Seq() const { return seq_; }                          // of the last complete census
    const std::vector<CensusEntry>& Census() const { return census_; }
    void Clear() { *this = CensusAssembler{}; }
private:
    bool have_ = false;
    uint16_t seq_ = 0;
    std::vector<CensusEntry> census_;
    bool pending_ = false;
    uint16_t pendingSeq_ = 0;
    uint8_t pendingParts_ = 0;
    std::map<uint8_t, std::vector<CensusEntry>> got_;
};

// ---------------------------------------------------------------- a client finds its own actor for each index
struct CensusCandidate { uint32_t classHash = 0; float px = 0, py = 0, pz = 0; };   // one of the client's physics props
// For each census entry, the candidate that is that prop (its position in locals), or -1: same class, within tolUu of
// the home, each candidate used once, the nearest pairs of the whole census first (ties: lower entry, then lower
// candidate), so one entry's near miss never takes the candidate another entry sits on.
inline std::vector<int> MatchCensus(const std::vector<CensusEntry>& census, const std::vector<CensusCandidate>& locals,
                                    float tolUu = kCensusMatchUu) {
    std::map<uint32_t, std::vector<size_t>> byClass;
    for (size_t j = 0; j < locals.size(); ++j) byClass[locals[j].classHash].push_back(j);
    std::vector<std::tuple<float, size_t, size_t>> pairs;           // distance squared, entry, candidate
    const float reach2 = tolUu * tolUu;
    for (size_t i = 0; i < census.size(); ++i) {
        const auto it = byClass.find(census[i].classHash);
        if (it == byClass.end()) continue;
        for (size_t j : it->second) {
            const float dx = locals[j].px - census[i].px, dy = locals[j].py - census[i].py, dz = locals[j].pz - census[i].pz;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 <= reach2) pairs.emplace_back(d2, i, j);
        }
    }
    std::sort(pairs.begin(), pairs.end());
    std::vector<int> out(census.size(), -1);
    std::vector<bool> used(locals.size(), false);
    for (const auto& [d2, i, j] : pairs) {
        if (out[i] != -1 || used[j]) continue;
        out[i] = int(j); used[j] = true;
    }
    return out;
}

// The place MatchCensus compares for one of the client's props: its snapshot place when it has one of the same class (where
// it lay early in the level, before a roll or a carry moved it), else where it is now. nullptr: no snapshot.
inline CensusCandidate CensusMatchPlace(const CensusCandidate& now, const CensusCandidate* snap) {
    if (snap && snap->classHash == now.classHash) return CensusCandidate{now.classHash, snap->px, snap->py, snap->pz};
    return now;
}

// ---------------------------------------------------------------- the host names its props
template <class Key>
class CensusBook {
public:
    struct Seen { Key key; uint32_t classHash = 0; float px = 0, py = 0, pz = 0; };
    // One walk: every physics prop present now. A known key keeps its index and home; a new one gets the next index
    // and its place now as home; a known key missing from the walk is gone, and its index is never handed out again
    // (until Clear, at a level change). Left out, counted in LeftOut(): a key with class 0 or a place no census can
    // carry, a key past kMaxCensusProps live props or past index 65535. A key twice in one walk counts once.
    // True when the set of props changed.
    bool Update(const std::vector<Seen>& walk) {
        bool changed = false;
        leftOut_ = 0;
        std::set<Key> present;
        for (const Seen& s : walk) present.insert(s.key);
        for (auto it = live_.begin(); it != live_.end();) {
            if (present.count(it->first)) { ++it; continue; }
            it = live_.erase(it); changed = true;
        }
        std::set<Key> done;
        for (const Seen& s : walk) {
            if (!done.insert(s.key).second || live_.count(s.key)) continue;
            if (s.classHash == 0 || !CensusPlaceOk(s.px, s.py, s.pz) || live_.size() >= kMaxCensusProps || next_ > 0xFFFF) {
                ++leftOut_; continue;
            }
            CensusEntry e; e.index = uint16_t(next_++); e.classHash = s.classHash; e.px = s.px; e.py = s.py; e.pz = s.pz;
            live_[s.key] = e; changed = true;
        }
        return changed;
    }
    std::vector<CensusEntry> Entries() const {                     // sorted by index
        std::vector<CensusEntry> out;
        for (const auto& [key, e] : live_) out.push_back(e);
        std::sort(out.begin(), out.end(), [](const CensusEntry& a, const CensusEntry& b) { return a.index < b.index; });
        return out;
    }
    bool Find(const Key& key, uint16_t& index) const {
        const auto it = live_.find(key);
        if (it == live_.end()) return false;
        index = it->second.index; return true;
    }
    size_t Size() const { return live_.size(); }
    size_t LeftOut() const { return leftOut_; }                    // in the last Update
    void Clear() { live_.clear(); next_ = 0; leftOut_ = 0; }
private:
    std::map<Key, CensusEntry> live_;
    uint32_t next_ = 0;
    size_t leftOut_ = 0;
};

}  // namespace hsm
