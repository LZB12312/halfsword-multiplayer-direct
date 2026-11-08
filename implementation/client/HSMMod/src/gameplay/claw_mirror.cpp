#include "gameplay/claw_mirror.h"
#include "log.h"
#include "match/hsm_claw_mirror.h"
#include "net/direct_api.h"
#include "ue/engine.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <utility>

namespace hsm::game {
using namespace hsm::ue;
namespace {

constexpr const char* kClawTag = "Logan_Claw";   // Half Wolverine's tag on its blades (claws/cosmetic.lua)
constexpr const char* kBoneMeshPath = "/Game/Mods/LoganClaws/SM_LoganBlade_Bone.SM_LoganBlade_Bone";
constexpr const char* kMetalMeshPath = "/Game/Mods/LoganClaws/SM_LoganBlade_Adamantium.SM_LoganBlade_Adamantium";
// In the base game, so a player without LoganClaws_P.pak still sees that someone has claws out.
constexpr const char* kFallbackMeshPath = "/Game/Assets/Weapons/Messers/Langmessers/Meshes/Blades/SM_Weapon_Messer_Langmesser_Blade_CC_001.SM_Weapon_Messer_Langmesser_Blade_CC_001";
constexpr const char* kBoneMaterialPath = "/Game/Assets/Props/Cadaver/Bones/Materials/MI_Prop_Cadaver_Bones_001.MI_Prop_Cadaver_Bones_001";
constexpr uint8_t kMovable = 2, kNoCollision = 0, kKeepRelative = 0;
constexpr uint32_t kOwnScanEveryMs = 3000, kPublishEveryMs = 500, kDrawEveryMs = 500;

uint32_t Now() { return uint32_t(GetTickCount64()); }
bool Due(uint32_t now, uint32_t at) { return int32_t(now - at) >= 0; }

UObject* RootOf(UObject* actor) {
    Call c(actor, "K2_GetRootComponent");
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

UObject* LoadObjectBlocking(const std::string& path) {
    UObject* ksl = nullptr;
    if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    if (!ksl) return nullptr;
    FStringArgHolder str(path);
    Call make(ksl, "MakeSoftObjectPath");
    make.set("PathString", str.arg);
    if (!make.valid() || !make.invoke()) return nullptr;
    Call conv(ksl, "Conv_SoftObjPathToSoftObjRef");
    void* softPath = make.paramPtr("ReturnValue");
    if (!conv.valid() || !softPath) return nullptr;
    conv.setFrom("SoftObjectPath", softPath);
    if (!conv.invoke()) return nullptr;
    Call load(ksl, "LoadAsset_Blocking");
    void* softRef = conv.paramPtr("ReturnValue");
    if (!load.valid() || !softRef) return nullptr;
    load.setFrom("Asset", softRef);
    return load.invoke() ? load.get<UObject*>("ReturnValue") : nullptr;
}

UObject* CachedAsset(const char* path) {
    static std::map<std::string, std::pair<UObject*, int32_t>> cache;
    auto it = cache.find(path);
    if (it != cache.end() && it->second.first && ObjectAt(it->second.second) == it->second.first) return it->second.first;
    UObject* o = LoadObjectBlocking(path);
    if (o) cache[path] = {o, ObjectIndex(o)};
    return o;
}

}  // namespace

void ClawMirror::Publish(Adapter& adapter, Lobby& lobby, UObject* mine, bool bone) {
    const uint32_t now = Now();
    if (!Due(now, nextPublish_)) return;
    nextPublish_ = now + kPublishEveryMs;
    UObject* const body = mine ? adapter.Mesh(mine) : nullptr;
    std::erase_if(ownBlades_, [&](const Spawned& s) { return !s.actor || ObjectAt(s.index) != s.actor; });
    if (ownBlades_.empty() && body && Due(now, nextOwnScan_)) {
        nextOwnScan_ = now + kOwnScanEveryMs;   // every scenery actor in the level: not each publish
        const FName tag = MakeName(kClawTag);
        for (UObject* a : FindInstances("StaticMeshActor")) {
            Call has(a, "ActorHasTag");
            has.setName("Tag", tag);
            if (has.invoke() && has.get<bool>("ReturnValue")) ownBlades_.push_back({a, ObjectIndex(a)});
        }
    }
    ClawState state;
    state.bone = bone;
    for (const Spawned& s : ownBlades_) {
        UObject* const root = RootOf(s.actor);
        if (!root || state.blades.size() >= kMaxClawBlades) continue;
        Call parent(root, "GetAttachParent");
        if (!parent.invoke() || parent.get<UObject*>("ReturnValue") != body) continue;   // an old fighter's blade
        bool hidden = true;
        GetBool(s.actor, "bHidden", hidden);
        if (hidden) continue;
        Call socket(root, "GetAttachSocketName");
        const FVector* const at = Field<FVector>(root, "RelativeLocation");
        const FRotator* const rot = Field<FRotator>(root, "RelativeRotation");
        if (!socket.invoke() || !at || !rot) continue;
        state.blades.push_back({NameToString(socket.get<FName>("ReturnValue")), at->x, at->y, at->z, rot->pitch, rot->yaw, rot->roll});
    }
    state.out = !state.blades.empty();
    bladesOut_ = state.out;
    const std::string value = EncodeClaws(state);
    if (value == published_) return;
    published_ = value;
    direct::SetMemberData(lobby.id(), kMemberClawKey, value);
    HSM_LOG("claws: my claws %s (%zu blades)", state.out ? "out" : "away", state.blades.size());
}

void ClawMirror::Remove(Adapter& adapter, Remote& r) {
    for (const Spawned& s : r.blades) if (s.actor && ObjectAt(s.index) == s.actor) adapter.DestroyActor(s.actor);
    r.blades.clear();
    r.fighter = nullptr;
}

void ClawMirror::Draw(Adapter& adapter, Remote& r, UObject* fighter) {
    const ClawState state = DecodeClaws(r.value);
    UObject* const body = fighter ? adapter.Mesh(fighter) : nullptr;
    UClass* const cls = FindClass("StaticMeshActor");
    UObject* mesh = state.out ? CachedAsset(state.bone ? kBoneMeshPath : kMetalMeshPath) : nullptr;
    if (state.out && !mesh) {
        if (!loggedNoMesh_) { loggedNoMesh_ = true; HSM_LOG("claws: LoganClaws_P.pak isn't loaded here, so other players' claws use a base-game blade"); }
        mesh = CachedAsset(kFallbackMeshPath);
    }
    if (!state.out || !body || !cls || !mesh) return;
    UObject* const material = state.bone ? CachedAsset(kBoneMaterialPath) : nullptr;
    for (const ClawBlade& b : state.blades) {
        UObject* const a = adapter.SpawnActorAt(cls, adapter.ActorLocation(fighter), FRotator{});
        if (!a) continue;
        r.blades.push_back({a, ObjectIndex(a)});
        UObject** const component = Field<UObject*>(a, "StaticMeshComponent");
        if (UObject* const c = component ? *component : nullptr) {
            Call(c, "SetMobility").set<uint8_t>("NewMobility", kMovable).invoke();
            Call(c, "SetStaticMesh").set<UObject*>("NewMesh", mesh).invoke();
            Call(c, "SetCollisionEnabled").set<uint8_t>("NewType", kNoCollision).invoke();
            if (material) Call(c, "SetMaterial").set<int32_t>("ElementIndex", 0).set<UObject*>("Material", material).invoke();
        }
        Call(a, "K2_AttachToComponent").set<UObject*>("Parent", body).setName("SocketName", MakeName(b.bone))
            .set<uint8_t>("LocationRule", kKeepRelative).set<uint8_t>("RotationRule", kKeepRelative).set<uint8_t>("ScaleRule", kKeepRelative).set<bool>("bWeldSimulatedBodies", false).invoke();
        Call(a, "K2_SetActorRelativeLocation").set<FVector>("NewRelativeLocation", FVector{b.x, b.y, b.z}).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
        Call(a, "K2_SetActorRelativeRotation").set<FRotator>("NewRelativeRotation", FRotator{b.pitch, b.yaw, b.roll}).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
    }
    r.fighter = fighter;
    HSM_LOG("claws: drew %zu blades on another player", r.blades.size());
}

void ClawMirror::Tick(Adapter& adapter, Lobby& lobby, UObject* mine, bool bone, const ActorOf& actorOf) {
    Publish(adapter, lobby, mine, bone);
    const uint32_t now = Now();
    if (!Due(now, nextDraw_)) return;
    nextDraw_ = now + kDrawEveryMs;
    std::vector<direct::UserId> here;
    for (const LobbyMember& m : lobby.members()) {
        if (m.id == direct::MyUserId()) continue;
        here.push_back(m.id);
        Remote& r = remotes_[m.id];
        const std::string value = direct::MemberData(lobby.id(), m.id, kMemberClawKey).substr(0, kMaxClawValue + 1);
        UObject* const fighter = actorOf(m.id);
        if (!fighter && !value.empty() && !r.loggedNoFighter) { r.loggedNoFighter = true; HSM_LOG("claws: %s has claws but no fighter here yet", m.name.c_str()); }
        if (value == r.value && fighter == r.fighter) continue;
        if (value.empty() && r.blades.empty()) { r.value = value; continue; }
        Remove(adapter, r);
        r.value = value;
        Draw(adapter, r, fighter);
        r.fighter = fighter;
    }
    for (auto it = remotes_.begin(); it != remotes_.end();) {
        if (std::find(here.begin(), here.end(), it->first) == here.end()) { Remove(adapter, it->second); it = remotes_.erase(it); }
        else ++it;
    }
}

void ClawMirror::Clear(Adapter& adapter) {
    for (auto& [id, r] : remotes_) Remove(adapter, r);
    remotes_.clear();
    ownBlades_.clear();
    published_.clear();
    bladesOut_ = false;
}

}  // namespace hsm::game
