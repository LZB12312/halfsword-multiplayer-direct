#include "gameplay/object_sync.h"
#include "log.h"
#include "session/hsm_ground_clear.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>

// Map reset (session/hsm_map_reset.h). At a Duel round reset and a rematch the host sends one MapReset; each machine then puts
// its own copy of the map back as the match began, without a level reload and without a stream of messages:
//   level props    their start transform (read once, RecordStart, when the prop was first tracked and the match had not begun)
//                  with zero velocities; the lease goes back to FREE, so the host streams the prop again from where it lies
//   spawned        objects tracked after the match began (a helmet knocked off, a snapped plank, a thrown barrel) are destroyed
//                  on every machine, not announced as lost; what the level spawned during its own start-up stays
//   level weapons  the weapons the level placed (MarkBaseline records them: Parent Actor and Last Parent empty) go back on
//                  their spot; one a fighter picked up and dropped was destroyed by the drop clear, so a new one of its
//                  class is spawned (a procedural weapon, whose class is the bare base, cannot be: skipped with a log)
// Each machine does this on its own copy, so every machine ends in the same state; the host's FREE streams only confirm it.
// The work is queued and done resetPerFrame_ items a frame (ResetPerFrame: never more than three frames), so a map with
// hundreds of props does not spike one frame. Not covered: a shattered geometry collection (its pieces are local debris
// that cannot be put back), a body that came off its object (it belongs to that object), a level prop first tracked after
// the match began (no start recorded).

namespace hsm::game {
using namespace hsm::ue;
using namespace hsm::game::hands;

namespace {
constexpr double kBaselineGraceMs = 5000.0;   // objects tracked within this long of the baseline are the level's own start-up
constexpr size_t kMaxLevelWeapons = 256;
constexpr uint64_t kLogFirstSpawns = 6;

bool Finite(const FVector& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// A weapon (or any actor) put back on its spot, at rest: its mesh's velocities set to zero when it simulates physics.
void PlaceActor(UObject* actor, const FVector& at, const FRotator& rot) {
    Call move(actor, "K2_SetActorLocationAndRotation");
    move.set<FVector>("NewLocation", at).set<FRotator>("NewRotation", rot).set<bool>("bSweep", false).set<bool>("bTeleport", true);
    if (!move.invoke()) return;
    if (UObject** meshPtr = Field<UObject*>(actor, "BaseMesh")) {
        if (UObject* mesh = *meshPtr) {
            Call sim(mesh, "IsSimulatingPhysics");
            if (sim.invoke() && sim.get<bool>("ReturnValue")) {
                Call(mesh, "SetPhysicsLinearVelocity").set("NewVel", FVector{}).set<bool>("bAddToCurrent", false).setName("BoneName", FName{}).invoke();
                Call(mesh, "SetPhysicsAngularVelocityInDegrees").set("NewAngVel", FVector{}).set<bool>("bAddToCurrent", false)
                    .setName("BoneName", FName{}).invoke();
            }
        }
    }
}
}  // namespace

void ObjectSync::MarkBaseline() {
    if (!active_ || !adapter_ || baselineMs_ >= 0) return;
    baselineMs_ = SteadyNowMs();
    levelWeapons_.clear();
    for (UObject* w : adapter_->LevelWeapons()) {
        if (levelWeapons_.size() >= kMaxLevelWeapons) break;
        LevelWeapon lw;
        lw.actor = w;
        lw.index = ObjectIndex(w);
        lw.classHash = ClassHashOf(w);
        lw.at = adapter_->ActorLocation(w);
        lw.rot = adapter_->ActorRotation(w);
        if (Finite(lw.at)) levelWeapons_.push_back(lw);
    }
    HSM_LOG("reset: the match began here; %zu level weapon(s) recorded", levelWeapons_.size());
}

bool ObjectSync::IsLevelWeaponAtStart(UObject* actor) {
    if (!actor || !adapter_) return false;
    for (const LevelWeapon& w : levelWeapons_) {
        if (w.actor != actor || !adapter_->StillValid(w.actor, w.index)) continue;
        const FVector now = adapter_->ActorLocation(actor);
        const double dx = now.x - w.at.x, dy = now.y - w.at.y, dz = now.z - w.at.z;
        return Finite(now) && AtLevelStart(dx * dx + dy * dy + dz * dz);
    }
    return false;
}

void ObjectSync::RecordStart(Prop& p) {
    p.startTried = true;
    // After the match began a prop may lie anywhere: where it is now is not where the level had it.
    if (baselineMs_ >= 0 && SteadyNowMs() > baselineMs_ + kBaselineGraceMs) return;
    std::vector<StartBody> st(p.bodies.size());
    for (size_t b = 0; b < st.size(); ++b)
        if (Read(p, b, st[b].s)) st[b].ok = true;
    if (!st.empty() && st[0].ok) p.start = std::move(st);
}

bool ObjectSync::ShatterBroken(uint64_t id) const {
    const auto it = shatter_.objs.find(id);
    if (it == shatter_.objs.end()) return false;
    for (const ShatterBody& b : it->second.gcs)
        if (!b.broken.empty()) return true;
    return false;
}

// Plans the whole reset now (what each object is, from PlanReset) and queues the work; StepReset does it a few items a frame.
void ObjectSync::MapReset(double nowMs, uint8_t flags) {
    if (!active_ || !adapter_) return;
    if (!resetQueue_.empty()) { resetPerFrame_ = resetQueue_.size(); StepReset(nowMs); }   // a reset still running: finish it first
    resetQueue_.clear();
    resetTally_ = ResetTally{};
    const char* what = (flags & kMapResetRematch) ? "rematch" : "round";
    if (baselineMs_ < 0) { HSM_LOG("reset: %s, but the match has not begun here yet; nothing to reset", what); return; }
    size_t noStart = 0, broken = 0;
    for (auto& [id, p] : props_) {
        if (!p.level && !p.spawned) continue;
        if (OwnCopy(id, p)) { ++n_.structResetSkipped; resetTally_.Count(ResetAction::Leave); continue; }   // a structure is never teleported
        ResetCandidate c;
        c.kind = p.level ? ResetKind::LevelProp : ResetKind::Spawned;
        c.alive = adapter_->StillValid(p.actor, p.index);
        c.haveStart = !p.start.empty();
        c.broken = p.level && ShatterBroken(id);
        c.held = c.alive && (p.attachedSent || ObjectField(p.actor, 0) != nullptr);
        c.bornInRound = p.spawned && p.bornMs > baselineMs_ + kBaselineGraceMs;
        c.detached = p.sub;
        const ResetAction a = PlanReset(c);
        resetTally_.Count(a);
        if (a == ResetAction::Skip) { if (c.broken) ++broken; else if (!c.haveStart) ++noStart; }
        if (a == ResetAction::Restore || a == ResetAction::Destroy) { ResetItem it; it.action = a; it.id = id; resetQueue_.push_back(it); }
    }
    size_t noClass = 0;
    for (size_t i = 0; i < levelWeapons_.size(); ++i) {
        const LevelWeapon& w = levelWeapons_[i];
        ResetCandidate c;
        c.kind = ResetKind::LevelWeapon;
        c.haveStart = true;
        c.alive = adapter_->StillValid(w.actor, w.index);
        c.held = c.alive && ObjectField(w.actor, 0) != nullptr;
        c.classKnown = c.alive || WeaponClassByHash(w.classHash, nowMs) != nullptr;
        const ResetAction a = PlanReset(c);
        resetTally_.Count(a);
        if (a == ResetAction::Skip) ++noClass;
        if (a == ResetAction::Restore || a == ResetAction::Respawn) { ResetItem it; it.action = a; it.weapon = i; it.isWeapon = true; resetQueue_.push_back(it); }
    }
    resetPerFrame_ = ResetPerFrame(resetQueue_.size());
    HSM_LOG("reset: %s: %u prop(s)/weapon(s) back to their start, %u object(s) destroyed, %u weapon(s) respawned, %u skipped "
            "(%zu with no start, %zu shattered, %zu weapon(s) of no known class), %zu frame(s)", what, resetTally_.restored, resetTally_.destroyed,
            resetTally_.respawned, resetTally_.skipped, noStart, broken, noClass,
            resetQueue_.empty() ? size_t(0) : (resetQueue_.size() + resetPerFrame_ - 1) / resetPerFrame_);
}

void ObjectSync::StepReset(double nowMs) {
    const size_t n = std::min(resetPerFrame_ ? resetPerFrame_ : resetQueue_.size(), resetQueue_.size());
    const std::vector<ResetItem> step(resetQueue_.begin(), resetQueue_.begin() + std::ptrdiff_t(n));
    resetQueue_.erase(resetQueue_.begin(), resetQueue_.begin() + std::ptrdiff_t(n));
    for (const ResetItem& it : step) {
        if (it.isWeapon) { ResetWeapon(it.weapon, it.action, nowMs); continue; }
        if (auto p = props_.find(it.id); p != props_.end()) ResetProp(it.id, p->second, it.action, nowMs);
    }
}

// The prop is FREE again: the host drops whoever held the lease (a Grant to everyone), a client gives its own claim back.
void ObjectSync::ReleaseLease(uint64_t id, Prop& p, double nowMs) {
    if (env_.host) {
        uint8_t slot = kHostSlot;
        LeaseLevel level = LeaseLevel::Free;
        if (table_.Lessee(id, slot, level) && level != LeaseLevel::Free) HostRelease(id, slot);
    } else if (p.claim.sent != LeaseLevel::Free && !p.provisional) {
        Claim(id, ClaimSend::Release, LeaseLevel::Free, nowMs);
    }
    p.claim = LeaseClaim{};
    p.lastContactMs = -1e18;
    p.toucher = 0;
    p.restoreTicks = 0;
}

namespace {
// The body is where it started: within 1 cm, and its rotation within about 1 degree (q and -q are the same turn).
bool Unmoved(const BodyStateMsg& now, const BodyStateMsg& start) {
    const double dx = now.px - start.px, dy = now.py - start.py, dz = now.pz - start.pz;
    if (dx * dx + dy * dy + dz * dz > 1.0) return false;
    auto near = [&](int sign) {
        return std::abs(now.rot.x - sign * start.rot.x) < 300 && std::abs(now.rot.y - sign * start.rot.y) < 300 &&
               std::abs(now.rot.z - sign * start.rot.z) < 300 && std::abs(now.rot.w - sign * start.rot.w) < 300;
    };
    return near(1) || near(-1);
}
}  // namespace

size_t ObjectSync::ClearMovedProps(double nowMs) {
    if (!active_ || !adapter_) return 0;
    if (baselineMs_ < 0) { HSM_LOG("clear: rocks, but the match has not begun here yet; nothing to clear"); return 0; }
    std::unordered_set<UObject*> armor;                   // armour is the armour pass's, not a rock
    for (UObject* a : adapter_->LooseArmor()) armor.insert(a);
    size_t level = 0, spawned = 0;
    for (auto& [id, p] : props_) {
        if (!p.level && !p.spawned) continue;
        if (!adapter_->StillValid(p.actor, p.index) || armor.count(p.actor)) continue;
        PropCandidate c;
        c.level = p.level; c.spawned = p.spawned;
        c.haveStart = !p.start.empty();
        c.bornInMatch = p.bornMs > baselineMs_ + kBaselineGraceMs;
        c.held = p.attachedSent || ObjectField(p.actor, 0) != nullptr;
        c.denied = p.denied; c.sub = p.sub;
        c.structure = IsStructure(id, p.sub, p.bodies.size(), p.constraints.size(), p.found);   // by shape, whatever sync_structures says
        c.unmoved = true;                                  // a body that cannot be read counts as at rest: kept
        for (size_t b = 0; b < p.bodies.size() && b < p.start.size(); ++b) {
            if (!p.start[b].ok || p.bodies[b].detached) continue;
            if (BodyStateMsg here; Read(p, b, here) && !Unmoved(here, p.start[b].s)) { c.unmoved = false; break; }
        }
        if (!ClearableProp(c)) continue;
        ResetItem it; it.action = ResetAction::Destroy; it.id = id; resetQueue_.push_back(it);
        if (p.level) ++level; else ++spawned;
    }
    resetPerFrame_ = std::max(resetPerFrame_, ResetPerFrame(resetQueue_.size()));
    HSM_LOG("clear: rocks: %zu moved level prop(s) and %zu spawned object(s) to clear (%zu frame(s))", level, spawned,
            resetQueue_.empty() ? size_t(0) : (resetQueue_.size() + resetPerFrame_ - 1) / resetPerFrame_);
    (void)nowMs;
    return level + spawned;
}

void ObjectSync::ResetProp(uint64_t id, Prop& p, ResetAction action, double nowMs) {
    if (!adapter_->StillValid(p.actor, p.index)) return;
    ReleaseLease(id, p, nowMs);
    if (action == ResetAction::Restore) {
        for (size_t b = 0; b < p.bodies.size() && b < p.start.size(); ++b) {
            if (!p.start[b].ok || p.bodies[b].detached) continue;
            if (BodyStateMsg here; Read(p, b, here) && Unmoved(here, p.start[b].s)) { ++n_.resetUnmoved; continue; }   // #85: not woken
            BodyStateMsg s = p.start[b].s;
            s.vx = s.vy = s.vz = s.wx = s.wy = s.wz = 0.f;
            DriveBody(p, b, s);              // places the body (the whole actor for a root body) and zeroes what simulates
            p.bodies[b].dirty = false;
            p.bodies[b].readTick = 0;        // the next read sees the new place
        }
        p.dirty = false;
        p.moving = true;                     // the host streams it again soon (the moving schedule), confirming the place
        p.lastSentMs = -1e18;
        return;
    }
    if (action != ResetAction::Destroy) return;
    UObject* actor = p.actor;
    adapter_->DestroyActor(actor);
    if (!p.sub) byActor_.erase(actor);
    heardMs_.erase(id);
    freeSince_.erase(id);
    shatter_.objs.erase(id);
    destroyed_.insert(id);                   // a late announce of it binds nothing
    announced_.erase(id);
    if (env_.host) reconciler_.Remove(id);
    props_.erase(id);                        // not "lost": nobody is told it was destroyed, every machine does the same
}

void ObjectSync::ResetWeapon(size_t i, ResetAction action, double nowMs) {
    if (i >= levelWeapons_.size()) return;
    LevelWeapon& w = levelWeapons_[i];
    if (action == ResetAction::Restore) {
        if (adapter_->StillValid(w.actor, w.index)) PlaceActor(w.actor, w.at, w.rot);
        return;
    }
    if (action != ResetAction::Respawn) return;
    UClass* cls = WeaponClassByHash(w.classHash, nowMs);
    UObject* a = cls ? adapter_->SpawnActorAt(cls, w.at, w.rot) : nullptr;
    if (!a) { HSM_LOG("reset: a level weapon could not be spawned again (%s)", cls ? "the spawn failed" : "its class is not loaded"); return; }
    w.actor = a;
    w.index = ObjectIndex(a);
    PlaceActor(a, w.at, w.rot);
    static uint64_t spawned = 0;
    if (++spawned <= kLogFirstSpawns) HSM_LOG("reset: %s is back on its spot", ObjectName(a).c_str());
}

}  // namespace hsm::game
