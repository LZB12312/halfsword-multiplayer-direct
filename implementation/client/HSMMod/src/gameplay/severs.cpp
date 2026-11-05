#include "gameplay/severs.h"
#include "gameplay/combat_sync.h"
#include "log.h"
#include "session/hsm_authority.h"
#include "sync/hsm_grab.h"
#include "ue/script_hook.h"
#include "util/hsm_names.h"
#include "util/hsm_sanitize.h"
#include "util/hsm_validate.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace hsm::game {

using namespace hsm::ue;
using combat::SeverClaim;
using combat::StrikeSource;
using combat::StuckBladeMsg;
using combat::V3;
using combat::Xform;

namespace {

constexpr const char* kHookOwner = "severs";
constexpr const char* kInitiate = "Dismember Function Initiate";
constexpr const char* kDelayed = "Dismember Function Delayed";
constexpr const char* kInProcess = "Dismemberment In Process";        // set by an accepted Initiate (Willie_BP.cpp:3654),
                                                                      // cleared 0.5 s after Delayed finishes (8514)
constexpr const char* kCurrentPart = "Currently Dismembered Part";
constexpr const char* kNoDismember = "Force Disable Dismemberment";   // the stand-in rig re-asserts it every frame
constexpr double kRetryMs = 2000;            // a sever refused for another in process waits this long for it to finish
constexpr double kBlockedDelayedMs = 1000;   // a blocked Initiate's Delayed follows 0.1 s later; remembered this long
constexpr size_t kMaxBlockedOnMine = 64;
constexpr size_t kMaxRetries = 64;
constexpr size_t kMaxOwnCuts = 8;            // Initiates in flight on fighters simulated here (one, unless nested)
constexpr uint64_t kLogFirst = 20;           // the first events of each kind are logged in full (test evidence)
constexpr double kStuckSlackUu = 2;          // Constraint_Weapon_Stuck_BP.cpp:600-602: its limited linear axis on a cut
constexpr int32_t kMaxBoneNames = 1024;      // a sane bound on a Willie's bone and socket name arrays

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };
struct FNameArray { const FName* data = nullptr; int32_t num = 0, max = 0; };   // TArray<FName>

bool SameName(FName a, FName b) { return a.index == b.index && a.number == b.number; }
bool IsNone(FName n) { return n.index == 0 && n.number == 0; }

bool HasText(const std::string& s, const char* lowerNeedle) {   // KismetStringLibrary.Contains, case ignored
    std::string l(s);
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return l.find(lowerNeedle) != std::string::npos;
}

FVector ToF(const V3& v) { return FVector{v.x, v.y, v.z}; }
V3 ToV(const FVector& v) { return V3{v.x, v.y, v.z}; }

FTransform ToF(const Xform& x) {
    FTransform t{};
    t.rotation = FQuat{x.rot.x, x.rot.y, x.rot.z, x.rot.w};
    t.translation = ToF(x.pos);
    t.scale = ToF(x.scale);
    return t;
}

Xform ToXform(const FTransform& t) {
    Xform x;
    x.pos = ToV(t.translation);
    x.rot = sync::Q4{t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w};
    x.scale = ToV(t.scale);
    return x;
}

bool Finite(const FTransform& t) {
    return std::isfinite(t.translation.x) && std::isfinite(t.translation.y) && std::isfinite(t.translation.z) &&
           std::isfinite(t.rotation.x) && std::isfinite(t.rotation.y) && std::isfinite(t.rotation.z) &&
           std::isfinite(t.rotation.w);
}

const char* SourceName(StrikeSource s) {
    switch (s) {
    case StrikeSource::WeaponRight: return "weapon R";
    case StrikeSource::WeaponLeft: return "weapon L";
    default: return "body";
    }
}

// A world transform in a bone's frame (the inverse of combat::SeverBoxWorld).
Xform InBoneFrame(const Xform& bone, const Xform& world) {
    Xform x;
    x.pos = bone.ToLocal(world.pos);
    x.rot = combat::QuatMul(combat::Conjugate(bone.rot), world.rot);
    x.scale = world.scale;
    return x;
}

Xform FromBoneFrame(const Xform& bone, const Xform& local) {
    Xform x;
    x.pos = bone.FromLocal(local.pos);
    x.rot = combat::QuatMul(bone.rot, local.rot);
    x.scale = local.scale;
    return x;
}

}  // namespace

bool Severs::Start(Adapter& adapter, StandIns& standIns, OwnerStreams& streams, Env env) {
    Stop();
    adapter_ = &adapter;
    standIns_ = &standIns;
    streams_ = &streams;
    env_ = std::move(env);
    if (UClass* actor = FindClass("Actor"))
        components_ = PreparedCall(actor, "K2_GetComponentsByClass", {"ComponentClass", "ReturnValue"});
    primitive_ = FindClass("PrimitiveComponent");
    boxClass_ = FindClass("BoxComponent");
    constraintClass_ = FindClass("PhysicsConstraintComponent");
    int n = HookScript(kHookOwner, "Willie_BP_C", kInitiate, [this](ScriptCall& c) { return Classify(c, true); },
                       [this](ScriptCall& c) { OwnCutDone(c); });
    n += HookScript(kHookOwner, "Willie_BP_C", kDelayed, [this](ScriptCall& c) { return Classify(c, false); });
    // The rules stay registered: at the menu Willie_BP_C may not be loaded yet (n == 0); every level load resolves them.
    active_ = ScriptHookActive();
    HSM_LOG("severs: %s (%d function(s) hooked now; components %s, box class %s, constraint class %s)",
            active_ ? "on: severs of stand-ins become claims, replayed by the victim's owner; stuck blades held"
                    : "NOT ACTIVE (script VM hook unavailable): no sever claims, stand-ins show severs from vitals only",
            n, components_.valid() ? "ok" : "MISSING", boxClass_ ? "ok" : "MISSING", constraintClass_ ? "ok" : "MISSING");
    return active_;
}

void Severs::Stop() {
    if (adapter_) {
        ClearScriptHooks(kHookOwner);
        // A Delayed still due would leave that fighter's "Dismemberment In Process" set for the rest of its life (no
        // sever could start again): finish the ones on fighters simulated here. Stand-ins go with the session.
        std::vector<DueDelayed> due = std::move(due_);
        due_.clear();
        for (DueDelayed& d : due)
            if (!d.standIn) CallDelayed(d);
        while (!joints_.empty()) {
            const auto key = joints_.begin()->first;
            Let(key.first, StrikeSource(key.second));
        }
        if (active_) HSM_LOG("severs: off (%s)", HealthLine().c_str());
    }
    *this = Severs{};
}

void Severs::Tick(double nowMs, const Owned& owned) {
    if (!active_) return;
    for (size_t i = 0; i < due_.size();) {
        if (nowMs < due_[i].dueMs) { ++i; continue; }
        DueDelayed d = std::move(due_[i]);
        due_.erase(due_.begin() + static_cast<std::ptrdiff_t>(i));
        CallDelayed(d);
    }
    RunRetries(nowMs);
    pending_.Expire(nowMs);
    for (auto it = blockedOnMine_.begin(); it != blockedOnMine_.end();)
        it = nowMs - it->atMs >= kBlockedDelayedMs ? blockedOnMine_.erase(it) : std::next(it);
    for (const auto& [entity, willie] : owned)
        if (willie) ScanOurBlades(entity, willie);
    for (const combat::StuckBlades::Key& k : blades_.Expire(nowMs)) Let(k.attacker, k.source);
    // A hold also ends when its physics joint broke, or when anything in it died (a despawned stand-in, a level change).
    for (auto it = joints_.begin(); it != joints_.end();) {
        const Joint& j = it->second;
        bool broken = false;
        if (Live(j.constraint) && Live(j.owner) && Live(j.blade) && Live(j.mesh)) {
            Call b(j.constraint.obj, "IsBroken");
            if (!b.invoke() || !b.get<bool>("ReturnValue")) {
                ++it;
                continue;
            }
            broken = true;
        }
        const auto key = (it++)->first;   // Let erases that entry only
        if (broken && ++n_.broken <= kLogFirst)
            HSM_LOG("severs: the hold on the blade of e%u (%s) broke", key.first, SourceName(StrikeSource(key.second)));
        Let(key.first, StrikeSource(key.second));
    }
}

// ---------------------------------------------------------------- the hooks (game thread, inside the VM)

// self is the Willie losing the part; "Dismember Weapon" is the blade's component (Constraint_Weapon_Stuck_BP's
// "Component 1 (Weapon)"). On a stand-in every native sever is blocked: a stand-in loses a limb only when its owner's
// vitals say so (Show, under ScriptHookBypass). One by a fighter simulated here is captured as a claim first. On a
// fighter simulated here, a sever by a stand-in's blade is blocked (its owner's claim decides), and so is the Delayed
// that follows it: a Delayed whose Initiate never ran works on the stale "Currently Dismembered Part" (3255).
bool Severs::Classify(ScriptCall& c, bool initiate) {
    if (!active_) return false;
    UObject* self = c.self();
    UObject* weaponComp = nullptr;
    c.get("Dismember Weapon", weaponComp);
    UObject* striker = weaponComp ? adapter_->FighterOf(OuterOf(weaponComp)) : nullptr;
    if (const uint8_t victim = standIns_->EntityOf(self)) {
        const uint8_t attacker = (initiate && striker) ? env_.ownedEntity(striker) : 0;
        if (!attacker || attacker == victim) {
            ++n_.blockedStandIn;
            return true;
        }
        if (env_.allowed && !env_.allowed(attacker, victim)) {
            ++n_.friendly;
            return true;
        }
        uint8_t part = 0xFF;
        c.get("Dismembered Part", part);
        const double now = SteadyNowMs();
        if (!limiter_.Allow(victim, part, now)) {
            ++n_.limited;
            return true;
        }
        Capture(c, self, victim, attacker, striker, weaponComp, part, now);
        return true;
    }
    const uint8_t own = env_.ownedEntity(self);
    if (!own) return false;
    const bool fromStandIn = striker && standIns_->IsStandIn(striker);
    const double now = SteadyNowMs();
    if (initiate) {
        if (!fromStandIn) {
            NoteOwnCut(c, self, own, striker, weaponComp);
            return false;   // the game's own sever runs; its post-hook sends the notice (OwnCutDone)
        }
        if (blockedOnMine_.size() >= kMaxBlockedOnMine) blockedOnMine_.erase(blockedOnMine_.begin());
        blockedOnMine_.push_back(BlockedOnMine{self, weaponComp, now});
        ++n_.blockedOnMine;
        return true;
    }
    bool paired = false;
    for (auto it = blockedOnMine_.begin(); it != blockedOnMine_.end(); ++it)
        if (it->willie == self && it->weapon == weaponComp && now - it->atMs < kBlockedDelayedMs) {
            blockedOnMine_.erase(it);
            paired = true;
            break;
        }
    if (!fromStandIn && !paired) return false;
    ++n_.blockedOnMine;
    return true;
}

// ---------------------------------------------------------------- attacker side

void Severs::Capture(ScriptCall& c, UObject* standIn, uint8_t victim, uint8_t attacker, UObject* attackerWillie,
                     UObject* weaponComp, uint8_t part, double nowMs) {
    combat::SeverCapture in;
    in.attacker = attacker;
    in.victim = victim;
    in.part = part;
    switch (ReadSever(c, standIn, in)) {
    case Read::NoBoxes:
        ++n_.invalid;
        if (n_.invalid <= 3) HSM_LOG("severs: a sever of e%u by e%u had no readable VP boxes; no claim", victim, attacker);
        return;
    case Read::NoBone:
        ++n_.noBone;
        if (n_.noBone <= 3) HSM_LOG("severs: no frame bone for part %u on the stand-in of e%u; no claim", part, victim);
        return;
    case Read::Ok:
        break;
    }
    SourceOf(attackerWillie, weaponComp, in);
    in.claimId = nextClaimId_[attacker] + 1;
    in.attackerTsMs = streams_->ClockMs(nowMs);
    SeverClaim claim;
    if (!combat::MakeSeverClaim(in, claim)) {
        ++n_.invalid;
        if (n_.invalid <= 3)
            HSM_LOG("severs: a sever of e%u part %u by e%u cannot travel (marker '%s', bone '%s'); no claim", victim, part,
                    attacker, in.attachMarker.c_str(), in.frameBone.c_str());
        return;
    }
    nextClaimId_[attacker] = claim.claimId;
    env_.sendSever(claim);
    ++n_.captured;
    if (n_.captured <= kLogFirst)
        HSM_LOG("severs: captured #%u e%u -> e%u part %u frame %s marker '%s' (+%zu) by %s '%s' at t=%u", claim.claimId,
                attacker, victim, part, claim.frameBone.c_str(), claim.attachMarker.c_str(), claim.markers.size(),
                SourceName(claim.source), claim.weaponComponent.c_str(), claim.attackerTsMs);
    Remember(claim, nowMs);   // the stand-in shows it here once the owner's vitals confirm it
}

// The arguments of a native Initiate on willie (a stand-in, or a fighter simulated here), read before its body runs:
// its mesh and marker components by name, and the two VP boxes in the frame of the bone they are attached to.
Severs::Read Severs::ReadSever(ScriptCall& c, UObject* willie, combat::SeverCapture& in) {
    UObject* mesh = adapter_->Mesh(willie);
    UObject* master = nullptr;
    c.get("Master Mesh", master);
    if (!master) master = mesh;
    if (master != mesh) in.masterMesh = ObjectName(master);
    UObject* attach = nullptr;
    if (c.get("Attach Marker", attach) && attach) in.attachMarker = ObjectName(attach);
    // A by-reference TArray: param() points at the caller's array, read in place.
    if (const auto* arr = static_cast<const FObjArray*>(c.param("Overlapped Markers")))
        if (arr->data && arr->num > 0 && arr->num <= 1024)
            for (int32_t i = 0; i < arr->num; ++i)
                if (arr->data[i]) in.markers.push_back(ObjectName(arr->data[i]));
    auto readBox = [this](UObject* box, Xform& world, V3& extent) {
        if (!box || !ComponentXform(box, world)) return false;
        Call extentOf(box, "GetUnscaledBoxExtent");
        if (!extentOf.invoke()) return false;
        extent = ToV(extentOf.get<FVector>("ReturnValue"));
        return true;
    };
    UObject* box1 = nullptr;
    UObject* box2 = nullptr;
    c.get("VP Box Collision 1", box1);
    c.get("VP Box Collision 2", box2);
    if (!readBox(box1, in.box1World, in.box1Extent) || !readBox(box2, in.box2World, in.box2Extent)) return Read::NoBoxes;
    // Call_Dismember attaches both boxes to the struck bone ("Bone Name 2"): that bone is the claim's frame. A box with
    // no usable socket falls back to the part's own spawn bone.
    std::string bone;
    Call socket(box1, "GetAttachSocketName");
    if (socket.invoke()) bone = NameToString(socket.get<FName>("ReturnValue"));
    if (bone.empty() || bone == "None" || !BoneXform(master, bone, in.boneWorld)) {
        const char* spawn = combat::SeverSpawnBone(in.part);
        bone = spawn ? spawn : "";
        if (bone.empty() || !BoneXform(master, bone, in.boneWorld)) return Read::NoBone;
    }
    in.frameBone = bone;
    return Read::Ok;
}

// Which of fighter's hands the blade is in (Body when neither), and the blade's component by name.
void Severs::SourceOf(UObject* fighter, UObject* weaponComp, combat::SeverCapture& in) const {
    UObject* weapon = weaponComp ? OuterOf(weaponComp) : nullptr;
    if (weapon && weapon == WeaponOf(fighter, StrikeSource::WeaponRight)) in.source = StrikeSource::WeaponRight;
    else if (weapon && weapon == WeaponOf(fighter, StrikeSource::WeaponLeft)) in.source = StrikeSource::WeaponLeft;
    if (weaponComp) in.weaponComponent = ObjectName(weaponComp);
}

// ---------------------------------------------------------------- an own cut: the owner tells everyone else

// A native Initiate on a fighter simulated here, not by a stand-in's blade: the game's own sever runs. Its arguments are
// read now, before the body, as Capture reads them; OwnCutDone sends them if the game accepted the call. Only a cut by
// the fighter's own blade names the blade: the notice cannot name another striker (attacker == victim).
void Severs::NoteOwnCut(ScriptCall& c, UObject* self, uint8_t entity, UObject* striker, UObject* weaponComp) {
    OwnCut cut;
    cut.willie = self;
    cut.in.attacker = entity;
    cut.in.victim = entity;
    cut.in.part = 0xFF;
    c.get("Dismembered Part", cut.in.part);
    GetBool(self, kInProcess, cut.before);
    if (!cut.before && cut.in.part < combat::kSeverParts) {   // otherwise the game refuses it: nothing to read
        cut.read = ReadSever(c, self, cut.in);
        if (striker == self) SourceOf(self, weaponComp, cut.in);
    }
    if (ownCuts_.size() >= kMaxOwnCuts) ownCuts_.erase(ownCuts_.begin());   // a post-hook that never ran
    ownCuts_.push_back(std::move(cut));
}

void Severs::OwnCutDone(ScriptCall& c) {
    if (ownCuts_.empty() || ownCuts_.back().willie != c.self()) return;   // not a call NoteOwnCut saw
    OwnCut cut = std::move(ownCuts_.back());
    ownCuts_.pop_back();
    if (!active_) return;
    bool after = false;
    GetBool(cut.willie, kInProcess, after);
    const uint8_t* current = Field<uint8_t>(cut.willie, kCurrentPart);
    if (!combat::InitiateAccepted(cut.before, after, current ? *current : 0xFF, cut.in.part)) return;   // refused
    const uint8_t e = cut.in.victim;
    const double now = SteadyNowMs();
    cut.in.claimId = nextClaimId_[e] + 1;
    cut.in.attackerTsMs = streams_->ClockMs(now);
    SeverClaim notice;
    if (cut.read != Read::Ok || !combat::MakeSeverNotice(cut.in, notice)) {
        ++n_.noticeInvalid;
        if (n_.noticeInvalid <= 3)
            HSM_LOG("severs: an own cut of e%u part %u cannot travel (%s, marker '%s', bone '%s'); the others show it "
                    "from the vitals", e, cut.in.part,
                    cut.read == Read::NoBoxes ? "no readable VP boxes" : cut.read == Read::NoBone ? "no frame bone" : "read",
                    cut.in.attachMarker.c_str(), cut.in.frameBone.c_str());
        return;
    }
    nextClaimId_[e] = notice.claimId;
    if (env_.sendNotice) env_.sendNotice(notice);
    ++n_.noticed;
    if (n_.noticed <= kLogFirst)
        HSM_LOG("severs: noticed #%u: e%u lost part %u natively, frame %s marker '%s' (+%zu) by %s '%s' at t=%u",
                notice.claimId, e, notice.part, notice.frameBone.c_str(), notice.attachMarker.c_str(),
                notice.markers.size(), SourceName(notice.source), notice.weaponComponent.c_str(), notice.attackerTsMs);
}

// ---------------------------------------------------------------- running the game's own two functions

Severs::Run Severs::Begin(UObject* willie, const SeverClaim& c, double nowMs, bool standIn) {
    Rebuilt a;
    if (!Rebuild(willie, c, a)) return Run::Failed;
    bool before = false;
    GetBool(willie, kInProcess, before);
    if (!Invoke(kInitiate, a, standIn)) {
        DestroyBoxes(a);
        ++n_.callFailed;
        if (n_.callFailed <= 3) HSM_LOG("severs: #%u: %s could not be called on e%u", c.claimId, kInitiate, c.victim);
        return Run::Failed;
    }
    bool after = false;
    GetBool(willie, kInProcess, after);
    const uint8_t* current = Field<uint8_t>(willie, kCurrentPart);
    if (!combat::InitiateAccepted(before, after, current ? *current : 0xFF, c.part)) {
        DestroyBoxes(a);
        return before ? Run::InProcess : Run::Refused;
    }
    due_.push_back(DueDelayed{nowMs + combat::kSeverDelayedMs, std::move(a), c.victim, standIn, c.claimId, c});
    return Run::Accepted;
}

// The claim's arguments on willie: its mesh and marker components by name, and two fresh VP boxes placed on its own
// frame bone and attached to it as Call_Dismember attaches its own (kept for the fighter's life: Delayed and the limb
// spawn read them later through "Dism VP Collision Box 1/2"). Collision is off, they serve as geometry only.
bool Severs::Rebuild(UObject* willie, const SeverClaim& c, Rebuilt& out) {
    out = Rebuilt{};
    const std::vector<UObject*> comps = Components(willie);
    auto named = [&comps](const std::string& name) -> UObject* {
        if (name.empty()) return nullptr;
        for (UObject* o : comps)
            if (o && SameNameAscii(ObjectName(o), name)) return o;
        return nullptr;
    };
    UObject* mesh = c.masterMesh.empty() ? nullptr : named(c.masterMesh);
    if (!mesh) mesh = adapter_->Mesh(willie);
    Xform bone;
    if (!mesh || !PeerBoneOk(c.frameBone, c.attacker) || !BoneXform(mesh, c.frameBone, bone)) {
        ++n_.noBone;
        if (n_.noBone <= 3) HSM_LOG("severs: #%u from e%u names frame bone '%s', which e%u lacks", c.claimId, c.attacker, c.frameBone.c_str(), c.victim);
        return false;
    }
    UObject* attach = named(c.attachMarker);
    if (!attach) {
        ++n_.noMarker;
        if (n_.noMarker <= 3) HSM_LOG("severs: #%u from e%u names marker '%s', which e%u lacks", c.claimId, c.attacker, c.attachMarker.c_str(), c.victim);
        return false;
    }
    for (const std::string& m : c.markers)
        if (UObject* o = named(m)) out.markers.push_back(RefOf(o));
    if (out.markers.empty()) out.markers.push_back(RefOf(attach));   // Delayed divides by the marker count
    UObject* box1 = NewBox(willie, mesh, c.frameBone, combat::SeverBoxWorld(c.box1, bone), c.box1.extent);
    UObject* box2 = box1 ? NewBox(willie, mesh, c.frameBone, combat::SeverBoxWorld(c.box2, bone), c.box2.extent) : nullptr;
    if (!box2) {
        if (box1) Call(box1, "K2_DestroyComponent").set<UObject*>("Object", willie).invoke();
        ++n_.noBox;
        return false;
    }
    UObject* weapon = WeaponPart(c.attacker, c.source, c.weaponComponent);
    out.willie = RefOf(willie);
    out.mesh = RefOf(mesh);
    out.attach = RefOf(attach);
    out.box1 = RefOf(box1);
    out.box2 = RefOf(box2);
    out.weapon = RefOf(weapon ? weapon : box1);   // never null: Delayed disables the limb's collision against it
    out.part = c.part;
    return true;
}

// One of the two functions with a's arguments, under ScriptHookBypass (our own call passes our hooks, and every call it
// makes passes the claims hooks). On a stand-in dismemberment is allowed for exactly this call: both functions read
// "Force Disable Dismemberment" only at their own gates (Willie_BP.cpp:3255, 3652).
bool Severs::Invoke(const char* function, const Rebuilt& a, bool standIn) {
    UObject* w = a.willie.obj;
    std::vector<UObject*> markers;
    for (const Ref& m : a.markers)
        if (Live(m)) markers.push_back(m.obj);
    UObject* attach = Live(a.attach) ? a.attach.obj : a.box1.obj;
    if (markers.empty()) markers.push_back(attach);
    UObject* weapon = Live(a.weapon) ? a.weapon.obj : a.box1.obj;
    // Overlapped Markers is a by-reference array both functions only read: it points at our vector for the call.
    const FObjArray arr{markers.data(), int32_t(markers.size()), int32_t(markers.size())};
    Call call(w, function);
    if (!call.valid()) return false;
    call.set<UObject*>("Master Mesh", a.mesh.obj)
        .set<uint8_t>("Dismembered Part", a.part)
        .set<UObject*>("Attach Marker", attach)
        .set<FObjArray>("Overlapped Markers", arr)
        .set<UObject*>("VP Box Collision 1", a.box1.obj)
        .set<UObject*>("VP Box Collision 2", a.box2.obj)
        .set<UObject*>("Dismember Weapon", weapon);
    bool ok = false;
    {
        ScriptHookBypass bypass;
        if (standIn) SetBool(w, kNoDismember, false);
        ok = call.invoke();
        if (standIn) SetBool(w, kNoDismember, true);
    }
    return ok;
}

void Severs::CallDelayed(DueDelayed& d) {
    const Rebuilt& a = d.args;
    const char* lost = !Live(a.willie) ? "the fighter is gone" : !Live(a.mesh) ? "its mesh is gone" : !Live(a.box1) ? "VP box 1 is gone"
                     : !Live(a.box2) ? "VP box 2 is gone" : !Invoke(kDelayed, a, d.standIn) ? "the function could not be called" : nullptr;
    if (lost) {
        if (++n_.delayedLost <= kLogFirst) HSM_LOG("severs: #%u Delayed on e%u lost: %s", d.claimId, d.entity, lost);
        if (d.standIn && env_.nativeLost) env_.nativeLost(d.entity, a.part);   // the gore fallback hides it instead
        // Without its Delayed the fighter would keep "Dismemberment In Process" for good.
        if (Live(a.willie)) SetBool(a.willie.obj, kInProcess, false);
        return;
    }
    ++n_.delayed;
    if (!d.standIn) {
        // Replay bypasses the native post-hook, so OwnCutDone cannot announce
        // this accepted cut. Send the owner's confirmed geometry reliably too.
        const bool confirmed = (ReadSeveredMask(a.willie.obj) & (1u << a.part)) != 0;
        HSM_LOG("severs: owner e%u part %u Delayed completed; severed bit %s", d.entity, a.part, confirmed ? "set" : "missing");
        if (confirmed && env_.sendNotice) {
            SeverClaim notice = d.claim;
            notice.attacker = notice.victim;
            notice.claimId = ++nextClaimId_[notice.victim];
            notice.attackerTsMs = streams_->ClockMs(SteadyNowMs());
            notice.source = StrikeSource::Body;
            notice.weaponComponent.clear();
            env_.sendNotice(notice);
            ++n_.noticed;
        }
    }
}

void Severs::DestroyBoxes(const Rebuilt& a) {
    for (const Ref* b : {&a.box1, &a.box2})
        if (Live(*b) && Live(a.willie)) Call(b->obj, "K2_DestroyComponent").set<UObject*>("Object", a.willie.obj).invoke();
}

// ---------------------------------------------------------------- the victim's owner

void Severs::Replay(const SeverClaim& c, double nowMs) {
    if (!active_) return;
    if (!dedupe_.Accept(c.attacker, c.claimId)) {
        ++n_.dupes;
        return;
    }
    int64_t ownerNow = 0;
    if (standIns_->OwnerClockNow(c.attacker, nowMs, ownerNow) && !combat::ClaimFresh(ownerNow - int64_t(c.attackerTsMs))) {
        ++n_.stale;
        if (n_.stale <= 5)
            HSM_LOG("severs: dropped #%u from e%u: %lld ms old", c.claimId, c.attacker,
                    static_cast<long long>(ownerNow - int64_t(c.attackerTsMs)));
        return;
    }
    Mine(c, nowMs, false);
}

void Severs::Mine(const SeverClaim& c, double nowMs, bool retry) {
    UObject* me = env_.ownedActor ? env_.ownedActor(c.victim) : nullptr;
    if (!me) {
        ++n_.noVictim;   // between lives: the cut has nothing to land on
        return;
    }
    switch (Begin(me, c, nowMs, false)) {
    case Run::Accepted:
        ++n_.replayed;
        if (n_.replayed <= kLogFirst)
            HSM_LOG("severs: replayed #%u e%u -> e%u part %u frame %s%s", c.claimId, c.attacker, c.victim, c.part,
                    c.frameBone.c_str(), retry ? " (after the sever in process)" : "");
        return;
    case Run::InProcess:
        if (!retry && retry_.size() < kMaxRetries) {
            retry_.push_back(Retry{c, nowMs + kRetryMs, false, false});
            return;
        }
        [[fallthrough]];
    case Run::Refused:
        ++n_.refused;   // the part is gone already, armour blocks it, or the armour is being set up
        if (n_.refused <= 5) HSM_LOG("severs: #%u e%u -> e%u part %u refused by my fighter's Initiate", c.claimId, c.attacker, c.victim, c.part);
        return;
    case Run::Failed:
        return;   // counted where it failed
    }
}

// Retries wait for the fighter's own sever to finish ("Dismemberment In Process" clears 0.5 s after its Delayed).
void Severs::RunRetries(double nowMs) {
    for (size_t i = 0; i < retry_.size();) {
        Retry& r = retry_[i];
        UObject* w = r.standIn ? standIns_->ActorOf(r.claim.victim) : (env_.ownedActor ? env_.ownedActor(r.claim.victim) : nullptr);
        bool busy = false;
        if (w && nowMs < r.untilMs && GetBool(w, kInProcess, busy) && busy) {
            ++i;
            continue;
        }
        const Retry take = std::move(r);
        retry_.erase(retry_.begin() + static_cast<std::ptrdiff_t>(i));
        if (!w || nowMs >= take.untilMs) {
            ++n_.retryLost;
            continue;
        }
        // A stand-in shows a part natively only as late as a late claim may (the gore fallback shows it after that).
        if (take.standIn && !combat::NativeSeverDisplayFresh(take.claim.part, nowMs - bitAtMs_[take.claim.victim][take.claim.part])) {
            ++n_.retryLost;
            continue;
        }
        ++n_.retried;
        if (take.standIn) Show(take.claim, nowMs, take.late, true);
        else Mine(take.claim, nowMs, true);
    }
}

// ---------------------------------------------------------------- everyone else: shown when the owner confirms it

void Severs::Remember(const SeverClaim& c, double nowMs) {
    if (!active_ || !ValidEntity(c.victim) || c.part >= combat::kSeverParts) return;
    ++n_.remembered;
    pending_.Put(c, nowMs);
    // The owner's vitals may have reported the part already (they travel unreliably and faster than a resent claim).
    const uint32_t bit = 1u << c.part;
    if ((severed_[c.victim] & bit) && !(shownMask_[c.victim] & bit) && combat::NativeSeverDisplayFresh(c.part, nowMs - bitAtMs_[c.victim][c.part])) {
        SeverClaim taken;
        if (pending_.Take(c.victim, c.part, nowMs, taken)) Show(taken, nowMs, true);
    }
}

void Severs::OnVitals(const VitalsMsg& v, double nowMs) {
    if (!active_ || !ValidEntity(v.entity)) return;
    const uint8_t e = v.entity;
    const uint32_t mask = v.severed & ((1u << combat::kSeverParts) - 1);
    const uint32_t was = severed_[e];
    if (was & ~mask) {   // a part came back: a new life
        shownMask_[e] &= mask;
        for (uint8_t p = 0; p < combat::kSeverParts; ++p)
            if (!(mask & (1u << p))) bitAtMs_[e][p] = 0;
    }
    severed_[e] = mask;
    const uint32_t newly = mask & ~was;
    for (uint8_t p = 0; p < combat::kSeverParts; ++p) {
        const uint32_t bit = 1u << p;
        if (!(newly & bit)) continue;
        bitAtMs_[e][p] = nowMs;
        if (shownMask_[e] & bit) continue;
        SeverClaim claim;
        if (pending_.Take(e, p, nowMs, claim)) Show(claim, nowMs, false);
        else ++n_.noClaim;   // the gore fallback shows it (or a claim arriving within kLateShowMs does)
    }
}

void Severs::Show(const SeverClaim& c, double nowMs, bool late, bool retry) {
    UObject* standIn = standIns_->ActorOf(c.victim);
    if (!standIn) {
        ++n_.showRefused;
        return;
    }
    switch (Begin(standIn, c, nowMs, true)) {
    case Run::Accepted:
        shownMask_[c.victim] |= 1u << c.part;
        ++(late ? n_.shownLate : n_.shown);
        if (env_.nativeShown) env_.nativeShown(c.victim, c.part);
        if (n_.shown + n_.shownLate <= kLogFirst)
            HSM_LOG("severs: shown #%u e%u -> e%u part %u on the stand-in%s%s", c.claimId, c.attacker, c.victim, c.part,
                    late ? " (claim after the vitals)" : "", retry ? " (after the sever in process)" : "");
        return;
    case Run::InProcess:
        if (!retry && retry_.size() < kMaxRetries) {
            retry_.push_back(Retry{c, nowMs + kRetryMs, true, late});
            return;
        }
        [[fallthrough]];
    case Run::Refused:
    case Run::Failed:
        ++n_.showRefused;
        return;
    }
}

void Severs::Forget(uint8_t entity) {
    if (!ValidEntity(entity)) return;
    limiter_.Forget(entity);
    pending_.Forget(entity);
    dedupe_.Forget(entity);   // its claim ids may start again from 1 (a restarted machine)
    severed_[entity] = 0;
    shownMask_[entity] = 0;
    bitAtMs_[entity] = {};
    for (auto it = due_.begin(); it != due_.end();) it = it->entity == entity ? due_.erase(it) : std::next(it);
    for (auto it = retry_.begin(); it != retry_.end();) it = it->claim.victim == entity ? retry_.erase(it) : std::next(it);
    for (const combat::StuckBlades::Key& k : blades_.Forget(entity)) Let(k.attacker, k.source);
    // A blade of ours stuck in that entity's stand-in: its owner is told it is free (the stand-in it was stuck in is gone).
    for (auto& [key, h] : hands_)
        if (h.stuck && (key.first == entity || h.victim == entity)) SendFreed(key.first, StrikeSource(key.second), h);
}

void Severs::LetGo(uint8_t victim) {
    if (!ValidEntity(victim)) return;
    for (const combat::StuckBlades::Key& k : blades_.Forget(victim)) Let(k.attacker, k.source);
}

// ---------------------------------------------------------------- stuck blades: the attacker

// The first live sticking of each hand of a fighter simulated here whose Hit Actor is a stand-in. Reported when it
// starts or changes, and freed when the hand has none.
void Severs::ScanOurBlades(uint8_t entity, UObject* willie) {
    if (!Alive(willie)) return;
    UObject* right = WeaponOf(willie, StrikeSource::WeaponRight);
    for (StrikeSource source : {StrikeSource::WeaponRight, StrikeSource::WeaponLeft}) {
        UObject* weapon = WeaponOf(willie, source);
        if (source == StrikeSource::WeaponLeft && weapon == right) weapon = nullptr;   // one two-handed weapon
        if (weapon && !Alive(weapon)) weapon = nullptr;   // dropped and destroyed: its hand has no stuck blade
        UObject* found = nullptr;
        uint8_t victim = 0;
        if (weapon)
            if (const auto* arr = Field<FObjArray>(weapon, "Stuck Constraints Array"))
                if (arr->data && arr->num > 0 && arr->num <= 256)
                    for (int32_t i = 0; i < arr->num && !found; ++i) {
                        UObject* k = arr->data[i];
                        if (!k || !adapter_->StillValid(k, ObjectIndex(k))) continue;
                        bool on = false;
                        UObject** hit = Field<UObject*>(k, "Hit Actor");
                        if (!GetBool(k, "Activate Constraint", on) || !on || !hit || !*hit) continue;
                        const uint8_t e = standIns_->EntityOf(*hit);
                        if (!e || e == entity || (env_.allowed && !env_.allowed(entity, e))) continue;
                        found = k;
                        victim = e;
                    }
        auto it = hands_.find({entity, uint8_t(source)});
        if (!found) {
            if (it != hands_.end() && it->second.stuck) SendFreed(entity, source, it->second);
            if (it != hands_.end()) it->second.bad = false;
            continue;
        }
        Hand& h = hands_[{entity, uint8_t(source)}];
        if ((h.stuck || h.bad) && h.actor.obj == found && h.victim == victim && Live(h.actor)) continue;
        if (h.stuck) SendFreed(entity, source, h);
        SendStuck(entity, source, h, found, victim);
    }
}

void Severs::SendStuck(uint8_t entity, StrikeSource source, Hand& h, UObject* constraint, uint8_t victim) {
    UObject** comp = Field<UObject*>(constraint, "Component 1 (Weapon)");
    UObject** body = Field<UObject*>(constraint, "Component 2 (Body)");
    const FName* boneName = Field<FName>(constraint, "Bone Name 2");
    UObject* standIn = FighterActor(victim);
    UObject* mesh = (body && *body) ? *body : (standIn ? adapter_->Mesh(standIn) : nullptr);
    if (comp && !Alive(*comp)) comp = nullptr;   // the constraint may name a component the game has since destroyed
    if (mesh && !Alive(mesh)) mesh = nullptr;
    StuckBladeMsg m;
    m.attacker = entity;
    m.victim = victim;
    m.source = source;
    m.stuck = true;
    m.bone = boneName ? NameToString(*boneName) : std::string{};
    Xform bone, blade;
    if (!comp || !*comp || !mesh || m.bone.empty() || m.bone == "None" || !BoneXform(mesh, m.bone, bone) ||
        !ComponentXform(*comp, blade)) {
        ++n_.stuckBad;
        h = Hand{false, true, h.seq, victim, RefOf(constraint), {}};   // not retried every frame
        return;
    }
    m.weaponComponent = ObjectName(*comp);
    m.weaponInBone = InBoneFrame(bone, blade);
    if (m.weaponComponent.size() > combat::kMaxClaimString || m.bone.size() > combat::kMaxClaimString ||
        !combat::detail::LocalXformOk(m.weaponInBone)) {
        ++n_.stuckBad;
        h = Hand{false, true, h.seq, victim, RefOf(constraint), {}};
        return;
    }
    m.seq = ++nextStuckSeq_;
    env_.sendStuck(m);
    h = Hand{true, false, m.seq, victim, RefOf(constraint), m.bone};
    ++n_.stuckSent;
    if (n_.stuckSent <= kLogFirst)
        HSM_LOG("severs: blade of e%u (%s '%s') stuck in e%u bone %s, seq %u", entity, SourceName(source),
                m.weaponComponent.c_str(), victim, m.bone.c_str(), m.seq);
}

void Severs::SendFreed(uint8_t entity, StrikeSource source, Hand& h) {
    StuckBladeMsg m;
    m.attacker = entity;
    m.victim = h.victim;
    m.source = source;
    m.stuck = false;
    m.seq = h.seq;
    env_.sendStuck(m);
    h.stuck = false;
    ++n_.stuckFreed;
}

// ---------------------------------------------------------------- stuck blades: the victim's owner

void Severs::OnStuck(const StuckBladeMsg& m, double nowMs) {
    if (!active_ || !env_.ownedActor || !env_.ownedActor(m.victim)) return;
    switch (blades_.On(m, nowMs)) {
    case combat::StuckBlades::Change::Attach: Hold(m); break;
    case combat::StuckBlades::Change::Reattach: Let(m.attacker, m.source); Hold(m); break;
    case combat::StuckBlades::Change::Release: Let(m.attacker, m.source); break;
    case combat::StuckBlades::Change::None: break;
    }
}

// The attacker's stand-in weapon held to my bone where the attacker sees it stuck: the weapon component is teleported
// to the reported pose in my bone's frame, then a PhysicsConstraintComponent joins it to the bone with the game's own
// stuck limits, and the blade's collision with my fighter is turned off as the game's own stuck constraint does.
void Severs::Hold(const StuckBladeMsg& m) {
    UObject* me = env_.ownedActor(m.victim);
    if (!Alive(me)) me = nullptr;
    UObject* mesh = me ? adapter_->Mesh(me) : nullptr;
    UObject* blade = WeaponPart(m.attacker, m.source, m.weaponComponent);
    if (!Alive(mesh)) mesh = nullptr;
    if (!Alive(blade)) blade = nullptr;
    Xform bone;
    if (!mesh || !blade || !constraintClass_ || !PeerBoneOk(m.bone, m.attacker) || !BoneXform(mesh, m.bone, bone)) {
        ++n_.holdFailed;
        if (n_.holdFailed <= 3)
            HSM_LOG("severs: cannot hold the blade of e%u (%s '%s') in bone %s", m.attacker, SourceName(m.source),
                    m.weaponComponent.c_str(), m.bone.c_str());
        return;
    }
    // The stand-in's own weapon may be natively stuck in me already (its sticking ran on this machine too).
    if (UObject* weapon = OuterOf(blade))
        if (const auto* arr = Field<FObjArray>(weapon, "Stuck Constraints Array"))
            if (arr->data && arr->num > 0 && arr->num <= 256)
                for (int32_t i = 0; i < arr->num; ++i) {
                    UObject* k = arr->data[i];
                    bool on = false;
                    UObject** hit = k && adapter_->StillValid(k, ObjectIndex(k)) ? Field<UObject*>(k, "Hit Actor") : nullptr;
                    if (hit && *hit == me && GetBool(k, "Activate Constraint", on) && on) {
                        ++n_.alreadyStuck;
                        return;
                    }
                }
    const Xform at = FromBoneFrame(bone, m.weaponInBone);
    Call(blade, "K2_SetWorldTransform").set<FTransform>("NewTransform", ToF(at)).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
    const FTransform identity{};
    Call add(me, "AddComponentByClass");
    add.set<UClass*>("Class", constraintClass_).set<bool>("bManualAttachment", false).set<FTransform>("RelativeTransform", identity)
        .set<bool>("bDeferredFinish", true);
    UObject* joint = add.invoke() ? add.get<UObject*>("ReturnValue") : nullptr;
    if (!joint) {
        ++n_.holdFailed;
        return;
    }
    Call(me, "FinishAddComponent").set<UObject*>("Component", joint).set<bool>("bManualAttachment", false)
        .set<FTransform>("RelativeTransform", identity).invoke();
    Call(joint, "K2_SetWorldTransform").set<FTransform>("NewTransform", ToF(at)).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
    // Constraint_Weapon_Stuck_BP.cpp:597-602: every angular motion locked; linear motion limited on one axis and locked on
    // the other two, which axis depending on the thrust flag the report does not carry: limited on all three here.
    Call(joint, "SetLinearXLimit").set<uint8_t>("ConstraintType", 1).setReal("LimitSize", kStuckSlackUu).invoke();   // LCM_Limited
    Call(joint, "SetLinearYLimit").set<uint8_t>("ConstraintType", 1).setReal("LimitSize", kStuckSlackUu).invoke();
    Call(joint, "SetLinearZLimit").set<uint8_t>("ConstraintType", 1).setReal("LimitSize", kStuckSlackUu).invoke();
    Call(joint, "SetAngularSwing1Limit").set<uint8_t>("MotionType", 2).setReal("Swing1LimitAngle", 0).invoke();   // ACM_Locked
    Call(joint, "SetAngularSwing2Limit").set<uint8_t>("MotionType", 2).setReal("Swing2LimitAngle", 0).invoke();
    Call(joint, "SetAngularTwistLimit").set<uint8_t>("ConstraintType", 2).setReal("TwistLimitAngle", 0).invoke();
    Call(joint, "SetDisableCollision").set<bool>("bDisableCollision", true).invoke();
    const FName struck = MakeName(m.bone);
    Call(joint, "SetConstrainedComponents")
        .set<UObject*>("Component1", blade)
        .setName("BoneName1", FName{})
        .set<UObject*>("Component2", mesh)
        .setName("BoneName2", struck)
        .invoke();
    Joint j;
    j.constraint = RefOf(joint);
    j.owner = RefOf(me);
    j.blade = RefOf(blade);
    j.mesh = RefOf(mesh);
    if (UObject** skeleton = Field<UObject*>(me, "SK_Skeleton"); skeleton && *skeleton) {
        j.skeleton = RefOf(*skeleton);
        const FNameArray* rigid = Field<FNameArray>(me, "Rigid Bones");
        if (rigid && rigid->data && rigid->num > 0 && rigid->num <= kMaxBoneNames) j.rigid.assign(rigid->data, rigid->data + rigid->num);
    }
    // "Disable Nearest Collisions" is never set where a weapon spawns its stuck constraint (ModularWeaponBP.cpp:733-760):
    // the class default decides. Unreadable, it is taken as on: a blade pushing on the neighbouring bones would shove my
    // fighter, while one passing through them is barely seen.
    if (nearDefault_ < 0)
        if (UClass* stuck = FindClass("Constraint_Weapon_Stuck_BP_C"))
            if (UObject* cdo = DefaultObject(stuck)) {
                bool on = false;
                if (GetBool(cdo, "Disable Nearest Collisions", on)) {
                    nearDefault_ = on ? 1 : 0;
                    HSM_LOG("severs: the game's stuck blades %s the bones near the struck one (class default)",
                            on ? "ignore" : "keep colliding with");
                }
            }
    if (nearDefault_ != 0) j.near = NearBones(mesh, struck);
    Collide(j, true);
    const bool collisionOff = (j.rigid.empty() || j.rigidOff) && (j.near.empty() || j.nearOff) && j.skeleton.obj;
    if (!collisionOff) ++n_.collisionKept;
    const size_t rigidCount = j.rigid.size(), nearCount = j.near.size();
    joints_[{m.attacker, uint8_t(m.source)}] = std::move(j);
    // Item 8: what the blade's Stuck writes on its victim (ModularWeaponBP.cpp:717-721): a just-grabbed fighter, whose
    // limb joints the dislocation check makes unbreakable a moment.
    SetBool(me, "Was Just Grabbed", true);
    Call(me, "Event Is Grabbed").set<bool>("Disable Weapons Momentary", false).invoke();
    Call(me, "Event Check Bone Dislocation Status").invoke();
    ++n_.held;
    if (n_.held <= kLogFirst)
        HSM_LOG("severs: holding the blade of e%u (%s '%s') in my bone %s, seq %u; its collision with %zu rigid and %zu "
                "near bone(s) %s", m.attacker, SourceName(m.source), m.weaponComponent.c_str(), m.bone.c_str(), m.seq,
                rigidCount, nearCount, collisionOff ? "off" : "NOT all off");
}

void Severs::Let(uint8_t attacker, StrikeSource source) {
    auto it = joints_.find({attacker, uint8_t(source)});
    if (it == joints_.end()) return;
    Joint j = std::move(it->second);
    joints_.erase(it);
    if (Live(j.constraint) && Live(j.owner)) {
        Call(j.constraint.obj, "BreakConstraint").invoke();
        Call(j.constraint.obj, "K2_DestroyComponent").set<UObject*>("Object", j.owner.obj).invoke();
    }
    Collide(j, false);   // as the game lets a stuck blade go (Constraint_Weapon_Stuck_BP.cpp:812-825): after the break
    ++n_.released;
}

// The blade's collision with the fighter turned off (or on again) as Constraint_Weapon_Stuck_BP.cpp:499-500 does it:
// against the Willie's SK_Skeleton "Rigid Bones" and against its Mesh bones near the struck one, never expiring (-1).
// Only a list whose call went through is turned on again; nothing is called with a component that died.
void Severs::Collide(Joint& j, bool off) {
    if (!collisionLib_)
        if (UClass* lib = FindClass("CollisionDisablerFunctionLibrary")) collisionLib_ = DefaultObject(lib);
    auto one = [&](const Ref& body, const std::vector<FName>& bones, bool& isOff) {
        if (isOff == off || bones.empty() || !collisionLib_ || !Live(j.blade) || !Live(body)) return;
        const FNameArray names{bones.data(), static_cast<int32_t>(bones.size()), static_cast<int32_t>(bones.size())};
        bool done = false;
        if (off) {
            done = Call(collisionLib_, "DisableCollision_SkeletalVsSingleBody").set<UObject*>("SingleBody", j.blade.obj)
                       .set<UObject*>("SkeletalMesh", body.obj).set<FNameArray>("SkeletalMeshBones", names)
                       .setReal("TimeToExpiration", -1).invoke();
        } else {
            done = Call(collisionLib_, "EnableCollision_SkeletalVsSingleBody").set<UObject*>("SingleBody", j.blade.obj)
                       .set<UObject*>("SkeletalMesh", body.obj).set<FNameArray>("SkeletalMeshBones", names).invoke();
        }
        if (done) isOff = off;
    };
    one(j.skeleton, j.rigid, j.rigidOff);
    one(j.mesh, j.near, j.nearOff);
}

// Constraint_Weapon_Stuck_BP.cpp:473-495 with Disable Nearest Collisions: every socket name whose parent bone is the
// struck one, or, when the struck bone is a spine or clavicle bone, every name containing "spine"; then the struck bone's
// parent; never the struck bone itself.
std::vector<FName> Severs::NearBones(UObject* mesh, FName bone) const {
    std::vector<FName> out;
    PreparedCall parent(ClassOf(mesh), "GetParentBone", {"BoneName", "ReturnValue"});
    Call all(mesh, "GetAllSocketNames");
    if (!parent.valid() || !all.invoke()) return out;
    FProperty* names = FindProperty(all.function(), "ReturnValue");
    void* value = all.paramPtr("ReturnValue");
    if (!names || !value) return out;
    auto parentOf = [&](FName n) { return parent.put(0, n).invoke(mesh) ? parent.take<FName>(1) : FName{}; };
    auto add = [&](FName n) {
        if (IsNone(n) || SameName(n, bone)) return;
        for (const FName& o : out)
            if (SameName(o, n)) return;
        out.push_back(n);
    };
    const std::string struck = NameToString(bone);
    const bool spine = HasText(struck, "spine") || HasText(struck, "clavicle");
    const FNameArray* arr = static_cast<const FNameArray*>(value);
    if (arr->data && arr->num > 0 && arr->num <= kMaxBoneNames)
        for (int32_t i = 0; i < arr->num; ++i) {
            const FName n = arr->data[i];
            if (SameName(parentOf(n), bone) || (spine && HasText(NameToString(n), "spine"))) add(n);
        }
    add(parentOf(bone));
    ClearPropertyValue(names, value);   // the engine filled the array: its own ClearValue releases it
    return out;
}

uint32_t Severs::HeldWeapons(uint8_t attacker) const {
    uint32_t slots = 0;
    for (const auto& [key, j] : joints_)
        if (key.first == attacker && Live(j.constraint)) {
            if (key.second == uint8_t(StrikeSource::WeaponRight)) slots |= 1u;   // the rig's slot 0, "Weapon R"
            else if (key.second == uint8_t(StrikeSource::WeaponLeft)) slots |= 2u;   // slot 1, "Weapon L"
        }
    return slots;
}

// The stand-in bone my lodged blade holds (and its parent) is left to the blade rather than the servo (StandIns' held
// bones): a bone driven by velocity with no force limit pulled the blade, my hand and me after the owner's every move
// (#70, #76). Stuck blades are scanned every tick (ScanOurBlades), so a freed one drops out at once.
uint32_t Severs::StuckBones(uint8_t victim) const {
    uint32_t bones = 0;
    for (const auto& [key, h] : hands_)
        if (h.stuck && h.victim == victim && Live(h.actor)) bones |= sync::YieldBones(sync::PoseBoneIndex(h.bone), false);
    return bones;
}

// ---------------------------------------------------------------- lookups

UObject* Severs::FighterActor(uint8_t entity) const {
    UObject* s = standIns_->ActorOf(entity);
    if (!s && env_.ownedActor) s = env_.ownedActor(entity);
    return Alive(s) ? s : nullptr;
}

UObject* Severs::WeaponOf(UObject* fighter, StrikeSource source) const {
    if (!fighter || source == StrikeSource::Body) return nullptr;
    UObject** w = Field<UObject*>(fighter, source == StrikeSource::WeaponLeft ? "Weapon L" : "Weapon R");
    return w ? *w : nullptr;
}

// The named primitive component of entity's weapon in that hand, else the weapon's root when it is a primitive.
UObject* Severs::WeaponPart(uint8_t entity, StrikeSource source, const std::string& component) const {
    UObject* weapon = WeaponOf(FighterActor(entity), source);
    if (!Alive(weapon)) return nullptr;
    if (!component.empty())
        for (UObject* o : Components(weapon))
            if (o && SameNameAscii(ObjectName(o), component)) return o;
    Call root(weapon, "K2_GetRootComponent");
    UObject* r = root.invoke() ? root.get<UObject*>("ReturnValue") : nullptr;
    return Alive(r) && primitive_ && IsA(r, primitive_) ? r : nullptr;
}

UObject* Severs::NewBox(UObject* willie, UObject* mesh, const std::string& bone, const Xform& world, const V3& extent) {
    if (!boxClass_) return nullptr;
    const FTransform identity{};
    Call add(willie, "AddComponentByClass");
    add.set<UClass*>("Class", boxClass_).set<bool>("bManualAttachment", false).set<FTransform>("RelativeTransform", identity)
        .set<bool>("bDeferredFinish", true);
    UObject* box = add.invoke() ? add.get<UObject*>("ReturnValue") : nullptr;
    if (!box) return nullptr;
    // Collision is off before the component is registered, so it never overlaps anything, even once.
    Call(box, "SetCollisionEnabled").set<uint8_t>("NewType", 0).invoke();   // ECollisionEnabled::NoCollision
    Call(box, "SetGenerateOverlapEvents").set<bool>("bInGenerateOverlapEvents", false).invoke();
    Call(willie, "FinishAddComponent").set<UObject*>("Component", box).set<bool>("bManualAttachment", false)
        .set<FTransform>("RelativeTransform", identity).invoke();
    Call(box, "K2_SetWorldTransform").set<FTransform>("NewTransform", ToF(world)).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
    Call(box, "SetBoxExtent").set<FVector>("InBoxExtent", ToF(extent)).set<bool>("bUpdateOverlaps", false).invoke();
    // As Call_Dismember attaches its boxes: to the struck bone, keeping the world transform (EAttachmentRule::KeepWorld).
    Call(box, "K2_AttachToComponent")
        .set<UObject*>("Parent", mesh)
        .setName("SocketName", MakeName(bone))
        .set<uint8_t>("LocationRule", 1)
        .set<uint8_t>("RotationRule", 1)
        .set<uint8_t>("ScaleRule", 1)
        .set<bool>("bWeldSimulatedBodies", false)
        .invoke();
    return box;
}

// M7: the FName pool never shrinks, so a frame or struck bone off the wire is interned (here, in NewBox and in Hold) only
// when it is a plain bone name (letters, digits, underscore, at most 64). Counted, and logged sparingly.
bool Severs::PeerBoneOk(const std::string& bone, uint8_t from) {
    if (util::ValidBoneName(bone)) return true;
    if (++n_.badBone <= 3 || n_.badBone % 1000 == 0)
        HSM_LOG("severs: e%u names an invalid bone '%.64s' (%llu so far); dropped", from, util::StripControlChars(bone).c_str(),
                static_cast<unsigned long long>(n_.badBone));
    return false;
}

bool Severs::BoneXform(UObject* mesh, const std::string& bone, Xform& out) const {
    if (!mesh || bone.empty()) return false;
    const FName name = MakeName(bone);
    Call index(mesh, "GetBoneIndex");
    if (!index.setName("BoneName", name).invoke() || index.get<int32_t>("ReturnValue") < 0) return false;
    Call socket(mesh, "GetSocketTransform");
    socket.setName("InSocketName", name);   // TransformSpace stays 0 in the zeroed buffer: RTS_World
    if (!socket.invoke()) return false;
    const FTransform t = socket.get<FTransform>("ReturnValue");
    if (!Finite(t)) return false;
    out = ToXform(t);
    return true;
}

bool Severs::ComponentXform(UObject* comp, Xform& out) const {
    Call world(comp, "K2_GetComponentToWorld");
    if (!world.invoke()) return false;
    const FTransform t = world.get<FTransform>("ReturnValue");
    if (!Finite(t)) return false;
    out = ToXform(t);
    return true;
}

std::vector<UObject*> Severs::Components(UObject* actor) const {
    std::vector<UObject*> out;
    if (!actor || !primitive_ || !components_.valid()) return out;
    if (!components_.put(0, primitive_).invoke(actor)) return out;
    const FObjArray arr = components_.take<FObjArray>(1);
    if (!arr.data || arr.num <= 0 || arr.num > 512) return out;
    out.assign(arr.data, arr.data + arr.num);
    return out;
}

std::string Severs::HealthLine() {
    const Counters& a = n_;
    const Counters& b = reported_;
    auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
    char line[900];
    std::snprintf(line, sizeof line,
                  "severs: captured %llu (limited %llu friendly %llu invalid %llu) | replayed %llu (refused %llu retried "
                  "%llu lost %llu dupes %llu stale %llu novictim %llu nomarker %llu nobone %llu nobox %llu) delayed %llu "
                  "(lost %llu) | remembered %llu shown %llu late %llu refused %llu noclaim %llu | blocked: on stand-ins "
                  "%llu, on mine %llu | call failed %llu | stuck sent %llu freed %llu bad %llu, held %llu released %llu "
                  "failed %llu native %llu broken %llu collision kept %llu | noticed %llu (invalid %llu) | bad bone names %llu",
                  d(a.captured, b.captured), d(a.limited, b.limited), d(a.friendly, b.friendly), d(a.invalid, b.invalid),
                  d(a.replayed, b.replayed), d(a.refused, b.refused), d(a.retried, b.retried), d(a.retryLost, b.retryLost),
                  d(a.dupes, b.dupes), d(a.stale, b.stale), d(a.noVictim, b.noVictim), d(a.noMarker, b.noMarker),
                  d(a.noBone, b.noBone), d(a.noBox, b.noBox), d(a.delayed, b.delayed), d(a.delayedLost, b.delayedLost),
                  d(a.remembered, b.remembered), d(a.shown, b.shown), d(a.shownLate, b.shownLate),
                  d(a.showRefused, b.showRefused), d(a.noClaim, b.noClaim), d(a.blockedStandIn, b.blockedStandIn),
                  d(a.blockedOnMine, b.blockedOnMine), d(a.callFailed, b.callFailed), d(a.stuckSent, b.stuckSent), d(a.stuckFreed, b.stuckFreed),
                  d(a.stuckBad, b.stuckBad), d(a.held, b.held), d(a.released, b.released), d(a.holdFailed, b.holdFailed),
                  d(a.alreadyStuck, b.alreadyStuck), d(a.broken, b.broken), d(a.collisionKept, b.collisionKept),
                  d(a.noticed, b.noticed), d(a.noticeInvalid, b.noticeInvalid), d(a.badBone, b.badBone));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game
