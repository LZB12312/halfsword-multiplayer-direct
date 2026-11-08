#include "gameplay/object_sync.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

// Spawned objects and object events (plan M0; session/hsm_object.h, hsm_object_event.h). The members of the object class that
// name what appears mid-match and replay what happens to it. The shape:
//   The host walks (HostCensus, every kCensusRefreshMs and sooner on NoteSpawn) and gives an object nobody names a canonical
//   id (ObjectReconciler::SpawnLocal) and announces it. A client walks the same way (Recheck) and PROPOSES what it finds
//   moving that nothing names, under a provisional id it mints; nothing streams or claims under that id. The host answers
//   with the canonical announce, which re-keys the proposer's entry (Rekey) and lets every other machine bind its own actor
//   by class and place (BindAnnounced), or keep a ghost until it can.
//   Whoever streams an object checks its joints, its detached bodies and its parent every kEventPollMs (PollEvents) and sends
//   an ObjectEvent; every machine replays it by index (ReplayNow), idempotently, queuing an event for an object not found yet.

namespace hsm::game {
using namespace hsm::ue;
using namespace hsm::game::hands;

namespace {
bool PlaceOk(const FVector& at) {
    return CensusPlaceOk(float(at.x), float(at.y), float(at.z));
}
}  // namespace

void ObjectSync::NoteSpawn() {
    if (!active_) return;
    const double now = SteadyNowMs();
    if (now - hintMs_ < kHintGapMs) return;
    hintMs_ = now;
    if (env_.host) { if (haveSent_) censusMs_ = std::min(censusMs_, now + kHintGapMs); }
    else if (matched_) recheckMs_ = std::min(recheckMs_, now + kHintGapMs);
}

uint64_t ObjectSync::MintSpawned() {
    if (minter_.Slot() != MySlot()) minter_ = SpawnedIdMinter(MySlot());
    return minter_.Mint();
}

bool ObjectSync::CompMoving(UObject* comp) {
    RootCalls* c = comp && !IsGarbage(comp) ? CallsFor(comp) : nullptr;
    if (!c) return false;
    c->linVel.put(0, FName{});
    if (!c->linVel.invoke(comp)) return false;
    const FVector v = c->linVel.take<FVector>(1);
    return v.x * v.x + v.y * v.y + v.z * v.z > double(kRestSpeed) * double(kRestSpeed);
}

// ---------------------------------------------------------------- the re-check

// Client: the same pass the host's census makes, for what is not named. Runs on the census cadence, brought forward by
// NoteSpawn and by a canonical announce with no actor here yet.
void ObjectSync::Recheck(double nowMs) {
    recheckMs_ = nowMs + kCensusRefreshMs;
    ++n_.rechecks;
    size_t notSimulating = 0;
    std::vector<LevelActor> free;
    for (LevelActor& a : WalkLevel(notSimulating))
        if (!byActor_.count(a.actor)) free.push_back(a);
    AfterWalk(free, nowMs);
}

// Client: free are the physics objects here that nothing names yet. Pending announces bind to them first; what is left and
// is on the move, touched by a fighter simulated here, or hinted (armour off, a spawn this machine caused) is proposed.
void ObjectSync::AfterWalk(std::vector<LevelActor>& free, double nowMs) {
    if (env_.host) return;
    BindAnnounced(free, nowMs);
    for (auto it = refused_.begin(); it != refused_.end();) it = it->second <= nowMs ? refused_.erase(it) : std::next(it);
    for (auto it = touched_.begin(); it != touched_.end();) it = (it->second < 0 ? touched_.size() > 128 : nowMs - it->second > 60000.0) ? touched_.erase(it) : std::next(it);
    const bool hinted = nowMs - hintMs_ < 2000.0;
    std::set<const UObject*> left;
    size_t proposals = 0;
    for (LevelActor& a : free) {
        if (byActor_.count(a.actor)) continue;
        left.insert(a.actor);
        if (proposals >= kProposalsPerPass) continue;
        if (auto rf = refused_.find(a.actor); rf != refused_.end() && nowMs < rf->second) continue;
        const auto t = touched_.find(a.actor);
        const bool touchedNow = t != touched_.end() && t->second > 0 && nowMs - t->second < 15000.0;
        if (!touchedNow && !((hinted || leftBefore_.count(a.actor)) && CompMoving(a.simComp))) continue;
        ProposeLocal(a, nowMs);
        ++proposals;
    }
    leftBefore_ = std::move(left);
}

void ObjectSync::ProposeLocal(const LevelActor& a, double nowMs) {
    if (!a.classHash || !PlaceOk(a.at)) return;
    const uint64_t prov = MintSpawned();
    if (!prov) return;
    Prop* p = Track(prov, a.actor);
    if (!p) { refused_[a.actor] = nowMs + kRefusedKeepMs; return; }
    p->provisional = true;
    p->spawned = true;
    p->provisionalMs = nowMs;
    if (auto t = touched_.find(a.actor); t != touched_.end() && t->second > 0) {
        p->lastContactMs = t->second;
        p->toucher = uint8_t(MySlot() + 1);   // this machine's fighter: the roster slot's entity
    }
    ObjectAnnounceMsg m;
    m.objId = prov;
    m.flags = kAnnounceProposal;
    m.classHash = a.classHash;
    m.px = float(a.at.x);
    m.py = float(a.at.y);
    m.pz = float(a.at.z);
    m.bodyCount = uint8_t(std::clamp<size_t>(p->bodies.size(), 1, kMaxAnnounceBodies));
    if (env_.send) env_.send(Encode(m), true);
    ++n_.proposed;
    if (n_.proposed <= kLogFirst)
        HSM_LOG("props: proposed %s as %016llx (%u bodies)", ObjectName(a.actor).c_str(), static_cast<unsigned long long>(prov), unsigned(m.bodyCount));
}

// Host: an object nobody names yet is new. It gets a canonical id in the host's slot and everyone hears of it.
void ObjectSync::HostSpawn(const LevelActor& a, double nowMs) {
    if (!a.classHash || !PlaceOk(a.at)) return;
    const uint64_t id = reconciler_.SpawnLocal(a.classHash, float(a.at.x), float(a.at.y), float(a.at.z), std::max<uint8_t>(1, a.bodies));
    if (!id) return;
    Prop* p = Track(id, a.actor);
    if (!p) { reconciler_.Remove(id); return; }
    p->spawned = true;
    MarkFound(id, *p, 0, 0);   // a level actor the walk found after the census: a structure if it has the shape of one
    if (auto t = touched_.find(a.actor); t != touched_.end() && t->second > 0 && !OwnCopy(id, *p)) {
        p->lastContactMs = t->second;
        p->toucher = uint8_t(kHostSlot + 1);
    }
    ObjectAnnounceMsg m;
    if (reconciler_.AnnounceOf(id, m) && env_.send) env_.send(Encode(m), true);
    ++n_.announced;
    if (n_.announced <= kLogFirst)
        HSM_LOG("props: announced %s as %016llx (%zu bodies)", ObjectName(a.actor).c_str(), static_cast<unsigned long long>(id), p->bodies.size());
    (void)nowMs;
}

// ---------------------------------------------------------------- announces

void ObjectSync::OnObjectAnnounce(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return;
    ObjectAnnounceMsg m;
    if (!Decode(d, n, m)) { ++n_.announceRejected; return; }
    const double now = SteadyNowMs();
    if (env_.host) {
        if (m.flags != kAnnounceProposal || fromSlot == kHostSlot || fromSlot >= kMaxFighters) { ++n_.announceRejected; return; }
        HostPropose(m, fromSlot, now);
    } else {
        if (m.flags != kAnnounceCanonical || fromSlot != kHostSlot) { ++n_.announceRejected; return; }
        ClientAnnounce(m, now);
    }
}

// Host: a proposal. An object the host already tracks here (a census prop, or a spawned object) of that class near that
// place is the same object and answers it; otherwise the reconciler names it, and the host looks for its own actor.
void ObjectSync::HostPropose(const ObjectAnnounceMsg& m, uint8_t fromSlot, double nowMs) {
    if (!ValidSpawnedId(m.objId) || SpawnedIdSlot(m.objId) != fromSlot) { ++n_.announceRejected; return; }
    uint64_t existing = 0;
    double best = -1.0;
    // The proposal was made from the proposer's view some network trips ago; a moving object has travelled since. Its match
    // tolerance grows with its speed (ProposalTolUu, capped) so one object proposed by two clients, or by a client after
    // the host announced it, stays one object. The nearest in reach wins (ties: the lower id, the map's order).
    const double reach = double(kCensusMatchUu) + double(kMotionTolCapUu);
    for (auto& [id, p] : props_) {
        if (IsWeaponId(id) || p.provisional || p.sub || p.classHash != m.classHash || !adapter_->StillValid(p.actor, p.index)) continue;
        const FVector at = adapter_->ActorLocation(p.actor);
        const double dx = at.x - m.px, dy = at.y - m.py, dz = at.z - m.pz;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > reach * reach) continue;                         // too far whatever its speed: no need to read it
        float speed = 0.f;
        if (d2 > double(kCensusMatchUu) * double(kCensusMatchUu) && !p.bodies.empty()) {   // only a candidate that depends on its speed is read
            BodyStateMsg s;
            if (Read(p, 0, s)) speed = SpeedOf(s.vx, s.vy, s.vz);
        }
        const double tol = double(ProposalTolUu(kCensusMatchUu, speed));
        if (d2 <= tol * tol && (best < 0.0 || d2 < best)) { best = d2; existing = id; }
    }
    ObjectAnnounceMsg out;
    if (existing) {
        out = m;
        out.objId = existing;
        out.flags = kAnnounceCanonical;
        out.proposalId = m.objId;
        const FVector at = adapter_->ActorLocation(props_[existing].actor);
        out.px = float(at.x);
        out.py = float(at.y);
        out.pz = float(at.z);
    } else {
        // Only a new object counts against the player's limit: an answer to one already known costs nothing.
        const SpawnGate::Verdict gate = reconciler_.WouldCreate(m, nowMs) ? spawnGate_.Check(fromSlot, nowMs) : SpawnGate::Verdict::Allowed;
        if (gate != SpawnGate::Verdict::Allowed) {
            ++n_.announceRejected;
            if (++spawnRefused_ <= 5 || spawnRefused_ % 1000 == 0)
                HSM_LOG("props: refused a spawn from slot %u: %s (%llu refused so far)", fromSlot, gate == SpawnGate::Verdict::TooMany ? "too many of theirs are live" : "too fast", static_cast<unsigned long long>(spawnRefused_));
            return;
        }
        const ObjectReconciler::Result r = reconciler_.Propose(m, fromSlot, nowMs);   // the object's last velocity carries its place to now
        if (!r.objId) { ++n_.announceRejected; return; }
        out = r.announce;
        if (r.created) {   // the host's own actor for it, if it has one; else a ghost until it does
            spawnGate_.OnCreated(r.objId, fromSlot);
            announced_[r.objId] = Announced{out, nowMs};
            size_t ns = 0;
            std::vector<LevelActor> free;
            for (LevelActor& a : WalkLevel(ns))
                if (!byActor_.count(a.actor)) free.push_back(a);
            BindAnnounced(free, nowMs);
            if (const auto q = props_.find(r.objId); q != props_.end())   // bound to the host's own actor: everyone compares with that
                out.bodyCount = uint8_t(std::clamp<size_t>(q->second.bodies.size(), 1, kMaxAnnounceBodies));
        }
    }
    if (env_.send) env_.send(Encode(out), true);
    ++n_.announced;
}

// Client: the host's canonical announce. It answers one of my proposals (re-key), or names an object I bind to my own actor
// of that class and place, now or on a later re-check.
void ObjectSync::ClientAnnounce(const ObjectAnnounceMsg& m, double nowMs) {
    minter_.Observe(m.objId);
    minter_.Observe(m.proposalId);
    if (destroyed_.count(m.objId)) return;
    if (m.proposalId && Rekey(m.proposalId, m.objId, m.bodyCount)) { Bound(m.objId, nowMs); return; }
    if (props_.count(m.objId) || announced_.count(m.objId)) return;
    if (announced_.size() >= kMaxKnownObjects) { ++n_.announceRejected; return; }
    announced_[m.objId] = Announced{m, nowMs};
    ++n_.announced;
    if (matched_) recheckMs_ = std::min(recheckMs_, nowMs + kHintGapMs);   // look for the actor now
}

// My provisional entry becomes the canonical object. If the canonical id is already bound here to another actor the
// provisional entry is dropped (and its actor is not proposed again for a while).
bool ObjectSync::Rekey(uint64_t provisional, uint64_t canonical, uint8_t hostBodies) {
    auto it = props_.find(provisional);
    if (it == props_.end() || !it->second.provisional) return false;
    if (props_.count(canonical)) {
        refused_[it->second.actor] = SteadyNowMs() + kRefusedKeepMs;
        if (!rekeyRefusedLogged_) {   // the host's answer is a copy already bound here: this proposal's copy is dropped
            rekeyRefusedLogged_ = true;
            HSM_LOG("props: proposal %016llx (%s) is already here as %016llx; the copy is refused for %.0f s (logged once)",
                    static_cast<unsigned long long>(provisional), ObjectName(it->second.actor).c_str(),
                    static_cast<unsigned long long>(canonical), kRefusedKeepMs / 1000.0);
        }
        byActor_.erase(it->second.actor);
        props_.erase(it);
        return true;
    }
    auto node = props_.extract(it);
    node.key() = canonical;
    Prop& p = props_.insert(std::move(node)).position->second;
    p.provisional = false;
    p.spawned = IsSpawnedId(canonical);
    p.level = IsLevelProp(canonical);
    // An actor of this machine's walk, proposed: as the host's late finds, and checked against the host's copy as they are.
    if (p.spawned) MarkFound(canonical, p, hostBodies, std::clamp<size_t>(p.bodies.size(), 1, kMaxAnnounceBodies));
    p.claim = LeaseClaim{};
    p.lastSentMs = -1e18;
    byActor_[p.actor] = canonical;
    ++n_.rekeyed;
    if (n_.rekeyed <= kLogFirst)
        HSM_LOG("props: %016llx is now %016llx", static_cast<unsigned long long>(provisional), static_cast<unsigned long long>(canonical));
    return true;
}

// Match the pending announces to the actors in free (class and place; where the object was last heard of if it has been
// streamed since). Bound actors leave free. An announce nothing matched keeps a ghost (GhostTable) until it expires.
void ObjectSync::BindAnnounced(std::vector<LevelActor>& free, double nowMs) {
    for (uint64_t id : ghosts_.Expire(nowMs))
        if (announced_.erase(id)) {
            ++n_.ghostsExpired;
            HSM_LOG("props: no actor here for announced object %016llx after %.0f s; it is not synced here", static_cast<unsigned long long>(id), kGhostTtlMs / 1000.0);
        }
    if (announced_.empty() || free.empty()) {
        for (auto& [id, a] : announced_)
            if (!ghosts_.IsGhost(id)) ghosts_.Note(id, nowMs);
        return;
    }
    std::vector<uint64_t> ids;
    std::vector<CensusEntry> open;
    for (const auto& [id, a] : announced_) {
        CensusEntry e;
        e.index = uint16_t(ids.size());
        e.classHash = a.msg.classHash;
        e.px = a.msg.px;
        e.py = a.msg.py;
        e.pz = a.msg.pz;
        if (const BodyStateMsg* s = view_.Find(id, 0)) { e.px = s->px; e.py = s->py; e.pz = s->pz; }   // where it is now, not where it appeared
        ids.push_back(id);
        open.push_back(e);
    }
    std::vector<CensusCandidate> locals;
    locals.reserve(free.size());
    for (const LevelActor& a : free) locals.push_back(CensusCandidate{a.classHash, float(a.at.x), float(a.at.y), float(a.at.z)});
    const std::vector<int> match = MatchCensus(open, locals);
    std::vector<bool> used(free.size(), false);
    for (size_t i = 0; i < open.size(); ++i) {
        const uint64_t id = ids[i];
        if (match[i] < 0) {
            if (!ghosts_.IsGhost(id)) ghosts_.Note(id, nowMs);
            continue;
        }
        Prop* p = Track(id, free[size_t(match[i])].actor);
        if (!p) continue;
        used[size_t(match[i])] = true;
        p->spawned = IsSpawnedId(id);
        if (const auto an = announced_.find(id); an != announced_.end())   // a level actor of this machine's walk, as on the host
            MarkFound(id, *p, an->second.msg.bodyCount, std::max<uint8_t>(1, free[size_t(match[i])].bodies));
        if (env_.host) reconciler_.SetBodyCount(id, uint8_t(std::clamp<size_t>(p->bodies.size(), 1, kMaxAnnounceBodies)));
        ghosts_.Resolve(id);
        announced_.erase(id);
        ++n_.bound;
        Bound(id, nowMs);
    }
    std::vector<LevelActor> rest;
    for (size_t j = 0; j < free.size(); ++j)
        if (!used[j]) rest.push_back(free[j]);
    free = std::move(rest);
}

// An object got its actor here: the state heard of it is driven once, and the events that waited for it replay.
void ObjectSync::Bound(uint64_t id, double nowMs) {
    auto it = props_.find(id);
    if (it == props_.end()) return;
    it->second.dirty = view_.Find(id) != nullptr;
    for (size_t i = 0; i < queuedEvents_.size();) {
        // Also an Attached that waited for this object as its parent (it names it in otherId).
        const ObjectEventMsg& q = queuedEvents_[i].msg;
        const bool mine = q.objId == id || (q.kind == ObjectEventKind::Attached && q.otherId == id);
        if (mine && ReplayNow(queuedEvents_[i].msg, nowMs)) queuedEvents_.erase(queuedEvents_.begin() + long(i));
        else ++i;
    }
}

std::vector<std::vector<uint8_t>> ObjectSync::ObjectsForJoiner() const {
    std::vector<std::vector<uint8_t>> out;
    if (!active_ || !env_.host) return out;
    for (const ObjectAnnounceMsg& a : reconciler_.AnnounceAll()) out.push_back(Encode(a));
    for (const std::vector<uint8_t>& h : history_) out.push_back(h);
    for (std::vector<uint8_t>& f : shatter_.log.ForJoiner()) out.push_back(std::move(f));   // breaks of geometry collections, in order
    return out;
}

// ---------------------------------------------------------------- host migration (0.8.0-pre2)

std::vector<ObjectAnnounceMsg> ObjectSync::ExportKnown() const {
    std::vector<ObjectAnnounceMsg> out;
    if (!active_) return out;
    if (env_.host) return reconciler_.AnnounceAll();
    for (const auto& [id, p] : props_) {   // the objects bound here under their canonical ids
        if (!IsSpawnedId(id) || p.provisional || p.sub || p.classHash == 0 || !adapter_->StillValid(p.actor, p.index)) continue;
        const FVector at = adapter_->ActorLocation(p.actor);
        if (!PlaceOk(at)) continue;
        ObjectAnnounceMsg m;
        m.objId = id; m.flags = kAnnounceCanonical; m.classHash = p.classHash;
        m.px = float(at.x); m.py = float(at.y); m.pz = float(at.z);
        m.bodyCount = uint8_t(std::clamp<size_t>(p.bodies.size(), 1, kMaxAnnounceBodies));
        out.push_back(m);
        if (out.size() >= kMaxKnownObjects) return out;
    }
    for (const auto& [id, a] : announced_)   // heard, no actor here yet
        if (out.size() < kMaxKnownObjects) out.push_back(a.msg);
    return out;
}

void ObjectSync::AdoptKnown(const std::vector<ObjectAnnounceMsg>& known) {
    if (!active_ || !env_.host) return;
    for (const ObjectAnnounceMsg& a : known) {
        ObjectAnnounceMsg c = a;
        c.flags = kAnnounceCanonical;
        c.proposalId = 0;
        if (!reconciler_.Adopt(c)) continue;
        adopt_.push_back(c);
        ++n_.adoptKnown;
    }
    adoptUntilMs_ = 0;   // the clock starts at the first census of the level (HostCensus)
    HSM_LOG("props: adopted %zu objects of the previous host", adopt_.size());
}

// Host: the adopted announces whose object has an actor here, by class and place (the same match a client makes): that
// actor is the object. What nothing matches waits (the object may not have been created yet) until adoptUntilMs_.
void ObjectSync::BindAdopted(std::vector<LevelActor>& unnamed, double nowMs) {
    if (adopt_.empty()) return;
    if (adoptUntilMs_ == 0) {   // the first pass of this level: tell everyone the ids again, and start the wait
        adoptUntilMs_ = nowMs + 30000.0;
        for (const ObjectAnnounceMsg& a : reconciler_.AnnounceAll())
            if (env_.send) env_.send(Encode(a), true);
    } else if (nowMs >= adoptUntilMs_) {
        n_.adoptExpired += adopt_.size();
        HSM_LOG("props: %zu adopted objects found no actor here; they are not synced from this host", adopt_.size());
        adopt_.clear();
        return;
    }
    if (unnamed.empty()) return;
    std::vector<CensusEntry> open;
    for (size_t i = 0; i < adopt_.size(); ++i) {
        CensusEntry e;
        e.index = uint16_t(i);
        e.classHash = adopt_[i].classHash;
        e.px = adopt_[i].px; e.py = adopt_[i].py; e.pz = adopt_[i].pz;
        open.push_back(e);
    }
    std::vector<CensusCandidate> locals;
    for (const LevelActor& a : unnamed) locals.push_back(CensusCandidate{a.classHash, float(a.at.x), float(a.at.y), float(a.at.z)});
    const std::vector<int> match = MatchCensus(open, locals);
    std::vector<bool> used(unnamed.size(), false), done(adopt_.size(), false);
    for (size_t i = 0; i < open.size(); ++i) {
        if (match[i] < 0) continue;
        const LevelActor& a = unnamed[size_t(match[i])];
        Prop* p = Track(adopt_[i].objId, a.actor);
        if (!p) continue;
        p->spawned = true;
        MarkFound(adopt_[i].objId, *p, adopt_[i].bodyCount, std::max<uint8_t>(1, a.bodies));
        used[size_t(match[i])] = true;
        done[i] = true;
        reconciler_.UpdatePlace(adopt_[i].objId, float(a.at.x), float(a.at.y), float(a.at.z));
        ++n_.adoptBound;
        HSM_LOG("props: adopted object %016llx is %s", static_cast<unsigned long long>(adopt_[i].objId), ObjectName(a.actor).c_str());
    }
    std::vector<ObjectAnnounceMsg> restAdopt;
    for (size_t i = 0; i < adopt_.size(); ++i) if (!done[i]) restAdopt.push_back(adopt_[i]);
    adopt_ = std::move(restAdopt);
    std::vector<LevelActor> rest;
    for (size_t j = 0; j < unnamed.size(); ++j) if (!used[j]) rest.push_back(unnamed[j]);
    unnamed = std::move(rest);
}

// ---------------------------------------------------------------- events

void ObjectSync::SendEvent(const ObjectEventMsg& m) {
    std::vector<uint8_t> bytes = Encode(m);
    if (env_.send) env_.send(bytes, true);
    ++n_.eventsSent;
    if (env_.host && m.kind == ObjectEventKind::Fracture) shatter_.log.Record(m);   // its own bounded log (ObjectsForJoiner)
    else if (env_.host && m.kind != ObjectEventKind::Custom) {
        if (history_.size() >= kMaxHistory) history_.erase(history_.begin());
        history_.push_back(std::move(bytes));
    }
}

// The objects I stream: a joint that broke, a body that left its parent, an object that was attached to another or worn.
void ObjectSync::PollEvents(double nowMs) {
    // Events waiting for an object (or the parent they name) are tried again: the object may have been bound by a census or
    // an announce that did not go through Bound, or its parent may have come after it.
    for (size_t i = 0; i < queuedEvents_.size();) {
        if (nowMs - queuedEvents_[i].atMs > kEventKeepMs || ReplayNow(queuedEvents_[i].msg, nowMs)) queuedEvents_.erase(queuedEvents_.begin() + long(i));
        else ++i;
    }
    const uint8_t me = MySlot();
    std::vector<uint64_t> gone;
    for (auto& [id, p] : props_) {
        if (IsWeaponId(id) || p.provisional || p.sub || !adapter_->StillValid(p.actor, p.index)) continue;
        // Structures (soft and local, hsm_structure.h): every machine reports the joints that break and the bodies that come off
        // in its own copy (a barrel's plank is welded to a ring and comes off when it is hit hard), and the host breaks its copy
        // and passes it on. A break this machine's sync may have caused (ClassifyLocalBreak: while the structure settles, just
        // after a snap or a rest placement), or that the host could not use (another shape, too many at once), is not sent at once: NextBreakStep holds
        // it for a later poll or keeps it here, where the copy is marked localBroken (SoftDrive says what becomes of it). Any
        // other object: its streamer only.
        const bool streamer = view_.Streamer(id) == me;
        const bool ownCopy = OwnCopy(id, p);
        if (!MayReportJoint(streamer, ownCopy)) continue;
        const LocalBreak share = streamer ? LocalBreak::Share : ClassifyLocalBreak(nowMs, p.bornMs, p.lastSnapMs, p.shapeDiffers, p.restPlacedMs,
                                                                             env_.structureShareHeld ? p.lastHitMs : -1e18);
        auto reason = [&] { return share == LocalBreak::Share && !streamer && !sendBudget_.Take(nowMs) ? LocalBreak::OverBudget : share; };
        for (size_t ci = 0; ci < p.constraints.size(); ++ci) {
            const uint64_t bit = uint64_t(1) << ci;
            const ConstraintRef& c = p.constraints[ci];
            if ((p.brokenMask | p.localBroken) & bit) continue;
            const bool held = (p.heldJoints & bit) != 0;
            if (!held) {
                // The game takes a barrel's joint away as it breaks it (BreakConstraint, then K2_DestroyComponent), so on a
                // structure a joint gone from a live actor broke.
                if (adapter_->StillValid(c.comp, c.index)) {
                    Call ib(c.comp, "IsBroken");
                    if (!ib.invoke() || !ib.get<bool>("ReturnValue")) continue;
                } else if (!ownCopy || !env_.structureShareHeld) {
                    continue;
                }
                if (ownCopy) LogStructureBreak(p, ci);
                if (p.following) ++n_.breaksWhileFollowing;   // a tear while this copy follows its streamer copy
            }
            const LocalBreak why = reason();
            const BreakStep step = NextBreakStep(why, env_.structureShareHeld, p.found);
            if (step == BreakStep::Hold) {
                if (!held) HoldBreakHere(p, "joint", ci, why);
                p.heldJoints |= bit;
                continue;
            }
            p.heldJoints &= ~bit;
            if (step == BreakStep::Keep) {
                KeepBreakHere(p, "joint", ci, why);
                p.localBroken |= bit;
                continue;
            }
            if (!streamer) { LogLocalBreak(p, "joint", ci, why, held); ++n_.jointsReported; }
            p.brokenMask |= bit;
            ObjectEventMsg e;
            e.objId = id;
            e.kind = ObjectEventKind::JointBroke;
            e.constraint = uint8_t(ci);
            SendEvent(e);
        }
        for (size_t b = 1; b < p.bodies.size(); ++b) {
            const uint64_t bit = uint64_t(1) << b;
            BodyRef& r = p.bodies[b];
            if (!(p.attachedMask & bit) || r.detached) continue;
            const bool held = (p.heldBodies & bit) != 0;
            if (!adapter_->StillValid(r.comp, r.index)) {
                if (held) {   // nothing left to pass on
                    p.heldBodies &= ~bit;
                    p.attachedMask &= ~bit;
                    p.bodyOffHere |= bit;
                }
                continue;
            }
            if (!held) {
                Call ap(r.comp, "GetAttachParent");
                if (!ap.invoke() || ap.get<UObject*>("ReturnValue")) continue;
                if (p.following) ++n_.breaksWhileFollowing;
            }
            const LocalBreak why = reason();
            const BreakStep step = NextBreakStep(why, env_.structureShareHeld, p.found);
            if (step == BreakStep::Hold) {   // keeps its attachedMask bit, so the next poll comes back to it
                if (!held) HoldBreakHere(p, "body", b, why);
                p.heldBodies |= bit;
                continue;
            }
            const uint64_t other = step == BreakStep::Send ? MintSpawned() : 0;
            if (step == BreakStep::Send && !other) continue;   // no id to give it: the next poll tries again
            p.heldBodies &= ~bit;
            if (step == BreakStep::Keep) {   // came off here only: not passed on, never scanned again
                KeepBreakHere(p, "body", b, why);
                p.attachedMask &= ~bit;
                p.bodyOffHere |= bit;
                continue;
            }
            if (!streamer) { LogLocalBreak(p, "body", b, why, held); ++n_.jointsReported; }
            p.attachedMask &= ~bit;
            r.detached = true;
            ObjectEventMsg e;
            e.objId = id;
            e.body = uint8_t(b);
            e.kind = ObjectEventKind::Detached;
            e.otherId = other;
            if (env_.host) detached_.insert(other);
            SendEvent(e);
            TrackSub(other, p, b, nowMs, true);
            ++n_.bodiesDetached;
        }
        if (!streamer) continue;
        if (p.attachedSent) {
            // Attach is two-way: an object I stream that was put on another and has been let go of (taken off, knocked off)
            // streams again from here, and everyone takes it off its parent: Detached on body 0 (a rev 26 receiver ignores a
            // Detached of body 0), naming the parent it left.
            if (p.attachParent && adapter_->StillValid(p.actor, p.index) && !ParentActorOf(p.actor)) {
                ObjectEventMsg e;
                e.objId = id;
                e.body = 0;
                e.kind = ObjectEventKind::Detached;
                e.otherId = p.attachParent;
                p.attachedSent = false;
                p.attachParent = 0;
                attach_.OnDetached(id);
                p.lastSentMs = -1e18;
                SendEvent(e);
                ++n_.letGoSent;
            }
            continue;
        }
        if (!(p.spawned || p.level)) continue;
        UObject* parent = ParentActorOf(p.actor);
        if (!parent) continue;
        if (auto q = byActor_.find(parent); q != byActor_.end() && q->second != id) {   // put on another tracked object
            p.attachedSent = true;
            p.attachParent = q->second;
            ObjectEventMsg e;
            e.objId = id;
            e.kind = ObjectEventKind::Attached;
            e.otherId = q->second;
            SendEvent(e);
        } else if (p.spawned && adapter_->IsWillie(parent)) {   // picked up and worn (armour): gone from the floor everywhere
            ObjectEventMsg e;
            e.objId = id;
            e.kind = ObjectEventKind::Destroyed;
            SendEvent(e);
            ++n_.destroyedSent;
            gone.push_back(id);
        }
    }
    for (uint64_t id : gone) {
        destroyed_.insert(id);
        if (env_.host) reconciler_.Remove(id);
        if (auto it = props_.find(id); it != props_.end()) { byActor_.erase(it->second.actor); props_.erase(it); }
    }
}

bool ObjectSync::OnObjectEvent(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return false;
    ObjectEventMsg m;
    if (!Decode(d, n, m) || (IsLevelProp(m.objId) && !InCensus(m.objId))) { ++n_.eventsRejected; return false; }
    const double now = SteadyNowMs();
    // A joint that broke or a body that came off in a structure's copy (soft and local, hsm_structure.h): any machine speaks
    // for its own copy (PollEvents), so the break reaches every screen.
    const auto sp = props_.find(m.objId);
    const bool structJoint = sp != props_.end() && OwnCopy(sp->first, sp->second) &&
                             (m.kind == ObjectEventKind::JointBroke || (m.kind == ObjectEventKind::Detached && m.body != 0));
    if (env_.host) {
        if (fromSlot == kHostSlot || fromSlot >= kMaxFighters) { ++n_.eventsRejected; return false; }
        if (structJoint && table_.Streamer(m.objId) != fromSlot) {
            const Prop& q = sp->second;
            // Already broken here (both saw it), or not one of its joints or bodies: nothing to pass on.
            const bool fresh = m.kind == ObjectEventKind::JointBroke
                                   ? m.constraint < q.constraints.size() && !(q.brokenMask & (uint64_t(1) << m.constraint))
                                   : m.body < q.bodies.size() && !q.bodies[m.body].detached;
            if (!fresh) return false;
            if (!jointBudget_[fromSlot].Take(now)) {   // more than a machine could break in a fight
                if (n_.jointReportsDropped++ == 0)
                    HSM_LOG("props: structure breaks from slot %u come too fast; the extra ones are not passed on", unsigned(fromSlot));
                return false;
            }
        }
        // Only the object's streamer speaks for it, and only about objects the host knows.
        if (m.kind != ObjectEventKind::Custom && !structJoint && table_.Streamer(m.objId) != fromSlot) { ++n_.eventsRejected; return false; }
        if (IsSpawnedId(m.objId) && !reconciler_.Knows(m.objId) && !detached_.count(m.objId)) { ++n_.eventsRejected; return false; }
        if (m.kind == ObjectEventKind::Detached && m.body != 0) {   // body 0: the whole object let go of its parent; otherId is the parent
            if (!ValidSpawnedId(m.otherId) || SpawnedIdSlot(m.otherId) != fromSlot || detached_.size() >= kMaxKnownObjects) { ++n_.eventsRejected; return false; }
            detached_.insert(m.otherId);
        }
        if (m.kind == ObjectEventKind::Fracture) {
            if (m.body >= kMaxObjectBodies || !shatter_.log.Record(m)) return false;   // a second report of the same break is not relayed again
        } else if (m.kind != ObjectEventKind::Custom) {
            if (history_.size() >= kMaxHistory) history_.erase(history_.begin());
            history_.emplace_back(d, d + n);
        }
        if (m.kind == ObjectEventKind::Destroyed && IsSpawnedId(m.objId)) reconciler_.Remove(m.objId);
    } else if (fromSlot != kHostSlot && view_.Streamer(m.objId) != fromSlot) {   // mesh: straight from the object's streamer
        // A structure's break from a peer is not a fault: it comes again through the host, which drops repeats and caps it.
        if (!structJoint) ++n_.eventsRejected;
        return false;
    }
    Replay(m, now);
    return true;
}

void ObjectSync::Replay(const ObjectEventMsg& m, double nowMs) {
    if (ReplayNow(m, nowMs)) return;
    if (queuedEvents_.size() >= kMaxQueuedEvents) queuedEvents_.erase(queuedEvents_.begin());
    queuedEvents_.push_back(QueuedEvent{m, nowMs});
    ++n_.eventsQueued;
}

// false: the object (or the parent it names) is not here yet, so the event waits.
bool ObjectSync::ReplayNow(const ObjectEventMsg& m, double nowMs) {
    auto it = props_.find(m.objId);
    switch (m.kind) {
        case ObjectEventKind::Destroyed: {
            if (!IsSpawnedId(m.objId)) return true;
            spawnGate_.OnGone(m.objId);
            if (destroyed_.size() < 4 * kMaxKnownObjects) destroyed_.insert(m.objId);
            attach_.OnDetached(m.objId);
            announced_.erase(m.objId);
            ghosts_.Resolve(m.objId);
            if (it == props_.end()) return true;
            Prop& p = it->second;
            // A copy held in a hand or worn is not taken away (the owner's pose and Armor events decide those).
            if (adapter_->StillValid(p.actor, p.index) && !ObjectField(p.actor, 0) && !ParentActorOf(p.actor)) adapter_->DestroyActor(p.actor);
            if (!p.sub) byActor_.erase(p.actor);
            props_.erase(it);
            ++n_.destroyedApplied;
            return true;
        }
        case ObjectEventKind::JointBroke: {
            if (it == props_.end()) return false;
            Prop& p = it->second;
            if (m.constraint >= p.constraints.size() || p.shapeDiffers) return true;   // another shape: its joint N is not this one
            const ConstraintRef& c = p.constraints[m.constraint];
            if (OwnCopy(it->first, p)) LogStructureBreak(p, m.constraint);
            p.brokenMask |= uint64_t(1) << m.constraint;   // set first: breaking it here is not an event of mine
            p.localBroken &= ~(uint64_t(1) << m.constraint);   // broken everywhere now: no longer this copy's alone
            p.heldJoints &= ~(uint64_t(1) << m.constraint);
            if (!adapter_->StillValid(c.comp, c.index)) return true;
            Call ib(c.comp, "IsBroken");
            if (ib.invoke() && ib.get<bool>("ReturnValue")) return true;
            Call(c.comp, "BreakConstraint").invoke();
            ++n_.jointsBroken;
            ++n_.eventsApplied;
            return true;
        }
        case ObjectEventKind::Detached: {
            if (it == props_.end()) return false;
            Prop& p = it->second;
            if (m.body == 0) {   // the whole object let go of the parent it was attached to: it is loose and streamed again
                attach_.OnDetached(m.objId);
                if (!p.attachedSent) return true;
                p.attachedSent = false;
                p.attachParent = 0;
                p.dirty = view_.Find(m.objId) != nullptr;
                if (adapter_->StillValid(p.actor, p.index))
                    Call(p.actor, "K2_DetachFromActor").set<uint8_t>("LocationRule", 1).set<uint8_t>("RotationRule", 1).set<uint8_t>("ScaleRule", 1).invoke();
                ++n_.letGoApplied;
                ++n_.eventsApplied;
                return true;
            }
            if (m.body >= p.bodies.size() || p.shapeDiffers) return true;   // another shape: its body N is not this one
            BodyRef& r = p.bodies[m.body];
            if (r.detached) {
                if (!env_.host) AdoptSubId(r.comp, m.otherId);   // it came off here too, under the id this machine gave it
                return true;
            }
            r.detached = true;
            p.attachedMask &= ~(uint64_t(1) << m.body);
            p.bodyOffHere &= ~(uint64_t(1) << m.body);   // off everywhere now: no longer this copy's alone
            p.heldBodies &= ~(uint64_t(1) << m.body);
            if (adapter_->StillValid(r.comp, r.index)) {
                Call(r.comp, "K2_DetachFromComponent")
                    .set<uint8_t>("LocationRule", 1).set<uint8_t>("RotationRule", 1).set<uint8_t>("ScaleRule", 1).set<bool>("bCallModify", false)
                    .invoke();
                // A structure's body that came off (a barrel's plank, welded to a ring until it is hit) simulates on the machine
                // it came off on, as the game's own break does; here too, or it would hang where it was.
                if (OwnCopy(it->first, p)) {
                    Call(r.comp, "SetSimulatePhysics").set<bool>("bSimulate", true).invoke();
                    GameBreakHere(it->first, p, m.body);
                }
            }
            if (!props_.count(m.otherId)) TrackSub(m.otherId, p, m.body, nowMs, false);
            ++n_.bodiesDetached;
            ++n_.eventsApplied;
            return true;
        }
        case ObjectEventKind::Attached: {
            if (it == props_.end()) return false;
            auto parent = props_.find(m.otherId);
            if (parent == props_.end()) return false;
            Prop& p = it->second;
            if (p.attachedSent) return true;
            p.attachedSent = true;
            p.attachParent = m.otherId;
            // States of the attacher still in flight must not read as "streamed again" and undo this (hsm_object_attach.h).
            attach_.OnAttached(m.objId, view_.Streamer(m.objId), view_.Level(m.objId), nowMs);
            if (adapter_->StillValid(p.actor, p.index) && adapter_->StillValid(parent->second.actor, parent->second.index))
                Call(p.actor, "K2_AttachToActor")
                    .set<UObject*>("ParentActor", parent->second.actor).set<FName>("SocketName", FName{})
                    .set<uint8_t>("LocationRule", 1).set<uint8_t>("RotationRule", 1).set<uint8_t>("ScaleRule", 1)
                    .set<bool>("bWeldSimulatedBodies", false).invoke();
            ++n_.eventsApplied;
            return true;
        }
        case ObjectEventKind::Fracture:
            return ShatterReplay(m, nowMs);   // shatter_sync.cpp
        case ObjectEventKind::Custom:
            return true;   // a mod's own event (M5): relayed, nothing replays it here
    }
    return true;
}

// A barrel's plank that came off on another machine comes off here as a hit would take it off: the game's "Spawn Sound On Hit"
// breaks its joint, turns off its collision with the three rings, plays the break and lowers the barrel's Health, which the
// Cellar's boss barrel is judged by. It also remembers the plank, so a later hit here counts the Health from all the planks. The
// force only has to pass the game's limit for a hit that names no weapon (5000). A hit with no actor reads as no weapon.
bool ObjectSync::GameBreakHere(uint64_t id, Prop& p, size_t body) {
    constexpr double kGameBreakForce = 100000.0;
    constexpr size_t kHitImpactPoint = 40;   // FHitResult::ImpactPoint (adapter.cpp reads it there too)
    BodyRef& r = p.bodies[body];
    UClass* barrel = FindClass("BP_Barrel_Destructable_Constraits_C");
    const bool plank = barrel && IsChildOf(ClassOf(p.actor), barrel) && ObjectName(r.comp).find("Destruction_001") != std::string::npos;
    if (!ReplaysGameBreak(env_.structureGameBreak, OwnCopy(id, p), plank)) return false;
    Call hit(p.actor, "Spawn Sound On Hit");
    void* comp = hit.valid() ? hit.paramPtr("AttachToComponent") : nullptr;
    auto* result = hit.valid() ? static_cast<uint8_t*>(hit.paramPtr("Hit")) : nullptr;
    RootCalls* c = CallsFor(r.comp);
    if (!comp || !result || !c || !c->toWorld.invoke(r.comp)) {
        static bool logged = false;
        if (!logged) { logged = true; HSM_LOG("props: the barrel's hit function is not as expected; planks come off without it"); }
        return false;
    }
    // Its joint goes with it: the game destroys it, and that is not a break of this machine's to report.
    for (size_t ci = 0; ci < p.constraints.size(); ++ci) {
        const ConstraintRef& k = p.constraints[ci];
        if (!adapter_->StillValid(k.comp, k.index)) continue;
        Call ap(k.comp, "GetAttachParent");
        if (!ap.invoke() || ap.get<UObject*>("ReturnValue") != r.comp) continue;
        const uint64_t bit = uint64_t(1) << ci;
        p.brokenMask |= bit;
        p.localBroken &= ~bit;
        p.heldJoints &= ~bit;
    }
    const FTransform at = c->toWorld.take<FTransform>(0);
    std::memcpy(result + kHitImpactPoint, &at.translation, sizeof at.translation);
    *static_cast<UObject**>(comp) = r.comp;
    if (!hit.set<FVector>("A", FVector{kGameBreakForce, 0, 0}).invoke()) return false;
    ++n_.gameBreaks;
    if (StructLogBudget()) {
        double* health = Field<double>(p.actor, "Health");
        HSM_LOG("props: barrel %s plank %zu came off by the game's own code, health %.2f", ObjectName(p.actor).c_str(), body, health ? *health : -1.0);
    }
    return true;
}

// A body that left its object is an object of its own on the same actor: one body, no census, no byActor_ entry.
// A body that came off on two machines at once got an id on each, and the host passed one on. This machine's sub takes that id,
// or its body would stay out of the stream on this screen: the host knows only the id it took.
void ObjectSync::AdoptSubId(const UObject* comp, uint64_t canonical) {
    if (props_.count(canonical)) return;
    for (auto it = props_.begin(); it != props_.end(); ++it) {
        Prop& s = it->second;
        if (!s.sub || s.bodies.empty() || s.bodies[0].comp != comp) continue;
        auto node = props_.extract(it);
        node.key() = canonical;
        Prop& q = props_.insert(std::move(node)).position->second;
        q.claim = LeaseClaim{};
        q.lastSentMs = -1e18;
        return;
    }
}

ObjectSync::Prop* ObjectSync::TrackSub(uint64_t id, Prop& from, size_t body, double nowMs, bool mine) {
    if (props_.count(id) || body >= from.bodies.size()) return nullptr;
    Prop& s = props_[id];
    s.actor = from.actor;
    s.index = from.index;
    s.sub = true;
    s.spawned = true;
    s.rootBody = false;
    s.classHash = from.classHash;
    BodyRef r;
    r.comp = from.bodies[body].comp;
    r.index = from.bodies[body].index;
    s.bodies.assign(1, r);
    s.root = r.comp;
    s.rootIndex = r.index;
    if (mine) { s.lastContactMs = nowMs; s.toucher = uint8_t(MySlot() + 1); }   // the machine that saw it come off leases it
    return &s;
}

}  // namespace hsm::game
