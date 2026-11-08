#include "gameplay/object_sync.h"
#include "log.h"
#include "session/hsm_budget.h"
#include "sync/hsm_pose_codec.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

// Identity. A weapon's SheathSlot is a sheath type shared by many weapons and its "Last Parent" is cleared by the game,
// so no machine can name a dropped weapon from what it sees alone. The dropping fighter's machine mints the id
// (MakeDropPropId: owner entity, hand, a 7-bit sequence under a per-process drop epoch) and asks for a Touch on it;
// everyone else finds the weapon in the hand of its stand-in of that fighter (DropSignature, ResolveDropNow). An id
// is kept for the weapon's life: a pick-up (HOLD) and a later drop by anyone reuse it. A level prop has no name two
// machines share (the level manager's purge and respawn make per-machine actors), so the host names them: its census
// (session/hsm_prop_census.h) gives each an index, MakeLevelPropId(index), and each client binds its own actor to it.
//
// Level props on the wire. The host walks its level kCensusFirstMs into a fight and every kCensusRefreshMs after (at
// once when one is lost) and sends the census whenever its set changed; a client matches each complete census it has
// not matched yet, and retries the indices it could not bind every kCensusRefreshMs. A level prop is FREE (the host
// streams it) or TOUCH (the machine whose fighter touched it last), never HOLD. Most of them lie still, so they are
// read at most kLevelReadsPerTick a tick, the moving ones first.
//
// Driving. A machine that does not stream a prop teleports its copy to each new state from the prop's streamer
// (ApplyPropPose, plus the angular velocity), and only while the copy is loose here: a copy in the hand of a
// fighter simulated here (a Hold the host denied) or of a stand-in is never moved by a stream. A request for a FREE
// prop is always granted, so local physics keeps it until that Grant arrives rather than fighting the host's stream.

namespace hsm::game {
using namespace hsm::ue;
using namespace hsm::game::hands;

namespace {
// Actor-level rotation has no reflected quaternion getter/setter in this build (only K2_GetActorRotation /
// K2_SetActorLocationAndRotation, both FRotator); KismetMathLibrary's own conversion functions close the gap
// instead of reimplementing UE's rotator<->quaternion math by hand.
UObject* MathLibraryCDO() {
    static UObject* lib = nullptr;
    if (!lib) if (UObject* c = FindObject("KismetMathLibrary", "Class")) lib = DefaultObject(reinterpret_cast<UClass*>(c));
    return lib;
}

// Teleports a prop actor to a streamed pose and, for a weapon (its "BaseMesh"), sets the linear velocity.
bool ApplyPropPose(UObject* actor, float px, float py, float pz, const Quat16& rotPacked, float vx, float vy, float vz) {
    if (!actor) return false;
    UObject* mathLib = MathLibraryCDO();
    if (!mathLib) { HSM_LOG("prop_sync: KismetMathLibrary unavailable"); return false; }

    float qx, qy, qz, qw;
    UnpackQuat(rotPacked, qx, qy, qz, qw);
    Call toRot(mathLib, "Quat_Rotator");
    toRot.set<FQuat>("Q", FQuat{double(qx), double(qy), double(qz), double(qw)});
    if (!toRot.invoke()) { HSM_LOG("prop_sync: Quat_Rotator failed"); return false; }
    FRotator rot = toRot.get<FRotator>("ReturnValue");

    Call move(actor, "K2_SetActorLocationAndRotation");
    move.set<FVector>("NewLocation", FVector{double(px), double(py), double(pz)})
        .set<FRotator>("NewRotation", rot).set<bool>("bSweep", false).set<bool>("bTeleport", true);
    if (!move.invoke()) { HSM_LOG("prop_sync: K2_SetActorLocationAndRotation failed"); return false; }

    if (UObject** meshPtr = Field<UObject*>(actor, "BaseMesh")) {
        if (UObject* mesh = *meshPtr) {
            Call sim(mesh, "IsSimulatingPhysics");
            if (sim.invoke() && sim.get<bool>("ReturnValue")) {
                Call(mesh, "SetPhysicsLinearVelocity")
                    .set<FVector>("NewVel", FVector{double(vx), double(vy), double(vz)})
                    .set<bool>("bAddToCurrent", false)
                    .invoke();
            }
        }
    }
    return true;
}

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };   // a TArray<UObject*> as a call returns it

// Teleports one body (a component) to a streamed pose; its velocities are set by the caller.
bool ApplyBodyPose(UObject* comp, float px, float py, float pz, const Quat16& rotPacked) {
    UObject* mathLib = MathLibraryCDO();
    if (!comp || !mathLib) return false;
    float qx, qy, qz, qw;
    UnpackQuat(rotPacked, qx, qy, qz, qw);
    Call toRot(mathLib, "Quat_Rotator");
    toRot.set<FQuat>("Q", FQuat{double(qx), double(qy), double(qz), double(qw)});
    if (!toRot.invoke()) return false;
    Call move(comp, "K2_SetWorldLocationAndRotation");
    move.set<FVector>("NewLocation", FVector{double(px), double(py), double(pz)})
        .set<FRotator>("NewRotation", toRot.get<FRotator>("ReturnValue")).set<bool>("bSweep", false).set<bool>("bTeleport", true);
    return move.invoke();
}

constexpr double kHeardLiveMs = 3000.0;       // an id heard this recently is live somewhere (the host re-sends FREE ones every second)
constexpr double kCensusFirstMs = 3000.0;     // a fight level's first census walk (and a client's first match) waits this
                                              // long: past the level manager's purge (0.1 s) and its first respawns (2.5 s)
constexpr double kCensusSnapMs = 1500.0;     // a client records where each free prop lies this long into a fight level, before its match
constexpr size_t kStartsPerTick = 8;          // level props whose start transform is read per tick at most (map reset)
constexpr size_t kLevelReadsPerTick = 16;     // level props read (and so streamed) per tick at most, the moving ones first
// While several move: an object goes whole and a barrel is 19 bodies, so at 16 Alley fell behind (5% deferred on 0.8.0-pre30,
// 0.1% on pre27, which sent late-found barrels another way). Idle props stay within kLevelReadsPerTick.
constexpr size_t kMovingReadsPerTick = 48;
constexpr uint8_t kMovable = 2;               // EComponentMobility::Movable: only a movable root can simulate
constexpr double kIdleCheckMs = 1000.0;       // DespawnIdle: how often the FREE weapons are checked

// Where this process starts every entity's drop epoch: the wall clock, so a machine that reconnects (Stop zeroes the
// sequences) does not mint the ids its previous session gave weapons that may still lie in the level elsewhere.
uint32_t DropEpochSeed() {
    const uint64_t t = uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
    return uint32_t(t ^ (t >> 32));
}

float Clamped(double v, float limit) {
    if (!std::isfinite(v)) return 0.f;
    const double l = double(limit) * 0.999;
    return float(std::clamp(v, -l, l));
}
}  // namespace

void ObjectSync::Start(Adapter& adapter, StandIns& standIns, Env env) {
    Stop();
    dropEpoch_.fill(DropEpochSeed());
    adapter_ = &adapter;
    standIns_ = &standIns;
    env_ = std::move(env);
    active_ = true;
    reconciler_ = ObjectReconciler(kHostSlot); spawnGate_.Reset();
    minter_ = SpawnedIdMinter(MySlot());
    HSM_LOG("props: on (%s): dropped weapons are leased FREE/TOUCH/HOLD; level props by the host's census and spawned objects by "
            "announce, FREE/TOUCH, every simulating body streamed",
            env_.host ? "host arbitrates" : "client");
}

void ObjectSync::Stop() {
    if (active_) HSM_LOG("props: off (%s)", HealthLine().c_str());
    *this = ObjectSync{};
}

void ObjectSync::Clear() {
    if (!active_) return;
    table_ = LeaseTable{};
    view_.Clear();
    props_.clear();
    byActor_.clear();
    pending_.clear();
    givenUp_.clear();
    late_.clear();
    heardMs_.clear();
    freeSince_.clear();
    idleCheckMs_ = 0;
    hands_ = {};
    standInHands_ = {};
    loose_.clear();
    weapons_.clear();
    weaponsMs_ = -1e18;
    calls_.clear();
    fieldOff_ = {};
    // Level props: a new level, a new census. censusSeq_ goes on (a client may still hold the last level's census until
    // its own level change), and the 'Random Delete' name stays what it is.
    levelSinceMs_ = -1;
    censusMs_ = 0;
    censusIndex_.clear();
    censusSnap_.clear();
    censusSnapDone_ = false;
    movingCursor_ = idleCursor_ = 0;
    book_.Clear();
    sent_.clear();
    haveSent_ = false;
    census_.Clear();
    matched_ = false;
    matchedSeq_ = 0;
    bound_.clear();
    misses_ = 0;
    purged_ = false;
    rootOff_.clear();
    mobilityOff_.clear();
    classKind_.clear();
    actorKind_.clear();
    // Spawned objects: a new level, a new set. The reconciler and the minter start over (a host that took over reloaded the
    // level and announces what it finds again); the provisional ids of this machine go on from where they were.
    reconciler_ = ObjectReconciler(kHostSlot); spawnGate_.Reset();
    for (const ObjectAnnounceMsg& a : adopt_) reconciler_.Adopt(a);   // a migrated host keeps the ids it adopted across the reload
    skippedLogged_.clear();
    simCache_.Clear();
    attach_.Clear();
    ghosts_.Clear();
    announced_.clear();
    refused_.clear();
    touched_.clear();
    leftBefore_.clear();
    detached_.clear();
    destroyed_.clear();
    queuedEvents_.clear();
    history_.clear();
    recheckMs_ = eventMs_ = 0;
    shatter_ = ShatterState{};
    baselineMs_ = -1;
    levelWeapons_.clear();
    resetQueue_.clear();
}

// ---------------------------------------------------------------- per frame

void ObjectSync::Tick(double nowMs, const std::vector<std::pair<uint8_t, UObject*>>& owned) {
    if (!active_ || !adapter_) return;
    ++tick_;
    if (!resetQueue_.empty()) StepReset(nowMs);
    if (env_.host)
        for (uint64_t id : table_.Tick(nowMs)) { ++n_.lapsed; Grant(id, kHostSlot, LeaseLevel::Free); }
    DetectDrops(nowMs, owned);
    if (!pending_.empty()) ResolvePending(nowMs);
    if (!late_.empty()) TickLate(nowMs);
    MatchStandIns(nowMs);
    if (!loose_.empty()) TickLoose(nowMs);
    if (env_.fightLevel && env_.fightLevel()) {
        if (levelSinceMs_ < 0) { levelSinceMs_ = nowMs; censusMs_ = nowMs + kCensusFirstMs; recheckMs_ = censusMs_; }
        // A client records where each free prop lies, once, kCensusSnapMs into the level: the census matches on those places.
        if (!env_.host && env_.censusSnapshot && !censusSnapDone_ && nowMs >= levelSinceMs_ + kCensusSnapMs) {
            censusSnapDone_ = true;
            size_t notSimulating = 0;
            for (const LevelActor& a : WalkLevel(notSimulating))
                if (!byActor_.count(a.actor))
                    censusSnap_[LevelKey{a.actor, a.index}] = CensusCandidate{a.classHash, float(a.at.x), float(a.at.y), float(a.at.z)};
        }
        if (env_.host) {
            if (nowMs >= censusMs_) HostCensus(nowMs);
        } else if (census_.HaveCensus() && nowMs >= levelSinceMs_ + kCensusFirstMs &&
                   (!matched_ || census_.Seq() != matchedSeq_ || (bound_.size() < census_.Census().size() && nowMs >= censusMs_))) {
            ClientCensus(nowMs);
            recheckMs_ = nowMs + kCensusRefreshMs;
        } else if (matched_ && nowMs >= recheckMs_) {
            Recheck(nowMs);
        }
    }
    const uint8_t me = MySlot();
    size_t startsRead = 0;                      // level props whose start transform was read this tick (map reset)
    std::vector<uint64_t> dueMoving, dueIdle;   // level props and spawned objects due on the moving schedule, by their last read
    for (auto it = props_.begin(); it != props_.end();) {
        const uint64_t id = it->first;
        Prop& p = it->second;
        if (!adapter_->StillValid(p.actor, p.index)) {   // destroyed here (a respawn takes what its fighter held)
            ++n_.lost;
            if (!p.provisional && p.claim.sent != LeaseLevel::Free) Claim(id, ClaimSend::Release, LeaseLevel::Free, nowMs);
            freeSince_.erase(id);
            // A level prop: the host walks again at once (its census drops it); a client binds that index again when it can.
            if (p.level) { if (env_.host) censusMs_ = nowMs; else bound_.erase(LevelPropIndex(id)); }
            // A spawned object gone from the machine that streams it is gone everywhere (the receivers keep what is held).
            if (p.spawned && !p.provisional && !p.sub && view_.Streamer(id) == me) {
                ObjectEventMsg e;
                e.objId = id;
                e.kind = ObjectEventKind::Destroyed;
                SendEvent(e);
                ++n_.destroyedSent;
                destroyed_.insert(id);
                if (env_.host) reconciler_.Remove(id);
            }
            if (!p.sub) byActor_.erase(p.actor);
            it = props_.erase(it);
            continue;
        }
        if (p.provisional) {   // proposed here, not named yet: nothing streams, claims or drives under a provisional id
            if (nowMs - p.provisionalMs > kProposalWaitMs) {
                ++n_.refused;
                refused_[p.actor] = nowMs + kRefusedKeepMs;
                HSM_LOG("props: the host did not name %s (%016llx); it is not synced", ObjectName(p.actor).c_str(), static_cast<unsigned long long>(id));
                byActor_.erase(p.actor);
                it = props_.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        if (p.level && !p.startTried && startsRead < kStartsPerTick) { ++startsRead; RecordStart(p); }
        UObject* parent = ObjectField(p.actor, 0);
        const uint8_t holder = parent && env_.ownedEntity ? env_.ownedEntity(parent) : 0;
        const bool holding = holder != 0;
        if (holding) p.toucher = holder;
        if (p.restoreTicks > 0) { --p.restoreTicks; if (!parent) RestorePhysics(p.actor, p.root, p.rootIndex); }

        // An object I put on another keeps my lease while it hangs there: I am the one that sees it let go of (PollEvents).
        if (p.attachedSent && p.claim.sent != LeaseLevel::Free && view_.Streamer(id) == MySlot()) p.lastContactMs = nowMs;

        bool moving = false;   // only read when it decides anything: a let-go claim of mine waiting for rest
        if (!holding && p.claim.sent != LeaseLevel::Free && nowMs - p.lastContactMs >= kContactKeepMs) {
            BodyStateMsg s;
            moving = Read(p, 0, s) && !PropAtRest(s);
        }
        LeaseLevel want = LeaseLevel::Free;
        // TOUCH leasing as in 0.8.0-pre7: whoever's fighter bumps, kicks or pushes a prop simulates it until it rests. Rev 32
        // gave every loose prop to the host instead, so a client's own bumps and kicks lagged a round trip (KNOWN_ISSUES.md).
        // A structure (soft or local) is leased by nobody (hsm_structure.h).
        const ClaimSend cs = OwnCopy(id, p) ? ClaimSend::None : StepClaim(p.claim, holding, p.lastContactMs, moving, nowMs, want);
        if (cs != ClaimSend::None) Claim(id, cs, want, nowMs);

        const uint8_t streamer = view_.Streamer(id);
        const LeaseLevel level = view_.Level(id);
        const bool denied = holding && streamer != me && level == LeaseLevel::Hold;
        if (denied && !p.denied) ++n_.holdDenied;
        p.denied = denied;

        if (streamer == me) {
            p.dirty = false;
            for (BodyRef& b : p.bodies) b.dirty = false;
            SetFollowing(p, false, nowMs);
            // A lease of mine I have just released streams no more (the host would drop it until its FREE Grant lands).
            const bool streaming = !p.attachedSent && (level == LeaseLevel::Free || p.claim.sent != LeaseLevel::Free);
            if (streaming && Kept(id, p)) {   // never streamed (hsm_structure.h): every machine simulates its own copy
                if (PropStreamDue(level, p.lastSentMs, false, nowMs)) { ++n_.structStatesSkipped; p.lastSentMs = nowMs; }
            } else if (streaming && (Soft(id, p) ? StructureStreamDue(p.lastSentMs, false, nowMs)    // a soft structure: 20 Hz while it moves
                                                 : PropStreamDue(level, p.lastSentMs, false, nowMs))) {   // due on the moving schedule: worth a read
                if (!p.level && !p.spawned) StreamState(id, p, level, me, nowMs);
                else (p.moving ? dueMoving : dueIdle).push_back(id);
            }
        } else if (p.dirty) {
            p.dirty = false;
            const bool awaitingGrant = p.claim.sent != LeaseLevel::Free && level == LeaseLevel::Free;
            if (p.shapeDiffers) {   // body N here is not body N on the host (MarkFound): left to the physics here
                for (BodyRef& b : p.bodies) b.dirty = false;
            } else if (!parent && !awaitingGrant && !p.attachedSent) {
                if (!p.bodies.empty()) p.bodies[0].dirty = true;   // p.dirty is an explicit redrive of the root (and the state heard)
                if ((Soft(id, p) ? SoftDrive(id, p, nowMs) : Drive(id, p)) && (p.level || p.spawned)) ++n_.levelDriven;
            }
        }
        ++it;
    }
    size_t budget = kMovingReadsPerTick;
    StreamLevel(dueMoving, movingCursor_, budget, true, me, nowMs);
    const size_t movingReads = kMovingReadsPerTick - budget;
    budget = movingReads < kLevelReadsPerTick ? kLevelReadsPerTick - movingReads : 0;
    StreamLevel(dueIdle, idleCursor_, budget, false, me, nowMs);
    if (nowMs >= eventMs_) { eventMs_ = nowMs + kEventPollMs; PollEvents(nowMs); ShatterPoll(nowMs); }
    if (idleDespawn_ && nowMs >= idleCheckMs_) { idleCheckMs_ = nowMs + kIdleCheckMs; DespawnIdle(nowMs); }
}

// Read an object I stream, and send the state of each simulating body when it is due by that read (at rest: the slow
// schedule). Returns the bodies read.
size_t ObjectSync::StreamState(uint64_t propId, Prop& p, LeaseLevel level, uint8_t me, double nowMs) {
    size_t reads = 0;
    std::vector<BodyStateMsg> out;
    bool atRest = true;
    for (size_t b = 0; b < p.bodies.size(); ++b) {
        BodyRef& body = p.bodies[b];
        if (body.detached) continue;
        // A part the game destroyed (a crossbow's bolt representation, a spent quiver bolt) leaves this body a stale pointer
        // while the actor lives (#608): checked before anything dereferences it, every body including the root.
        if (!adapter_->StillValid(body.comp, body.index)) {
            if (!body.gone) {
                body.gone = true;
                HSM_LOG("props: %s body %zu is gone; its other bodies stream on", ObjectName(p.actor).c_str(), b);
            }
            continue;
        }
        if (IsGeometryCollection(body.comp)) { p.lastSentMs = nowMs; continue; }   // its breaks travel, not its transform
        if (b > 0) {   // the root is always streamed (as before); the others only while they simulate physics
            RootCalls* c = CallsFor(body.comp);
            if (!c) continue;
            c->simulating.put(0, FName{});
            if (!c->simulating.invoke(body.comp) || !c->simulating.take<bool>(1)) continue;
        }
        BodyStateMsg s;
        ++reads;
        if (!Read(p, b, s)) continue;
        if (!PropAtRest(s)) atRest = false;
        s.objId = propId;
        s.body = uint8_t(b);
        s.lessee = me;
        s.senderTimeMs = uint32_t(uint64_t(nowMs));
        out.push_back(s);
    }
    p.moving = !atRest;
    // The host's own reads keep the reconciler's place and velocity of a spawned object current, as a client's states do
    // (OnBodyState): a proposal that reaches the host later is matched where the object has moved to.
    if (env_.host && !p.sub && IsSpawnedId(propId))
        for (const BodyStateMsg& s : out)
            if (s.body == 0) reconciler_.UpdateMotion(propId, s.px, s.py, s.pz, s.vx, s.vy, s.vz, nowMs);
    // A soft structure goes out as one snapshot: every body read this tick, in one go, at 20 Hz while any of them moves.
    const double period = Soft(propId, p) ? StructureStreamPeriodMs(atRest) : PropStreamPeriodMs(level, atRest);
    if (out.empty() || nowMs - p.lastSentMs < period) return reads;
    for (const BodyStateMsg& s : out)
        if (env_.send) { env_.send(Encode(s), false); ++n_.statesSent; }
    p.lastSentMs = NextSentMs(p.lastSentMs, period, nowMs);   // the remainder carries: the rate holds at any frame rate
    if (p.level || p.spawned) ++n_.levelStreamed;
    return reads;
}

// Level props and spawned objects due a read, in id order: as many as the budget allows (in bodies; an object is always
// served whole, and the first of a tick is served whatever its size), round robin from the one after cursor so each gets
// its turn however many there are. Those left over wait for a later tick; for objects that were moving (countDeferred)
// that is a stream falling behind, counted in levelDeferred (one at rest only waits for its next once-a-second send).
void ObjectSync::StreamLevel(const std::vector<uint64_t>& due, uint64_t& cursor, size_t& budget, bool countDeferred, uint8_t me,
                           double nowMs) {
    if (due.empty()) return;
    const size_t start = size_t(std::upper_bound(due.begin(), due.end(), cursor) - due.begin());
    size_t done = 0;
    for (; done < due.size() && budget > 0; ++done) {
        const uint64_t id = due[(start + done) % due.size()];
        cursor = id;
        size_t used = 1;
        if (auto it = props_.find(id); it != props_.end()) used = std::max<size_t>(1, StreamState(id, it->second, view_.Level(id), me, nowMs));
        budget = used >= budget ? 0 : budget - used;
    }
    if (countDeferred) n_.levelDeferred += due.size() - done;
}

// The movable primitive components of an actor in their order (Actor::K2_GetComponentsByClass); cls filters the class.
std::vector<UObject*> ObjectSync::ComponentsOf(UObject* actor, const UClass* cls) {
    std::vector<UObject*> out;
    if (!actor || !cls) return out;
    if (!components_.valid())
        if (UClass* actorClass = FindClass("Actor")) components_ = PreparedCall(actorClass, "K2_GetComponentsByClass", {"ComponentClass", "ReturnValue"});
    if (!components_.valid() || !components_.put(0, cls).invoke(actor)) return out;
    const FObjArray arr = components_.take<FObjArray>(1);
    if (!arr.data || arr.num <= 0 || arr.num > 512) return out;
    out.assign(arr.data, arr.data + arr.num);
    return out;
}

bool ObjectSync::MovableComp(UObject* comp) {
    if (!comp || IsGarbage(comp)) return false;
    const UClass* cls = ClassOf(comp);
    auto it = mobilityOff_.find(cls);
    if (it == mobilityOff_.end()) {
        FProperty* mp = FindProperty(ClassOf(comp), "Mobility");
        it = mobilityOff_.emplace(cls, mp && PropertySize(mp) == 1 ? PropertyOffset(mp) : -1).first;
    }
    return IsGeometryCollection(comp) || (it->second >= 0 && *(reinterpret_cast<const uint8_t*>(comp) + it->second) == kMovable);
}

// An actor class that is never an object: gore, blood, decals, effects, Niagara and particle actors, effect projectiles and the
// mod's own helper actors (session/hsm_object_skip.h), by its name or any class it derives from. Logged once per level.
bool ObjectSync::ClassSkipped(const UClass* actorClass) {
    int depth = 0;
    auto nameOf = [](const UClass* c) { return ObjectName(reinterpret_cast<const UObject*>(c)); };
    for (const UClass* c = actorClass; c && depth < 32; c = SuperOf(c), ++depth) {
        const std::string name = nameOf(c);
        if (!ObjectClassSkipped(name.c_str())) continue;
        ++n_.skippedClasses;
        const std::string own = nameOf(actorClass);
        if (skippedLogged_.insert(own).second)
            HSM_LOG("objects: class %s is never synced (%s%s)", own.c_str(), c == actorClass ? "effect, gore or mod actor" : "derives from ",
                    c == actorClass ? "" : name.c_str());
        return true;
    }
    return false;
}

// The bodies of an object (session/hsm_object.h BodyIndex): the root first when it is a movable primitive, then the actor's
// other movable primitives in their order. A dropped weapon (multi false) is its root alone, as it always was, unless
// it has simulating parts of its own (0.8.0-pre7: a flail's chain and head are primitives of the weapon actor, not its
// root; streamed as the root alone, the head lay wherever the far machine's physics left it, #78).
bool ObjectSync::BuildBodies(Prop& p, bool multi) {
    Call rc(p.actor, "K2_GetRootComponent");
    UObject* root = rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr;
    const bool rootBody = root && MovableComp(root) && CallsFor(root);
    if (!multi && LooseParts(p.actor, root)) {
        multi = true;
        HSM_LOG("props: %s has simulating parts; every body is streamed", ObjectName(p.actor).c_str());
    }
    if (!multi) {
        if (!rootBody) return false;
        p.bodies.assign(1, BodyRef{});
        p.bodies[0].comp = root;
    } else {
        if (!primitiveClass_) primitiveClass_ = FindClass("PrimitiveComponent");
        if (!constraintClass_) constraintClass_ = FindClass("PhysicsConstraintComponent");
        std::vector<const UObject*> comps;
        for (UObject* c : ComponentsOf(p.actor, primitiveClass_))
            if (c != root && MovableComp(c) && CallsFor(c)) comps.push_back(c);
        const UObject* first = rootBody ? root : (comps.empty() ? nullptr : comps.front());
        if (!first) return false;
        BodyIndex<const UObject*> idx;
        idx.Build(first, comps);
        p.bodies.clear();
        for (size_t i = 0; i < idx.Count(); ++i) {
            BodyRef b;
            b.comp = const_cast<UObject*>(*idx.KeyAt(uint8_t(i)));
            b.index = ObjectIndex(b.comp);
            p.bodies.push_back(b);
        }
        for (UObject* c : ComponentsOf(p.actor, constraintClass_)) {
            if (p.constraints.size() > kMaxConstraintIndex) break;
            p.constraints.push_back(ConstraintRef{c, ObjectIndex(c)});
        }
        for (size_t i = 1; i < p.bodies.size(); ++i) {   // which bodies hang on a parent now: one that lets go is a Detached
            Call ap(p.bodies[i].comp, "GetAttachParent");
            if (ap.invoke() && ap.get<UObject*>("ReturnValue")) p.attachedMask |= uint64_t(1) << i;
        }
    }
    p.rootBody = rootBody;
    p.bodies[0].index = ObjectIndex(p.bodies[0].comp);
    p.root = p.bodies[0].comp;
    p.rootIndex = p.bodies[0].index;
    return true;
}

bool ObjectSync::LooseParts(UObject* actor, UObject* root) {
    if (!primitiveClass_) primitiveClass_ = FindClass("PrimitiveComponent");
    for (UObject* c : ComponentsOf(actor, primitiveClass_)) {
        if (c == root || !MovableComp(c) || !CallsFor(c)) continue;
        Call sim(c, "IsSimulatingPhysics");
        if (sim.valid() && sim.invoke() && sim.get<bool>("ReturnValue")) return true;
    }
    return false;
}

ObjectSync::Prop* ObjectSync::Track(uint64_t propId, UObject* actor) {
    if (props_.count(propId) || byActor_.count(actor)) return nullptr;
    // A Capture the flag flag: every machine spawns and places its own from MatchInfo, so it is never streamed, announced,
    // reset or cleared as a loose object (Adapter::IsModActor).
    if (adapter_ && adapter_->IsModActor(actor)) return nullptr;
    Prop probe;
    probe.actor = actor;
    probe.index = ObjectIndex(actor);
    if (!BuildBodies(probe, !IsWeaponId(propId))) {
        ++n_.readFailed;
        HSM_LOG("props: %s has no physics body; %016llx is not synced", ObjectName(actor).c_str(), static_cast<unsigned long long>(propId));
        return nullptr;
    }
    probe.classHash = ClassHashOf(actor);
    probe.level = IsLevelProp(propId);
    probe.spawned = IsSpawnedId(propId);
    probe.bornMs = SteadyNowMs();
    Prop& p = props_[propId];
    p = std::move(probe);
    byActor_[actor] = propId;
    if (IsSpawnedId(propId)) ghosts_.Resolve(propId);
    return &p;
}

// The stand-in rig kept its weapons gravity-free and blind to world geometry, fighters and loose bodies
// (FighterRig::SetBodyPolicy); a weapon it let go of falls and lands like any other (Block on WorldStatic, WorldDynamic,
// Pawn and PhysicsBody, the weapon's default responses). 0.8.0-pre7: every movable primitive of it, a flail's chain too,
// and the Pawn and PhysicsBody responses as well: a dropped weapon passed through fighters and other weapons (#68).
void ObjectSync::RestorePhysics(UObject* actor, UObject* root, int32_t rootIndex) {
    if (!adapter_->StillValid(root, rootIndex)) return;
    if (!primitiveClass_) primitiveClass_ = FindClass("PrimitiveComponent");
    std::vector<UObject*> comps = actor ? ComponentsOf(actor, primitiveClass_) : std::vector<UObject*>{};
    if (std::find(comps.begin(), comps.end(), root) == comps.end()) comps.push_back(root);
    for (UObject* c : comps) {
        if (c != root && !MovableComp(c)) continue;
        Call(c, "SetEnableGravity").set<bool>("bGravityEnabled", true).invoke();
        for (uint8_t ch : {kWorldStatic, kWorldDynamic, kPawn, kPhysicsBody})
            Call(c, "SetCollisionResponseToChannel").set<uint8_t>("Channel", ch).set<uint8_t>("NewResponse", kBlock).invoke();
    }
}

// ---------------------------------------------------------------- leases

bool ObjectSync::InUse(uint64_t propId, double nowMs) const {
    if (props_.count(propId) || pending_.count(propId) || view_.Level(propId) != LeaseLevel::Free) return true;
    auto it = heardMs_.find(propId);
    return it != heardMs_.end() && nowMs - it->second < kHeardLiveMs;
}

void ObjectSync::Claim(uint64_t propId, ClaimSend what, LeaseLevel level, double nowMs) {
    const uint8_t me = MySlot();
    PropLeaseMsg m;
    m.propId = propId;
    m.slot = me;
    if (what == ClaimSend::Request) {
        ++n_.requests;
        if (env_.host) { HostRequest(propId, me, level, nowMs); return; }
        m.action = LeaseAction::Request;
        m.level = level;
    } else if (what == ClaimSend::Release) {
        ++n_.releases;
        if (env_.host) { HostRelease(propId, me); return; }
        m.action = LeaseAction::Release;
        m.level = LeaseLevel::Free;
    } else {
        return;
    }
    if (env_.send) env_.send(Encode(m), true);
}

void ObjectSync::HostRequest(uint64_t propId, uint8_t slot, LeaseLevel level, double nowMs) {
    const uint64_t deniedBefore = table_.CapDenied();
    const LeaseResult r = table_.Request(propId, slot, level, nowMs);
    if (table_.CapDenied() != deniedBefore && MsgBudget::ShouldLog(table_.CapDenied()))   // M4: counted, logged sparingly
        HSM_LOG("props: slot %u already holds %d leases; the request for %016llx is denied (%llu denied so far)", slot, kMaxLeasesPerSlot,
                static_cast<unsigned long long>(propId), static_cast<unsigned long long>(table_.CapDenied()));
    if (r.changed) Grant(propId, r.slot, r.level);
}

void ObjectSync::HostRelease(uint64_t propId, uint8_t slot) {
    if (table_.Release(propId, slot).changed) Grant(propId, kHostSlot, LeaseLevel::Free);
}

void ObjectSync::Grant(uint64_t propId, uint8_t slot, LeaseLevel level) {
    PropLeaseMsg g;
    g.propId = propId;
    g.action = LeaseAction::Grant;
    g.level = level;
    g.slot = level == LeaseLevel::Free ? kHostSlot : slot;
    ++n_.grants;
    if (env_.send) env_.send(Encode(g), true);
    OnGrant(propId, g.slot, level);
}

// A lease change from the owner of a stand-in that holds this prop's copy is the owner letting go of it (DropNoticed):
// the copy leaves the stand-in's hand now, not once the owner's pose has shown the hand empty and the hand matching's
// holds are over (the last shown class for sync::kReattachMinMs, then the retry backoff -- test 6: seconds late, the copy
// glued to the hand and undriven meanwhile). MatchHand stays as the fallback for a drop with no lease change. The host
// answers changes only, so every change reaches OnGrant, on the host too (Grant applies its own), and a renewal never does.
// OnGrant calls it with the lease as the view had it before the Grant, ahead of its early return for tracked ids.
void ObjectSync::NoticeDrop(uint64_t propId, LeaseLevel prevLevel, uint8_t prevSlot, LeaseLevel level, uint8_t slot) {
    auto known = props_.find(propId);
    if (known == props_.end() || !standIns_) return;
    UObject* copy = known->second.actor;
    if (!adapter_->StillValid(copy, known->second.index)) return;
    for (uint8_t e = 1; e <= kMaxWillies; ++e) {
        UObject* standIn = standIns_->ActorOf(e);
        if (!standIn) continue;
        for (int h = 0; h < 2; ++h) {
            if (ObjectField(standIn, h == 0 ? 1 : 2) != copy) continue;   // held in both hands: the right's pass lets both go
            if (!DropNoticed(prevLevel, prevSlot, level, slot, OwnerSlotOf(e), true)) return;
            const double nowMs = SteadyNowMs();
            StandIns::PoseHandView pose;
            const bool havePose = standIns_->PoseHandInfo(e, h, pose);   // the episode this hand lets go in, before the pose moves on
            const DropResult r = DropFromHands(standIn, h, copy, ObjectName(copy));
            if (r == DropResult::Dropped) {
                ++n_.leaseReleased;
                HSM_LOG("props: stand-in of e%u let go of %s from hand %d: its owner's lease changed", e, ObjectName(copy).c_str(), h);
                LetLoose(e, h, copy, nowMs);
                StandInHand& sh = standInHands_[e].hand[size_t(h)];
                sh.leaseLetGo = havePose;
                sh.leaseLetGoPoseMs = pose.sinceMs;
                if (env_.handsChanged) env_.handsChanged(e);
                ForgetHeld(e, standIn);
            }
            return;
        }
    }
}

// A drop heard of opens its one resolve window, with what that hand of the stand-in holds now as its witness.
void ObjectSync::OnGrant(uint64_t propId, uint8_t slot, LeaseLevel level) {
    const LeaseLevel prevLevel = view_.Level(propId);
    const uint8_t prevSlot = view_.Streamer(propId);
    const bool known = view_.Known(propId);   // before this Grant makes it known: only an id's first Grant can be its drop
    view_.OnGrant(propId, slot, level);
    attach_.OnGrant(propId, level == LeaseLevel::Free ? kHostSlot : slot, level);   // another holder ends an attach latch
    if (level != LeaseLevel::Free) freeSince_.erase(propId);
    else if (IsWeaponId(propId)) freeSince_.emplace(propId, SteadyNowMs());   // the first FREE Grant starts the minute
    NoticeDrop(propId, prevLevel, prevSlot, level, level == LeaseLevel::Free ? kHostSlot : slot);
    if (!IsWeaponId(propId) || slot == MySlot() || props_.count(propId) || pending_.count(propId) || !DropSignature(propId, level, slot, known)) return;
    const double now = SteadyNowMs();
    if (auto g = givenUp_.find(propId); g != givenUp_.end()) {
        if (now - g->second < kLooseKeepMs) return;   // given up already: that drop had its window
        givenUp_.erase(g);
    }
    PendingDrop& d = pending_[propId];
    d.heardMs = now;
    d.deadlineMs = now + kResolveWindowMs;
    d.witness = UntrackedInHand(standIns_->ActorOf(PropOwnerEntity(propId)), PropHand(propId));
    d.witnessIndex = d.witness ? ObjectIndex(d.witness) : -1;
    StandIns::PoseHandView pose;   // the class the owner's hand shows (or showed just before): GiveUp's late match needs it
    if (standIns_->PoseHandInfo(PropOwnerEntity(propId), PropHand(propId), pose))
        d.heardClass = pose.classHash != sync::kHandEmpty && pose.classHash != sync::kHandUnknown ? pose.classHash : pose.prevClassHash;
}

bool ObjectSync::OnBodyState(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return false;
    BodyStateMsg m;
    if (!Decode(d, n, m) || (IsLevelProp(m.objId) && !InCensus(m.objId))) { ++n_.statesRejected; return false; }
    // A spawned object is one the host announced (or a body that detached from one): any other id is garbage.
    if (env_.host && IsSpawnedId(m.objId) && !reconciler_.Knows(m.objId) && !detached_.count(m.objId)) { ++n_.statesRejected; return false; }
    if (env_.host && !table_.Accepts(m, fromSlot)) { ++n_.statesRejected; return false; }
    if (!env_.host && m.lessee == MySlot()) { ++n_.statesRejected; return false; }   // never mine back
    // Mesh v2: a state straight from another client must be that client's, for a prop the host granted it.
    if (!env_.host && fromSlot != kHostSlot && (m.lessee != fromSlot || view_.Streamer(m.objId) != fromSlot)) { ++n_.statesRejected; return false; }
    heardMs_[m.objId] = SteadyNowMs();
    // An object put on another object is held there against the attacher's states still in flight (hsm_object_attach.h): a
    // stale state must not read as "streamed again" and take it off its parent. Not applied, and not relayed.
    if (attach_.Blocks(m.objId, m.lessee, SteadyNowMs())) { ++n_.attachHeld; return false; }
    if (!view_.Apply(m)) { ++n_.statesStale; return false; }
    ++n_.statesApplied;
    if (auto it = props_.find(m.objId); it != props_.end()) {
        Prop& p = it->second;
        if (p.attachedSent && !p.provisional) {   // streamed again: it was let go of where it was attached
            p.attachedSent = false;
            if (adapter_->StillValid(p.actor, p.index))
                Call(p.actor, "K2_DetachFromActor").set<uint8_t>("LocationRule", 1).set<uint8_t>("RotationRule", 1).set<uint8_t>("ScaleRule", 1).invoke();
        }
        p.dirty = true;
        if (m.body < p.bodies.size()) { p.bodies[m.body].dirty = true; p.bodies[m.body].rxMs = SteadyNowMs(); }
    }
    // A late proposal finds it where it is: its place and velocity, read at this moment (the proposal is judged by the same clock).
    if (env_.host && m.body == 0 && IsSpawnedId(m.objId)) reconciler_.UpdateMotion(m.objId, m.px, m.py, m.pz, m.vx, m.vy, m.vz, SteadyNowMs());
    return true;
}

void ObjectSync::OnPropLease(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return;
    PropLeaseMsg m;
    // A level prop's index must be in the current census, and nobody holds a level prop or a spawned object (HOLD is for
    // weapons in hand); the host leases a spawned id only when it announced it (or a body detached from one).
    if (!Decode(d, n, m) || (IsLevelProp(m.propId) && (!InCensus(m.propId) || m.level == LeaseLevel::Hold)) ||
        (IsSpawnedId(m.propId) && (m.level == LeaseLevel::Hold ||
                                   (env_.host && !reconciler_.Knows(m.propId) && !detached_.count(m.propId))))) {
        ++n_.leasesRejected;
        return;
    }
    if (env_.host) {
        if (!LeaseFromPeerOk(m, fromSlot)) { ++n_.leasesRejected; return; }
        if (m.action == LeaseAction::Request) HostRequest(m.propId, m.slot, m.level, SteadyNowMs());
        else HostRelease(m.propId, m.slot);
        return;
    }
    if (m.action != LeaseAction::Grant || fromSlot != kHostSlot) { ++n_.leasesRejected; return; }
    OnGrant(m.propId, m.slot, m.level);
}

void ObjectSync::SlotLeft(uint8_t slot) {
    if (!active_ || !env_.host) return;
    for (uint64_t id : table_.EntityLeft(slot)) Grant(id, kHostSlot, LeaseLevel::Free);
}

std::vector<std::vector<uint8_t>> ObjectSync::GrantsForJoiner() const {
    std::vector<std::vector<uint8_t>> out;
    if (!active_ || !env_.host) return out;
    for (const PropLeaseMsg& g : table_.Grants()) out.push_back(Encode(g));
    return out;
}

// ---------------------------------------------------------------- level props (session/hsm_prop_census.h)

std::vector<std::vector<uint8_t>> ObjectSync::CensusForJoiner() const {
    std::vector<std::vector<uint8_t>> out;
    if (!active_ || !env_.host || !haveSent_) return out;
    for (const PropCensusMsg& m : SplitCensus(censusSeq_, sent_)) out.push_back(Encode(m));
    return out;
}

bool ObjectSync::OnPropCensus(const uint8_t* d, size_t n) {
    if (!active_ || env_.host) return false;
    PropCensusMsg m;
    if (!Decode(d, n, m)) { ++n_.censusRejected; return false; }
    switch (census_.Add(m)) {
        case CensusAssembler::Result::Rejected: ++n_.censusRejected; return false;
        case CensusAssembler::Result::Complete:   // matched in Tick; from now on only its indices are heard
            ++n_.censuses;
            censusIndex_.clear();
            for (const CensusEntry& e : census_.Census()) censusIndex_.insert(e.index);
            return true;
        default: return true;                     // a part of one still coming, or of one already replaced
    }
}

bool ObjectSync::InCensus(uint64_t propId) const {
    return IsCanonicalLevelPropId(propId) && censusIndex_.count(LevelPropIndex(propId)) != 0;
}

// The physics objects of the level here: actors that are no fighter and no weapon, attached to nothing and not being
// destroyed, with at least one movable primitive component that simulates physics (an actor none of whose movable
// primitives simulate is counted in notSimulating). One walk over every object (the WeaponActors pattern): each movable
// primitive component is credited to its owning actor, so a chain whose links are children of a scene root is found by
// its links. What each class is, and where its mobility lies, is read once.
std::vector<ObjectSync::LevelActor> ObjectSync::WalkLevel(size_t& notSimulating) {
    std::vector<LevelActor> out;
    notSimulating = 0;
    static UClass* actorClass = nullptr;
    if (!actorClass) actorClass = FindClass("Actor");
    if (!primitiveClass_) primitiveClass_ = FindClass("PrimitiveComponent");
    if (!actorClass || !primitiveClass_) return out;
    constexpr size_t kRejected = size_t(-1);
    std::unordered_map<const UObject*, size_t> seen;   // an owning actor -> its place in acc, or kRejected
    std::vector<LevelActor> acc;
    // The cost of a walk is the reflected IsSimulatingPhysics call per component, so its answer is kept per component for a
    // while (hsm_walk_cache.h; longest for a component of an object tracked here and at rest), and a pass asks at most
    // kWalkQueriesPerPass fresh ones (the rest answer from what was last known, or "not simulating", and are asked on later passes).
    const double startMs = SteadyNowMs();
    if (startMs >= simPruneMs_) { simPruneMs_ = startMs + 10000.0; simCache_.Prune(startMs); }
    size_t comps = 0, asked = 0, cached = 0, capped = 0;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o)) continue;
        const UClass* cls = ClassOf(o);
        auto kind = classKind_.find(cls);
        if (kind == classKind_.end()) kind = classKind_.emplace(cls, uint8_t(IsChildOf(cls, primitiveClass_))).first;   // 1: a primitive component class
        if (!kind->second || !MovableComp(o)) continue;
        UObject* actor = OuterOf(o);
        if (!actor || IsDefaultObject(actor) || IsGarbage(actor)) continue;
        auto st = seen.find(actor);
        if (st == seen.end()) {
            size_t slot = kRejected;
            const UClass* ac = ClassOf(actor);
            auto ak = actorKind_.find(ac);
            if (ak == actorKind_.end()) ak = actorKind_.emplace(ac, uint8_t(!IsChildOf(ac, actorClass) ? 0 : ClassSkipped(ac) ? 2 : 1)).first;
            if (ak->second == 1 && !adapter_->IsWillie(actor) && !adapter_->IsWeapon(actor) && !Adapter::BeingDestroyed(actor) && !ParentActorOf(actor)) {
                slot = acc.size();
                LevelActor la;
                la.actor = actor;
                la.index = ObjectIndex(actor);
                la.classHash = ClassHashOf(actor);
                la.at = adapter_->ActorLocation(actor);
                if (auto tracked = byActor_.find(actor); tracked != byActor_.end())
                    if (auto tp = props_.find(tracked->second); tp != props_.end()) la.asleep = !tp->second.moving && !tp->second.provisional;
                acc.push_back(la);
            }
            st = seen.emplace(actor, slot).first;
        }
        if (st->second == kRejected) continue;
        LevelActor& la = acc[st->second];
        RootCalls* c = CallsFor(o);
        if (!c) continue;
        if (la.bodies < kMaxObjectBodies) ++la.bodies;
        ++comps;
        if (la.anySim) continue;
        bool sim = false;
        const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(o));
        const int32_t oi = ObjectIndex(o);
        if (simCache_.Lookup(key, oi, la.asleep, startMs, sim)) {
            ++cached;
        } else if (asked < kWalkQueriesPerPass) {
            ++asked;
            c->simulating.put(0, FName{});
            sim = c->simulating.invoke(o) && c->simulating.take<bool>(1);
            simCache_.Store(key, oi, sim, startMs);
        } else {
            ++capped;
            if (!simCache_.Stale(key, oi, sim)) sim = false;   // never asked: on a later pass
        }
        if (sim || IsGeometryCollection(o)) { la.anySim = true; la.simComp = o; }   // a geometry collection is synced by its breaks (shatter_sync.cpp)
    }
    idle_.clear();
    for (LevelActor& la : acc) {
        if (la.bodies == 0) continue;
        if (la.anySim) out.push_back(la);
        else { ++notSimulating; idle_.push_back(Candidate{la.actor, la.index, la.classHash}); }
    }
    n_.simAsked += asked; n_.simCached += cached; n_.simCapped += capped;
    // The walk's cost, every census (the cache and the cap are tuned from this line).
    if (logWalks_)
        HSM_LOG("objects: walk %zu comps, %zu actors, %.1f ms (%zu asked, %zu cached, %zu capped)", comps, acc.size(), SteadyNowMs() - startMs,
                asked, cached, capped);
    return out;
}

std::vector<ObjectSync::WalkedObject> ObjectSync::WalkForRecorder(Adapter& adapter) {
    std::vector<WalkedObject> out;
    if (active_) return out;   // a session's own ObjectSync is never walked for the recorder
    adapter_ = &adapter;
    logWalks_ = false;
    size_t notSimulating = 0;
    const std::vector<LevelActor> walk = WalkLevel(notSimulating);
    for (const LevelActor& la : walk) {
        WalkedObject w;
        w.actor = la.actor;
        w.index = la.index;
        w.bodies = RecorderBodies(adapter, la.actor);
        if (!w.bodies.empty()) out.push_back(std::move(w));
    }
    return out;
}

std::vector<UObject*> ObjectSync::RecorderBodies(Adapter& adapter, UObject* actor) {
    std::vector<UObject*> out;
    if (active_ || !actor) return out;
    adapter_ = &adapter;
    if (!primitiveClass_) primitiveClass_ = FindClass("PrimitiveComponent");
    Call rc(actor, "K2_GetRootComponent");
    UObject* root = rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr;
    if (root && MovableComp(root) && CallsFor(root)) out.push_back(root);
    if (!primitiveClass_) return out;
    for (UObject* comp : ComponentsOf(actor, primitiveClass_))
        if (comp != root && out.size() < kMaxObjectBodies && MovableComp(comp) && CallsFor(comp)) out.push_back(comp);
    return out;
}

// An actor's attach parent actor, nullptr when it hangs on nothing (a fighter's armour, a weapon's part).
UObject* ObjectSync::ParentActorOf(UObject* actor) {
    Call parent(actor, "GetAttachParentActor");
    return parent.invoke() ? parent.get<UObject*>("ReturnValue") : nullptr;
}

// Host: walk, let the book name what it found, track the new ones, forget the gone ones, and send the census when its set
// changed (and the first time, empty or not: a client must hear that too). After the first census a new object is not a
// census entry: it is announced as a spawned object (HostSpawn), so the census only ever shrinks and a client's
// matched indices stay what they were.
void ObjectSync::HostCensus(double nowMs) {
    censusMs_ = nowMs + kCensusRefreshMs;
    ++n_.rechecks;
    size_t notSimulating = 0;
    const std::vector<LevelActor> walk = WalkLevel(notSimulating);
    const bool first = !haveSent_;
    std::vector<CensusBook<LevelKey>::Seen> seen;
    seen.reserve(walk.size());
    std::vector<LevelActor> unnamed;   // after the first census: not a census prop, tracked by nothing
    for (const LevelActor& a : walk) {
        uint16_t known = 0;
        if (!first && !book_.Find(LevelKey{a.actor, a.index}, known)) {
            if (!byActor_.count(a.actor)) unnamed.push_back(a);
            continue;
        }
        seen.push_back({LevelKey{a.actor, a.index}, a.classHash, float(a.at.x), float(a.at.y), float(a.at.z)});
    }
    if (!first) {
        BindAdopted(unnamed, nowMs);     // objects the previous host announced (host migration): same ids, this host's actors
        BindAnnounced(unnamed, nowMs);   // a client's object the host has named and not found a copy of yet
        size_t spawns = 0;
        for (const LevelActor& a : unnamed)
            if (!byActor_.count(a.actor) && spawns < kSpawnsPerPass) { ++spawns; HostSpawn(a, nowMs); }
    }
    if (!book_.Update(seen) && haveSent_) return;
    const std::vector<CensusEntry> entries = book_.Entries();
    std::set<uint16_t> live;
    for (const CensusEntry& e : entries) live.insert(e.index);
    size_t gone = 0, added = 0;
    for (uint16_t index : censusIndex_) {
        if (live.count(index)) continue;
        ++gone;
        if (auto p = props_.find(MakeLevelPropId(index)); p != props_.end()) {
            // A claim this machine still holds on a prop that left the census would otherwise never be given back
            if (p->second.claim.sent != LeaseLevel::Free) Claim(p->first, ClaimSend::Release, LeaseLevel::Free, nowMs);
            byActor_.erase(p->second.actor);
            props_.erase(p);
        }
    }
    for (const LevelActor& a : walk) {
        uint16_t index = 0;
        if (!book_.Find(LevelKey{a.actor, a.index}, index) || censusIndex_.count(index)) continue;
        ++added;
        if (Prop* p = Track(MakeLevelPropId(index), a.actor)) p->level = true;
    }
    censusIndex_ = std::move(live);
    sent_ = entries;
    ++censusSeq_;
    haveSent_ = true;
    ++n_.censuses;
    for (const PropCensusMsg& m : SplitCensus(censusSeq_, sent_))
        if (env_.send) env_.send(Encode(m), true);
    if (n_.censuses <= kLogFirst)
        HSM_LOG("props: level census %zu physics props (%zu movable not simulating, %zu left out), seq %u (+%zu -%zu)",
                sent_.size(), notSimulating, book_.LeftOut(), censusSeq_, added, gone);
}

// Client: bind the census's indices to this machine's own physics props. A bound index stays bound while the census
// still has it as it was matched and its actor lives; the others are matched (MatchCensus) against the props here that
// are not synced yet. In the first match of a level, a prop here tagged 'Random Delete' that matched nothing is one the
// host's level manager purged (its RandomBool differs per machine): destroyed, so both sides keep the host's set. What is
// left over is looked at for announces and proposals (AfterWalk). Each free prop is matched on the place it lay early in the
// level (censusSnap_, when it has one), so a roll or a carry before the match does not take it off its home.
void ObjectSync::ClientCensus(double nowMs) {
    const std::vector<CensusEntry>& census = census_.Census();
    const bool fresh = !matched_ || census_.Seq() != matchedSeq_;
    matched_ = true;
    matchedSeq_ = census_.Seq();
    censusMs_ = nowMs + kCensusRefreshMs;
    ++n_.rechecks;
    std::map<uint16_t, const CensusEntry*> want;
    for (const CensusEntry& e : census) want[e.index] = &e;
    for (auto it = bound_.begin(); it != bound_.end();) {
        const uint64_t id = MakeLevelPropId(it->first);
        const auto w = want.find(it->first);
        const bool same = w != want.end() && w->second->classHash == it->second.classHash && w->second->px == it->second.px &&
                          w->second->py == it->second.py && w->second->pz == it->second.pz;
        auto p = props_.find(id);
        if (same && p != props_.end()) { ++it; continue; }
        if (p != props_.end()) {
            if (w != want.end() && p->second.claim.sent != LeaseLevel::Free) Claim(id, ClaimSend::Release, LeaseLevel::Free, nowMs);
            byActor_.erase(p->second.actor);
            props_.erase(p);
        }
        it = bound_.erase(it);
    }
    size_t notSimulating = 0;
    std::vector<LevelActor> free;   // the physics props here that no census index has yet
    for (LevelActor& a : WalkLevel(notSimulating))
        if (!byActor_.count(a.actor)) free.push_back(a);
    std::vector<CensusEntry> open;
    for (const CensusEntry& e : census)
        if (!bound_.count(e.index)) open.push_back(e);
    std::vector<CensusCandidate> locals;
    locals.reserve(free.size());
    size_t snapped = 0;   // free props that have an early place
    for (const LevelActor& a : free) {
        const CensusCandidate now{a.classHash, float(a.at.x), float(a.at.y), float(a.at.z)};
        const auto snap = censusSnap_.find(LevelKey{a.actor, a.index});
        if (snap != censusSnap_.end()) ++snapped;
        locals.push_back(CensusMatchPlace(now, snap != censusSnap_.end() ? &snap->second : nullptr));
    }
    const std::vector<int> match = MatchCensus(open, locals);
    std::vector<bool> used(free.size(), false);
    size_t bound = 0;
    auto bindOne = [&](const CensusEntry& e, size_t j) {
        used[j] = true;
        const uint64_t id = MakeLevelPropId(e.index);
        Prop* p = Track(id, free[j].actor);
        if (!p) return;
        p->level = true;
        p->dirty = view_.Find(id) != nullptr;
        bound_[e.index] = e;
        ++bound;
    };
    std::vector<CensusEntry> unplaced;   // homes no early place fits
    for (size_t i = 0; i < open.size(); ++i) {
        if (match[i] < 0) unplaced.push_back(open[i]);
        else bindOne(open[i], size_t(match[i]));
    }
    // A prop still rolling at the early place lay far from where it came to rest, so its home is tried on the current places too.
    if (snapped > 0 && !unplaced.empty()) {
        std::vector<CensusCandidate> nowPlaces;
        std::vector<size_t> nowIndex;
        for (size_t j = 0; j < free.size(); ++j) {
            if (used[j]) continue;
            nowPlaces.push_back(CensusCandidate{free[j].classHash, float(free[j].at.x), float(free[j].at.y), float(free[j].at.z)});
            nowIndex.push_back(j);
        }
        const std::vector<int> again = MatchCensus(unplaced, nowPlaces);
        for (size_t i = 0; i < unplaced.size(); ++i)
            if (again[i] >= 0) bindOne(unplaced[i], nowIndex[size_t(again[i])]);
    }
    misses_ = census.size() - bound_.size();
    const bool purge = !purged_;
    purged_ = true;
    size_t purged = 0, unmatched = 0;
    std::vector<LevelActor> left;
    for (size_t j = 0; j < free.size(); ++j) {
        if (used[j]) continue;
        if (purge && TaggedRandomDelete(free[j].actor)) { adapter_->DestroyActor(free[j].actor); ++purged; }
        else { ++unmatched; left.push_back(free[j]); }
    }
    n_.censusPurged += purged;
    if ((fresh && n_.censuses <= kLogFirst) || (!fresh && bound > 0))
        HSM_LOG("props: level census seq %u resolved %zu of %zu (%zu now), purged %zu, miss %zu (%zu here unmatched, %zu movable not simulating, "
                "%zu of %zu free with an early place)",
                census_.Seq(), bound_.size(), census.size(), bound, purged, misses_, unmatched, notSimulating, snapped, free.size());
    AfterWalk(left, nowMs);
}

// Tagged 'Random Delete' (the tag BP_LevelManager's purge looks for): AActor::ActorHasTag. The FName is made once; if it
// cannot be made, nothing counts as tagged and nothing is purged.
bool ObjectSync::TaggedRandomDelete(UObject* actor) {
    if (!randomDeleteKnown_) {
        randomDeleteKnown_ = true;
        randomDelete_ = MakeName("Random Delete");
        if (randomDelete_.index == 0 && randomDelete_.number == 0) HSM_LOG("props: the 'Random Delete' tag could not be named; nothing is purged");
    }
    if (randomDelete_.index == 0 && randomDelete_.number == 0) return false;
    Call c(actor, "ActorHasTag");
    c.set<FName>("Tag", randomDelete_);
    return c.invoke() && c.get<bool>("ReturnValue");
}

void ObjectSync::ForgetEntity(uint8_t entity) {
    if (!active_ || !ValidEntity(entity)) return;
    hands_[entity] = Hands{};
    standInHands_[entity] = StandInHands{};
    for (Loose& l : loose_)
        if (l.entity == entity) l.entity = 0;   // still falls; no drop of that fighter's can be heard of any more
    for (auto it = pending_.begin(); it != pending_.end();)
        it = PropOwnerEntity(it->first) == entity ? pending_.erase(it) : std::next(it);
    for (auto it = givenUp_.begin(); it != givenUp_.end();)
        it = PropOwnerEntity(it->first) == entity ? givenUp_.erase(it) : std::next(it);
    for (auto it = late_.begin(); it != late_.end();)
        it = PropOwnerEntity(it->first) == entity ? late_.erase(it) : std::next(it);
    for (auto& [id, p] : props_)
        if (p.toucher == entity) p.toucher = 0;
}

// ---------------------------------------------------------------- contacts

void ObjectSync::NoteContact(UObject* a, UObject* b) {
    if (!active_ || !a || !b || !env_.ownedEntity) return;
    for (int side = 0; side < 2; ++side) {   // each side that is a prop, touched by the other (itself a prop, if picked up again)
        auto it = byActor_.find(side == 0 ? a : b);
        if (it == byActor_.end()) {
            // An actor nobody names yet that a fighter simulated here touched: the first owner of an object is the machine
            // whose fighter set it moving, so the re-check looks at it now (and a proposal is not held back for it).
            UObject* x = side == 0 ? a : b;
            UObject* other = side == 0 ? b : a;
            // A floor or a wall is looked at once and remembered as no candidate (-1); the map is pruned by the re-check.
            if (touched_.size() < 256 && !touched_.count(x) && (adapter_->IsWillie(other) || adapter_->IsWeapon(other)) &&
                !adapter_->IsWillie(x) && !adapter_->IsWeapon(x) && !byActor_.count(other)) {
                UObject* fighter = adapter_->IsWillie(other) ? other : ObjectField(other, 0);
                if (fighter && env_.ownedEntity(fighter)) {
                    // Blood, gore, decals, effects and the mod's own actors are never objects, whatever touched them.
                    static UClass* actorBase = nullptr;
                    if (!actorBase) actorBase = FindClass("Actor");
                    if (actorBase) {
                        const UClass* xc = ClassOf(x);
                        auto xk = actorKind_.find(xc);
                        if (xk == actorKind_.end()) xk = actorKind_.emplace(xc, uint8_t(!IsChildOf(xc, actorBase) ? 0 : ClassSkipped(xc) ? 2 : 1)).first;
                        if (xk->second == 2) { touched_[x] = -1.0; continue; }
                    }
                    Call rc(x, "K2_GetRootComponent");
                    UObject* root = rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr;
                    RootCalls* c = root && MovableComp(root) ? CallsFor(root) : nullptr;
                    bool sim = false;
                    if (c) { c->simulating.put(0, FName{}); sim = c->simulating.invoke(root) && c->simulating.take<bool>(1); }
                    touched_[x] = sim ? SteadyNowMs() : -1.0;
                    if (sim) NoteSpawn();
                }
            }
            continue;
        }
        UObject* other = side == 0 ? b : a;
        // What touched it (ContactClaimant): a fighter simulated here, or a weapon in its hand ("Parent Actor" only: a thrown
        // weapon's "Last Parent" is not a touch); a stand-in or its weapon claims nothing; and a prop this machine leases
        // (IslandSource: not a FREE one the host streams) claims what it pushes, credited to whoever touched that prop
        // last -- the pile one hit disturbed has one simulator.
        UObject* fighter = adapter_->IsWillie(other) ? other : adapter_->IsWeapon(other) ? ObjectField(other, 0) : nullptr;
        OtherKind kind = OtherKind::StandIn;
        uint8_t entity = 0;
        bool streamedHere = false;
        if (fighter) {
            entity = env_.ownedEntity(fighter);
            if (entity) kind = adapter_->IsWillie(other) ? OtherKind::OwnedFighter : OtherKind::OwnedWeapon;
        } else if (auto pusher = byActor_.find(other); pusher != byActor_.end()) {
            kind = OtherKind::Prop;
            streamedHere = IslandSource(view_.Streamer(pusher->second), view_.Level(pusher->second), MySlot());
            if (auto q = props_.find(pusher->second); q != props_.end()) entity = q->second.toucher;
            if (!entity) entity = uint8_t(MySlot() + 1);   // this machine's fighter: the roster slot's entity
        }
        // A touch here quiets a soft structure's rest lock for a while (sync/hsm_rest_lock.h): a fighter here (a stand-in too: it
        // pushes the copy here), a weapon, or another tracked prop. Stamped before the claim, since a stand-in claims nothing.
        if (fighter || kind == OtherKind::Prop) {
            if (auto t = props_.find(it->second); t != props_.end()) {
                t->second.lastTouchMs = SteadyNowMs();
                if (fighter) t->second.lastHitMs = t->second.lastTouchMs;   // a break right after it is a hit (ClassifyLocalBreak)
            }
        }
        entity = ContactClaimant(entity, kind, streamedHere);
        if (!entity) continue;
        auto p = props_.find(it->second);
        if (p == props_.end() || OwnCopy(p->first, p->second)) continue;   // a structure is leased by nobody
        p->second.lastContactMs = SteadyNowMs();
        p->second.toucher = entity;
        if (kind == OtherKind::Prop) ++n_.islands;
        else ++n_.contacts;
    }
}

// A prop's contact with a fighter: the hook of "Deal Complex Damage" asks what struck (game::Claims).
bool ObjectSync::PropOf(UObject* actor, Touch& out) {
    if (!active_ || !actor) return false;
    auto it = byActor_.find(actor);
    if (it == byActor_.end()) return false;
    auto p = props_.find(it->second);
    if (p == props_.end() || !adapter_->StillValid(p->second.actor, p->second.index) || ObjectField(actor, 0)) return false;
    if (OwnCopy(p->first, p->second)) return false;   // a structure is simulated by every machine: its blows are native, nobody claims them
    out = Touch{};
    out.id = p->first;
    out.streamedHere = view_.Streamer(p->first) == MySlot();
    out.toucher = p->second.toucher;
    BodyStateMsg s;
    if (out.streamedHere && Read(p->second, 0, s)) out.velocity = sync::V3{s.vx, s.vy, s.vz};
    return true;
}

UObject* ObjectSync::ActorOf(uint64_t propId) const {
    auto it = props_.find(propId);
    return it != props_.end() && adapter_->StillValid(it->second.actor, it->second.index) ? it->second.actor : nullptr;
}

// ---------------------------------------------------------------- engine access

bool ObjectSync::Read(Prop& p, size_t body, BodyStateMsg& out) {
    if (body >= p.bodies.size()) return false;
    BodyRef& r = p.bodies[body];
    if (r.readTick == tick_) { out = r.read; return r.readOk; }
    r.readTick = tick_;
    r.readOk = false;
    RootCalls* c = adapter_->StillValid(r.comp, r.index) ? CallsFor(r.comp) : nullptr;
    if (!c || !c->toWorld.invoke(r.comp)) { ++n_.readFailed; return false; }
    const FTransform t = c->toWorld.take<FTransform>(0);
    c->linVel.put(0, FName{});
    const FVector v = c->linVel.invoke(r.comp) ? c->linVel.take<FVector>(1) : FVector{};
    c->angVel.put(0, FName{});
    const FVector w = c->angVel.invoke(r.comp) ? c->angVel.take<FVector>(1) : FVector{};
    for (double x : {t.translation.x, t.translation.y, t.translation.z})
        if (!std::isfinite(x) || std::fabs(x) >= double(kMaxPropCoord)) { ++n_.readFailed; return false; }
    BodyStateMsg s;
    s.px = float(t.translation.x);
    s.py = float(t.translation.y);
    s.pz = float(t.translation.z);
    s.rot = PackQuat(float(t.rotation.x), float(t.rotation.y), float(t.rotation.z), float(t.rotation.w));
    s.vx = Clamped(v.x, kMaxPropSpeed);
    s.vy = Clamped(v.y, kMaxPropSpeed);
    s.vz = Clamped(v.z, kMaxPropSpeed);
    s.wx = Clamped(w.x, kMaxPropSpin);
    s.wy = Clamped(w.y, kMaxPropSpin);
    s.wz = Clamped(w.z, kMaxPropSpin);
    r.read = s;
    r.readOk = true;
    out = s;
    return true;
}

// Drive every body of a copy that has a fresh state from the streamer (BodyRef::dirty).
bool ObjectSync::Drive(uint64_t id, Prop& p) {
    if (Kept(id, p)) {   // a structure is never placed by a state (teleporting joined bodies breaks their joints)
        for (BodyRef& r : p.bodies) if (r.dirty) { r.dirty = false; ++n_.structStatesSkipped; }
        return false;
    }
    bool any = false;
    for (size_t b = 0; b < p.bodies.size(); ++b) {
        BodyRef& r = p.bodies[b];
        if (!r.dirty) continue;
        r.dirty = false;
        if (r.detached) continue;
        if (const BodyStateMsg* s = view_.Find(id, uint8_t(b)))
            if (DriveBody(p, b, *s)) any = true;
    }
    return any;
}

// A soft structure (hsm_structure.h) on a machine that does not stream it: no body is teleported by a state (the joints do
// not follow a teleport and break). Each body with a fresh state is steered toward the host's pose, extrapolated by its velocity
// to now, with velocity (sync/hsm_soft_correct.h Steer): v = v_host + a push toward the pose, nothing under the dead band.
// The exceptions: a copy that broke here alone (PlaceBroken), and a structure that is badly off as a whole (any body past kSnapCm,
// or one far for kStuckMs on end), which is put right from one snapshot in this frame, each body with the host's velocity, at most
// every kSnapGapMs. Only the root and the bodies that hang on nothing are placed; the others go with them.
// Applied when a state arrives (20 Hz while it swings), not every frame, so the local physics (a fighter walking on its own
// copy) is left alone in between.
bool ObjectSync::SoftDrive(uint64_t id, Prop& p, double nowMs) {
    struct Item {
        size_t b = 0;
        BodyStateMsg now;           // the body as it is here
        sync::V3 pos;
        sync::Q4 rot;
        BodyStateMsg host;          // the host state as heard, which DriveBody reads
        sync::SoftTarget target;    // the host's state for it, extrapolated to now
    };
    // A break waits to be passed on (session/hsm_structure.h NextBreakStep): it goes when the time after a snap is over, so nothing
    // here may place a body and start that time again.
    if (BreaksHeld(p.heldJoints, p.heldBodies)) {
        for (BodyRef& r : p.bodies) r.dirty = false;
        SetFollowing(p, false, nowMs);
        return false;
    }
    // A joint broke in this copy only and was not passed on (session/hsm_structure.h ClassifyLocalBreak): the copy is not the
    // host's shape any more, and steering or snapping its pieces toward the whole one would throw them about.
    if (CopyDiverged(p.localBroken, p.bodyOffHere)) {
        ++n_.structDiverged;
        SetFollowing(p, false, nowMs);
        if (FollowsWhenBroken(env_.syncStructures, env_.structureFollowBroken, p.found, p.shapeDiffers)) return PlaceBroken(id, p, nowMs);
        for (BodyRef& r : p.bodies) r.dirty = false;
        return false;
    }
    std::vector<Item> items;
    double worst = 0, oldest = 0, newest = 1e18;
    bool hostRest = true;               // every body the host streams is at rest (PropAtRest, as StreamState)
    for (size_t b = 0; b < p.bodies.size(); ++b) {
        BodyRef& r = p.bodies[b];
        if (r.detached || r.rxMs <= 0 || !adapter_->StillValid(r.comp, r.index) || IsGeometryCollection(r.comp)) continue;
        const BodyStateMsg* s = view_.Find(id, uint8_t(b));
        if (!s) continue;
        if (!PropAtRest(*s)) hostRest = false;
        // A body that does not simulate here (a plank before its start-up delay turned its physics on) is neither steered nor
        // placed: a teleport of a body held by an anchor joint breaks the joint, and it does not count toward the snap.
        RootCalls* c = CallsFor(r.comp);
        if (!c) continue;
        c->simulating.put(0, FName{});
        if (!c->simulating.invoke(r.comp) || !c->simulating.take<bool>(1)) { r.dirty = false; ++n_.structNotSimulating; continue; }
        Item it;
        it.b = b;
        it.host = *s;
        if (!Read(p, b, it.now)) continue;
        float qx, qy, qz, qw;
        UnpackQuat(it.now.rot, qx, qy, qz, qw);
        it.pos = {it.now.px, it.now.py, it.now.pz};
        it.rot = {qx, qy, qz, qw};
        UnpackQuat(s->rot, qx, qy, qz, qw);
        const double age = std::max(0.0, nowMs - r.rxMs);
        const sync::V3 vel{s->vx, s->vy, s->vz}, ang{s->wx, s->wy, s->wz};
        it.target.pos = sync::ExtrapolatePos({s->px, s->py, s->pz}, vel, age);
        it.target.rot = sync::ExtrapolateRot({qx, qy, qz, qw}, ang, age);
        it.target.vel = vel;
        it.target.angVel = ang;
        worst = std::max(worst, sync::VLen({it.target.pos.x - it.pos.x, it.target.pos.y - it.pos.y, it.target.pos.z - it.pos.z}));
        oldest = std::max(oldest, age);
        newest = std::min(newest, age);
        items.push_back(it);
    }
    if (items.empty()) { for (BodyRef& r : p.bodies) r.dirty = false; SetFollowing(p, false, nowMs); return false; }
    // Moving follow (sync/hsm_moving_follow.h): while the streamer copy of a late-found structure moves, every body here goes to
    // the host pose from this one snapshot, as a rock does, and the physics carries it between states. Items are in body order,
    // so the root goes first: moving the actor carries what hangs on it, and each body after it is placed at its own target.
    // A snapshot that is not followable leaves the steering below to run.
    double fastCm = 0, fastDeg = 0;
    bool fresh = false;
    for (const Item& it : items) {
        fastCm = std::max(fastCm, sync::VLen(it.target.vel));
        fastDeg = std::max(fastDeg, sync::VLen(it.target.angVel));
        fresh = fresh || p.bodies[it.b].dirty;
    }
    const bool moving = p.follow.Update(nowMs, fastCm, fastDeg);
    const bool follows = fresh && sync::CopyFollows(env_.structureFollowMoving, p.found, !p.shapeDiffers, nowMs - p.bornMs, moving,
                                                    sync::SnapshotFollowable(oldest, newest), env_.structureBridgesFollow);
    SetFollowing(p, follows, nowMs);
    if (follows) {
        bool any = false;
        for (const Item& it : items) {
            BodyStateMsg s = it.host;
            s.px = float(it.target.pos.x);
            s.py = float(it.target.pos.y);
            s.pz = float(it.target.pos.z);
            s.rot = PackQuat(float(it.target.rot.x), float(it.target.rot.y), float(it.target.rot.z), float(it.target.rot.w));
            p.bodies[it.b].steerMs = nowMs;
            if (!DriveBody(p, it.b, s)) continue;
            ++n_.structFollowed;
            any = true;
        }
        for (BodyRef& r : p.bodies) r.dirty = false;
        p.stuck = sync::StuckTimer{};   // a copy that follows is not stuck: its clock starts again when it stops
        p.rest = sync::RestLock{};      // and it is not at rest: the rest clock starts again when it stops
        // A bridge's joint that tears here stays here (NextBreakStep: after a snap, census): the host's own break reaches everyone.
        // A late-found copy takes no stamp, so a hit while it follows is shared.
        if (!p.found) p.lastSnapMs = nowMs;
        return any;
    }
    // A soft structure at rest (sync/hsm_rest_lock.h): a copy far from the host's rest pose is placed once to it, then held still:
    // no steer, no snap, no velocity, until something touches it here or the host's root moves. Hold writes nothing at all.
    if (env_.structureRestLock) {
        const Item* rootItem = &items.front();
        for (const Item& it : items) if (it.b == 0) rootItem = &it;
        const bool wasLocked = p.rest.locked;
        double bodyMoved = 0;   // a hinged piece can swing on the host while the root stays
        if (wasLocked)
            for (const Item& it : items) {
                const sync::V3& at = p.bodies[it.b].restAt;
                bodyMoved = std::max(bodyMoved, sync::VLen({it.target.pos.x - at.x, it.target.pos.y - at.y, it.target.pos.z - at.z}));
            }
        const sync::RestStep step = sync::DecideRestStep(p.rest, nowMs, hostRest, rootItem->target.pos, worst, nowMs - p.lastTouchMs,
                                                         (p.found || env_.structureBridgesFollow) && sync::SnapAllowed(nowMs - p.bornMs),
                                                         sync::SnapReady(oldest, newest), nowMs - p.lastSnapMs, bodyMoved);
        if (p.rest.locked && !wasLocked) for (const Item& it : items) p.bodies[it.b].restAt = it.target.pos;
        if (step == sync::RestStep::Hold) {
            if (!wasLocked) { ++n_.restLocked; LogRestLock(p, worst, false); }
            ++n_.restHeld;
            for (BodyRef& r : p.bodies) r.dirty = false;
            p.stuck = sync::StuckTimer{};
            return false;
        }
        if (step == sync::RestStep::Place) {
            bool placed = false, failed = false;
            for (const Item& it : items) {
                if (!BodyPlacedAtRest(it.b, p.attachedMask, p.found)) continue;
                BodyRef& r = p.bodies[it.b];
                const Quat16 packed = PackQuat(float(it.target.rot.x), float(it.target.rot.y), float(it.target.rot.z), float(it.target.rot.w));
                if (!ApplyBodyPose(r.comp, float(it.target.pos.x), float(it.target.pos.y), float(it.target.pos.z), packed)) { failed = true; continue; }
                placed = true;
                r.steerMs = nowMs;
                RootCalls* c = CallsFor(r.comp);
                if (!c) continue;
                c->setLin.put(0, FVector{0, 0, 0}).put(1, false).put(2, FName{}).invoke(r.comp);
                c->setAng.put(0, FVector{0, 0, 0}).put(1, false).put(2, FName{}).invoke(r.comp);
            }
            if (placed && !failed) {
                for (const Item& it : items)   // at rest: asleep until a touch or a move wakes it
                    if (RootCalls* c = CallsFor(p.bodies[it.b].comp); c && c->sleep.valid()) c->sleep.put(0, FName{}).invoke(p.bodies[it.b].comp);
                for (BodyRef& r : p.bodies) r.dirty = false;
                p.lastSnapMs = nowMs;
                p.restPlacedMs = nowMs;
                p.stuck = sync::StuckTimer{};
                ++n_.restLocked;
                ++n_.restPlaced;
                LogRestLock(p, worst, true);
                return true;
            }
            p.rest.locked = false;   // not all of it could be placed here: steered as before, and tried again
        }
    }
    const double stuckMs = p.stuck.Update(nowMs, worst);
    static_assert(kBreakSettleMs == sync::kSnapSettleMs, "a break kept for settling and a snap held back for it are the same wait");
    const bool snap = sync::SnapAllowed(nowMs - p.bornMs) &&
                      sync::ShouldSnap(sync::SnapReady(oldest, newest), worst, stuckMs, nowMs - p.lastSnapMs);
    bool any = false;
    for (const Item& it : items) {
        BodyRef& r = p.bodies[it.b];
        if (!snap && !r.dirty) continue;
        r.dirty = false;
        sync::V3 lin = it.target.vel, ang = it.target.angVel;
        if (snap) {   // one snapshot, all bodies, this frame; the host's velocity, no relative velocity left to correct
            const Quat16 packed = PackQuat(float(it.target.rot.x), float(it.target.rot.y), float(it.target.rot.z), float(it.target.rot.w));
            if (BodyPlacedOnItsOwn(it.b, p.attachedMask) &&
                !ApplyBodyPose(r.comp, float(it.target.pos.x), float(it.target.pos.y), float(it.target.pos.z), packed)) continue;
        } else {
            const double dt = r.steerMs < -1e17 ? 0.05 : (nowMs - r.steerMs) / 1000.0;
            const sync::SoftSteer st = sync::Steer(it.pos, it.rot, it.target, dt);
            lin = st.vel;
            ang = st.angVel;
            if (st.corrected) ++n_.structCorrected;
            // In place and at rest on both machines: leave its velocity alone so the physics can keep it asleep.
            if (sync::KeepLocalVelocity(st, it.target.vel, it.target.angVel, {it.now.vx, it.now.vy, it.now.vz},
                                        {it.now.wx, it.now.wy, it.now.wz})) {
                r.steerMs = nowMs;
                ++n_.structRestKept;
                continue;
            }
        }
        r.steerMs = nowMs;
        any = true;
        RootCalls* c = CallsFor(r.comp);
        if (!c) continue;
        c->setAng.put(0, FVector{ang.x, ang.y, ang.z}).put(1, false).put(2, FName{}).invoke(r.comp);
        c->setLin.put(0, FVector{lin.x, lin.y, lin.z}).put(1, false).put(2, FName{}).invoke(r.comp);
        ++n_.bodyDriven;
    }
    if (snap) {
        for (BodyRef& r : p.bodies) r.dirty = false;
        p.lastSnapMs = nowMs;
        p.stuck = sync::StuckTimer{};
        ++n_.structSnapped;
        LogStructureSnap(p, worst);
    }
    return any;
}

// A copy starts or stops following its streamer copy (SoftDrive). Each change is counted; the log lines are rate limited.
void ObjectSync::SetFollowing(Prop& p, bool on, double nowMs) {
    if (p.following == on) return;
    p.following = on;
    if (on) {
        p.followSinceMs = nowMs;
        ++n_.followStarts;
    }
    LogFollow(p, on, nowMs - p.followSinceMs);
}

// At most a few lines a minute, however many copies follow (the health line counts every start and stop).
void ObjectSync::LogFollow(const Prop& p, bool started, double durationMs) {
    constexpr int kFollowLogsPerMinute = 8;
    const double now = SteadyNowMs();
    if (now - followLogWindowMs_ >= 60000.0 || now < followLogWindowMs_) { followLogWindowMs_ = now; followLogCount_ = 0; }
    if (followLogCount_++ >= kFollowLogsPerMinute) return;
    if (started) HSM_LOG("props: structure %s follows the host copy while it moves", ObjectName(p.actor).c_str());
    else HSM_LOG("props: structure %s stops following the host copy after %.1f s", ObjectName(p.actor).c_str(), durationMs / 1000.0);
}

// A late-found prop that broke on this screen alone (session/hsm_structure.h FollowsWhenBroken): the root and the bodies that
// hang on nothing go to the host's place when ShouldPlaceBody says so, the others go with them. As in a snap, not while the copy
// settles and never a body that does not simulate here (a teleport of a body held by an anchor joint breaks the joint). Only a
// real placement stamps lastSnapMs, so a break right after one stays here and a later hit is passed on.
bool ObjectSync::PlaceBroken(uint64_t id, Prop& p, double nowMs) {
    const bool settled = sync::SnapAllowed(nowMs - p.bornMs);
    bool any = false;
    for (size_t b = 0; b < p.bodies.size(); ++b) {
        BodyRef& r = p.bodies[b];
        const bool fresh = r.dirty;
        r.dirty = false;
        if (!fresh || !settled || r.detached || !BodyPlacedOnItsOwn(b, p.attachedMask) || !adapter_->StillValid(r.comp, r.index)) continue;
        const BodyStateMsg* s = view_.Find(id, uint8_t(b));
        RootCalls* c = s ? CallsFor(r.comp) : nullptr;
        if (!c) continue;
        c->simulating.put(0, FName{});
        if (!c->simulating.invoke(r.comp) || !c->simulating.take<bool>(1)) { ++n_.structNotSimulating; continue; }
        BodyStateMsg here;
        if (!Read(p, b, here)) continue;
        const sync::V3 host{s->px, s->py, s->pz};
        const double off = sync::VLen({host.x - here.px, host.y - here.py, host.z - here.pz});
        const double moved = sync::VLen({host.x - r.placedAt.x, host.y - r.placedAt.y, host.z - r.placedAt.z});
        if (!ShouldPlaceBody(r.placed, moved, off) || !DriveBody(p, b, *s)) continue;
        r.placed = true;
        r.placedAt = host;
        any = true;
    }
    if (any) {
        p.lastSnapMs = nowMs;
        ++n_.structPlaced;
    }
    return any;
}

// The root body of an actor whose root component is that body moves the whole actor (ApplyPropPose, which also sets a
// weapon's linear velocity on its "BaseMesh"); every other body is placed by its own component transform. Velocities are set
// only on a body that simulates physics.
bool ObjectSync::DriveBody(Prop& p, size_t b, const BodyStateMsg& s) {
    BodyRef& r = p.bodies[b];
    if (!adapter_->StillValid(r.comp, r.index) || IsGeometryCollection(r.comp)) return false;
    const bool whole = b == 0 && p.rootBody;
    if (!(whole ? ApplyPropPose(p.actor, s.px, s.py, s.pz, s.rot, s.vx, s.vy, s.vz) : ApplyBodyPose(r.comp, s.px, s.py, s.pz, s.rot))) return false;
    if (b == 0) ++n_.driven; else ++n_.bodyDriven;
    RootCalls* c = CallsFor(r.comp);
    if (!c) return true;
    c->simulating.put(0, FName{});
    if (c->simulating.invoke(r.comp) && c->simulating.take<bool>(1)) {
        c->setAng.put(0, FVector{double(s.wx), double(s.wy), double(s.wz)}).put(1, false).put(2, FName{}).invoke(r.comp);
        // ApplyPropPose sets the linear velocity on a weapon's "BaseMesh"; any other body is set here.
        if (b > 0 || p.level || p.spawned || !p.rootBody)
            c->setLin.put(0, FVector{double(s.vx), double(s.vy), double(s.vz)}).put(1, false).put(2, FName{}).invoke(r.comp);
    }
    return true;
}

ObjectSync::RootCalls* ObjectSync::CallsFor(UObject* root) {
    const UClass* cls = ClassOf(root);
    if (auto it = calls_.find(cls); it != calls_.end()) return it->second.get();
    auto c = std::make_unique<RootCalls>();
    c->toWorld = PreparedCall(cls, "K2_GetComponentToWorld", {"ReturnValue"});
    c->linVel = PreparedCall(cls, "GetPhysicsLinearVelocity", {"BoneName", "ReturnValue"});
    c->angVel = PreparedCall(cls, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    c->setAng = PreparedCall(cls, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
    c->setLin = PreparedCall(cls, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
    c->simulating = PreparedCall(cls, "IsSimulatingPhysics", {"BoneName", "ReturnValue"});
    c->sleep = PreparedCall(cls, "PutRigidBodyToSleep", {"BoneName"});   // optional: without it a placed body is only left still
    if (!c->toWorld.valid() || !c->linVel.valid() || !c->angVel.valid() || !c->setAng.valid() || !c->setLin.valid() ||
        !c->simulating.valid()) {
        HSM_LOG("props: %s is not a physics body; props rooted in it are not synced", ObjectName(reinterpret_cast<const UObject*>(cls)).c_str());
        c.reset();
    }
    return (calls_[cls] = std::move(c)).get();
}

UObject* ObjectSync::ObjectField(UObject* o, int which) {
    if (!o || which < 0 || which > 2) return nullptr;
    std::unordered_map<const UClass*, int32_t>& offs = fieldOff_[size_t(which)];   // per class: props of mixed classes interleave
    const UClass* cls = ClassOf(o);
    auto it = offs.find(cls);
    if (it == offs.end()) {
        FProperty* p = nullptr;
        if (which == 0) p = FindProperty(ClassOf(o), "Parent Actor");
        else if (which == 1) p = FindProperty(ClassOf(o), "Weapon R");
        else p = FindProperty(ClassOf(o), "Weapon L");
        it = offs.emplace(cls, p && PropertySize(p) == int32_t(sizeof(UObject*)) ? PropertyOffset(p) : -1).first;
    }
    const int32_t off = it->second;
    return off >= 0 ? *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(o) + off) : nullptr;
}

// Every machine decides by itself (IdleDespawnDue): the FREE Grant came to all of them at once. Only in the bot formats
// (SetIdleDespawn); Duel, free-for-all and teams clear them all at a new round or rematch (session ClearDroppedWeapons).
void ObjectSync::SetIdleDespawn(bool on) {
    if (on && !idleDespawn_) {                       // the minute starts now for what already lay FREE
        const double now = SteadyNowMs();
        for (auto& kv : freeSince_) kv.second = now;
    }
    idleDespawn_ = on;
}

void ObjectSync::DespawnIdle(double nowMs) {
    if (freeSince_.empty()) return;
    for (auto it = props_.begin(); it != props_.end();) {
        Prop& p = it->second;
        const auto f = freeSince_.find(it->first);
        // Not FREE here, or a claim of mine on its way (my fighter touched it): not idle.
        if (p.level || p.spawned || f == freeSince_.end() || p.claim.sent != LeaseLevel::Free || !adapter_->StillValid(p.actor, p.index) ||
            ObjectField(p.actor, 0)) {
            ++it;
            continue;
        }
        if (!IdleDespawnDue(it->first, f->second, nowMs)) { ++it; continue; }
        if (++n_.despawned <= kLogFirst)
            HSM_LOG("props: %s (#%llx) despawned: FREE for %.0f s", ObjectName(p.actor).c_str(),
                    static_cast<unsigned long long>(it->first), (nowMs - f->second) / 1000.0);
        adapter_->DestroyActor(p.actor);
        freeSince_.erase(f);
        byActor_.erase(p.actor);
        it = props_.erase(it);
    }
}

// ---------------------------------------------------------------- health

std::string ObjectSync::HealthLine() {
    const Counters& a = n_;
    const Counters& b = reported_;
    auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
    size_t level = 0;
    for (const auto& kv : props_) level += kv.second.level ? 1 : 0;
    char line[1536];
    std::snprintf(line, sizeof line,
                  "props: live %zu pending %zu loose %zu | drops %llu minted %llu mintFailed %llu | resolved %llu forced %llu kept %llu "
                  "unresolved %llu adopted %llu releaseSkipped %llu releaseFailed %llu | requests %llu releases %llu holdDenied %llu contacts %llu islands %llu | "
                  "grants %llu lapsed %llu leasesRejected %llu | statesSent %llu statesApplied %llu statesStale %llu "
                  "statesRejected %llu driven %llu readFailed %llu lost %llu | hands: attached %llu attachMiss %llu spawned %llu (failed %llu) "
                  "released %llu selfDrops %llu reattached %llu leaseReleased %llu flagSkipped %llu | level %zu of %zu streamed %llu driven %llu deferred %llu miss %zu "
                  "leftOut %zu purged %llu censuses %llu censusRejected %llu | despawned %llu resetUnmoved %llu",
                  props_.size(), pending_.size(), loose_.size(), d(a.drops, b.drops), d(a.minted, b.minted), d(a.mintFailed, b.mintFailed),
                  d(a.resolved, b.resolved), d(a.forced, b.forced), d(a.kept, b.kept), d(a.unresolved, b.unresolved), d(a.adopted, b.adopted), d(a.releaseSkipped, b.releaseSkipped),
                  d(a.releaseFailed, b.releaseFailed), d(a.requests, b.requests), d(a.releases, b.releases),
                  d(a.holdDenied, b.holdDenied), d(a.contacts, b.contacts), d(a.islands, b.islands), d(a.grants, b.grants), d(a.lapsed, b.lapsed),
                  d(a.leasesRejected, b.leasesRejected), d(a.statesSent, b.statesSent), d(a.statesApplied, b.statesApplied),
                  d(a.statesStale, b.statesStale), d(a.statesRejected, b.statesRejected), d(a.driven, b.driven),
                  d(a.readFailed, b.readFailed), d(a.lost, b.lost), d(a.attached, b.attached), d(a.attachMiss, b.attachMiss), d(a.spawned, b.spawned), d(a.spawnFailed, b.spawnFailed),
                  d(a.matchReleased, b.matchReleased), d(a.selfDrops, b.selfDrops), d(a.reattached, b.reattached), d(a.leaseReleased, b.leaseReleased),
                  d(a.flagSkipped, b.flagSkipped), level,
                  censusIndex_.size(), d(a.levelStreamed, b.levelStreamed), d(a.levelDriven, b.levelDriven),
                  d(a.levelDeferred, b.levelDeferred), misses_, book_.LeftOut(), d(a.censusPurged, b.censusPurged),
                  d(a.censuses, b.censuses), d(a.censusRejected, b.censusRejected), d(a.despawned, b.despawned), d(a.resetUnmoved, b.resetUnmoved));
    size_t spawned = 0, bodies = 0, structures = 0, found = 0;
    for (const auto& kv : props_) {
        spawned += kv.second.spawned ? 1 : 0;
        bodies += kv.second.bodies.size();
        const bool own = OwnCopy(kv.first, kv.second);
        structures += own ? 1 : 0;
        found += own && kv.second.found ? 1 : 0;
    }
    char structText[640];   // by mode: soft streams and steers, local skips, full is pre17 (nothing is a structure)
    if (env_.syncStructures == StructureMode::Soft)
        std::snprintf(structText, sizeof structText,
                      "structures %zu soft (%zu found late), corrections %llu, snaps %llu, at rest %llu, not simulating %llu, "
                      "diverged %llu, placed %llu, breaks passed on %llu held %llu kept here %llu dropped %llu, shape differs %llu, "
                      "followed %llu bodies (%llu starts), breaks while following %llu, "
                      "rest placed %llu, rest locked %llu, rest held %llu, game breaks %llu",
                      structures, found, d(a.structCorrected, b.structCorrected), d(a.structSnapped, b.structSnapped),
                      d(a.structRestKept, b.structRestKept), d(a.structNotSimulating, b.structNotSimulating),
                      d(a.structDiverged, b.structDiverged), d(a.structPlaced, b.structPlaced), d(a.jointsReported, b.jointsReported),
                      d(a.breaksHeld, b.breaksHeld), d(a.jointsKeptLocal, b.jointsKeptLocal),
                      d(a.jointReportsDropped, b.jointReportsDropped), d(a.shapeMismatch, b.shapeMismatch),
                      d(a.structFollowed, b.structFollowed), d(a.followStarts, b.followStarts), d(a.breaksWhileFollowing, b.breaksWhileFollowing),

                      d(a.restPlaced, b.restPlaced), d(a.restLocked, b.restLocked), d(a.restHeld, b.restHeld),
                      d(a.gameBreaks, b.gameBreaks));
    else if (env_.syncStructures == StructureMode::Local)
        std::snprintf(structText, sizeof structText,
                      "structures %zu local (%zu found late), states skipped %llu, reset skipped %llu, breaks passed on %llu held %llu kept here %llu "
                      "dropped %llu, shape differs %llu",
                      structures, found, d(a.structStatesSkipped, b.structStatesSkipped), d(a.structResetSkipped, b.structResetSkipped),
                      d(a.jointsReported, b.jointsReported), d(a.breaksHeld, b.breaksHeld), d(a.jointsKeptLocal, b.jointsKeptLocal),
                      d(a.jointReportsDropped, b.jointReportsDropped), d(a.shapeMismatch, b.shapeMismatch));
    else
        std::snprintf(structText, sizeof structText, "structures full (as pre17)");
    char objects[640];
    std::snprintf(objects, sizeof objects,
                  " | objects: spawned %zu bodies %zu announced %llu pending %zu ghosts %zu (expired %llu) | proposed %llu refused %llu rekeyed %llu "
                  "bound %llu rejected %llu | bodiesDriven %llu rechecks %llu | events sent %llu applied %llu queued %zu rejected %llu "
                  "(joints %llu detached %llu destroyed sent %llu applied %llu)",
                  spawned, bodies, d(a.announced, b.announced), announced_.size(), ghosts_.Size(), d(a.ghostsExpired, b.ghostsExpired),
                  d(a.proposed, b.proposed), d(a.refused, b.refused), d(a.rekeyed, b.rekeyed), d(a.bound, b.bound),
                  d(a.announceRejected, b.announceRejected), d(a.bodyDriven, b.bodyDriven), d(a.rechecks, b.rechecks),
                  d(a.eventsSent, b.eventsSent), d(a.eventsApplied, b.eventsApplied), queuedEvents_.size(),
                  d(a.eventsRejected, b.eventsRejected), d(a.jointsBroken, b.jointsBroken), d(a.bodiesDetached, b.bodiesDetached),
                  d(a.destroyedSent, b.destroyedSent), d(a.destroyedApplied, b.destroyedApplied));
    char fixes[1408];   // structText (up to 640) included
    std::snprintf(fixes, sizeof fixes,
                  " | pre2: skippedClasses %llu attachHeld %llu letGo sent %llu applied %llu | adopt known %llu bound %llu expired %llu | "
                  "walk asked %llu cached %llu capped %llu | %s | "
                  "pre18: given up %llu, recovered %llu (aliased %llu expired %llu) dropRestored %llu",
                  d(a.skippedClasses, b.skippedClasses), d(a.attachHeld, b.attachHeld), d(a.letGoSent, b.letGoSent), d(a.letGoApplied, b.letGoApplied),
                  d(a.adoptKnown, b.adoptKnown), d(a.adoptBound, b.adoptBound), d(a.adoptExpired, b.adoptExpired),
                  d(a.simAsked, b.simAsked), d(a.simCached, b.simCached), d(a.simCapped, b.simCapped), structText,
                  d(a.unresolved, b.unresolved), d(a.lateRecovered, b.lateRecovered), d(a.lateAliased, b.lateAliased),
                  d(a.lateExpired, b.lateExpired), d(a.dropRestored, b.dropRestored));
    reported_ = n_;
    return std::string(line) + objects + fixes;
}

}  // namespace hsm::game
