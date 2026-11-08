#include "gameplay/hit_probe.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace hsm::ue;

namespace hsm::game {
namespace {

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };

constexpr int kCensusLines = 256;       // per stand-in per level: a component flipping on and off cannot fill the log
constexpr double kCensusBucketCm = 25;  // the steps a part's place is logged in
constexpr int32_t kMaxParts = 1024;     // a Willie has 300+ components
// ECollisionChannel: the engine's, then the game's (GameTraceChannelN is 13 + N, DefaultEngine.ini).
constexpr uint8_t kPawn = 2, kPhysicsBody = 5, kWeaponChannel = 14, kBodyCollision = 16, kSheathedWeapon = 28;

// The owner's class and the component, as the hit audit names them (claims.cpp); "none" for no component.
std::string PartName(UObject* comp) {
    if (!comp) return "none";
    UObject* outer = OuterOf(comp);
    return (outer ? ObjectName(reinterpret_cast<UObject*>(ClassOf(outer))) : std::string("?")) + "/" + ObjectName(comp);
}

bool BoneAt(UObject* comp, const char* bone, FVector& out) {
    Call c(comp, "GetSocketLocation");
    c.setName("InSocketName", MakeName(bone));
    if (!c.invoke()) return false;
    out = c.get<FVector>("ReturnValue");
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

double Between(const FVector& a, const FVector& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

combat::ProbeVec Vec(const FVector& v) { return {v.x, v.y, v.z}; }

struct BodyBones { FVector pelvis, head, left, right; };

bool ReadBones(UObject* mesh, BodyBones& b) {
    return BoneAt(mesh, "pelvis", b.pelvis) && BoneAt(mesh, "head", b.head) && BoneAt(mesh, "upperarm_l", b.left) &&
           BoneAt(mesh, "upperarm_r", b.right);
}

bool OnBody(const BodyBones& b, const FVector& at, combat::BodyOffset& out) {
    return combat::BodyFrameOffset(Vec(b.pelvis), Vec(b.head), Vec(b.left), Vec(b.right), Vec(at), out);
}

std::vector<UObject*> PartsOf(UObject* actor, UClass* cls) {
    std::vector<UObject*> parts;
    Call comps(actor, "K2_GetComponentsByClass");
    comps.set<UClass*>("ComponentClass", cls);
    if (!comps.invoke()) return parts;
    void* value = comps.paramPtr("ReturnValue");
    const FObjArray* arr = static_cast<const FObjArray*>(value);
    if (arr && arr->data && arr->num > 0 && arr->num <= kMaxParts) parts.assign(arr->data, arr->data + arr->num);
    if (FProperty* prop = FindProperty(comps.function(), "ReturnValue"); prop && value) ClearPropertyValue(prop, value);
    return parts;
}

UObject* ObjField(UObject* o, const char* name) { UObject** p = Field<UObject*>(o, name); return p ? *p : nullptr; }

// The stand-in, what is attached to it, and its weapons: a sheathed one hangs by a joint, so it is not attached.
std::vector<UObject*> CarriedBy(UObject* willie) {
    std::vector<UObject*> actors{willie};
    auto add = [&actors](UObject* a) { if (a && std::find(actors.begin(), actors.end(), a) == actors.end()) actors.push_back(a); };
    add(ObjField(willie, "Weapon R"));
    add(ObjField(willie, "Weapon L"));
    add(ObjField(willie, "Weapon Slot R 1"));
    add(ObjField(willie, "Weapon Slot R 2"));
    add(ObjField(willie, "Weapon Slot Back"));
    add(ObjField(willie, "Weapon Slot L 1"));
    add(ObjField(willie, "Weapon Slot L 2"));
    Call att(willie, "GetAttachedActors");
    att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", true);
    if (!att.invoke()) return actors;
    void* value = att.paramPtr("OutActors");
    const FObjArray* arr = static_cast<const FObjArray*>(value);
    if (arr && arr->data && arr->num > 0 && arr->num <= 256)
        for (int32_t i = 0; i < arr->num; ++i) add(arr->data[i]);
    if (FProperty* prop = FindProperty(att.function(), "OutActors"); prop && value) ClearPropertyValue(prop, value);
    return actors;
}

int Response(UObject* comp, uint8_t channel) {
    Call c(comp, "GetCollisionResponseToChannel");
    c.set<uint8_t>("Channel", channel);
    return c.invoke() ? c.get<uint8_t>("ReturnValue") : -1;
}

}  // namespace

void HitProbe::Clear() {
    offsets_.clear();
    census_.clear();
    primitive_ = nullptr;
}

const HitProbe::Offsets& HitProbe::OffsetsOf(UFunction* fn) {
    auto it = offsets_.find(fn);
    if (it != offsets_.end()) return it->second;
    auto offsetOf = [fn](const char* name, bool isStruct) {
        FProperty* p = FindProperty(fn, name);
        return p && (!isStruct || PropertyType(p) == "StructProperty") ? PropertyOffset(p) : -1;
    };
    Offsets o;
    o.hitComponent = offsetOf("HitComponent", false);
    o.otherComp = offsetOf("OtherComp", false);
    o.impulse = offsetOf("NormalImpulse", true);
    o.hit = offsetOf("Hit", true);
    return offsets_.emplace(fn, o).first->second;
}

bool HitProbe::ResolveHitResult() {
    if (hitResultRead_) return impactPoint_ >= 0;
    hitResultRead_ = true;
    UObject* hitResult = FindStruct("HitResult");
    FProperty* impact = StructMember(hitResult, "ImpactPoint");
    FProperty* bone = StructMember(hitResult, "BoneName");
    FProperty* myBone = StructMember(hitResult, "MyBoneName");
    if (!impact || !bone || !myBone) { HSM_LOG("probe: HitResult not found, contacts carry no point or bone"); return false; }
    impactPoint_ = PropertyOffset(impact);
    boneName_ = PropertyOffset(bone);
    myBoneName_ = PropertyOffset(myBone);
    return true;
}

void HitProbe::OnHit(uint8_t entity, UObject* willie, UObject* mesh, bool selfIsStandIn, bool down, UFunction* fn, void* params, double nowMs) {
    if (!on_) return;
    if (!budget_.Take(nowMs)) { tally_.Dropped(); return; }
    const Offsets& o = OffsetsOf(fn);
    if (o.hitComponent < 0 || o.otherComp < 0) return;
    auto* base = static_cast<uint8_t*>(params);
    UObject* self = *reinterpret_cast<UObject**>(base + o.hitComponent);
    UObject* other = *reinterpret_cast<UObject**>(base + o.otherComp);
    UObject* struck = selfIsStandIn ? self : other;
    UObject* by = selfIsStandIn ? other : self;

    FVector at{};
    FName selfBone{}, otherBone{};
    bool hasPoint = false;
    if (o.hit >= 0 && ResolveHitResult()) {
        const uint8_t* hit = base + o.hit;
        std::memcpy(&at, hit + impactPoint_, sizeof at);
        std::memcpy(&otherBone, hit + boneName_, sizeof otherBone);
        std::memcpy(&selfBone, hit + myBoneName_, sizeof selfBone);
        hasPoint = std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z);
    }
    const std::string struckBone = NameToString(selfIsStandIn ? selfBone : otherBone);

    bool measured = false;
    double offCm = 0;
    std::string nearest = "?";
    if (hasPoint && mesh) {
        Call c(mesh, "K2_GetClosestPointOnPhysicsAsset");
        c.set("WorldPosition", at);
        if (c.invoke() && c.get<bool>("ReturnValue")) {
            offCm = Between(c.get<FVector>("ClosestWorldPosition"), at);
            measured = std::isfinite(offCm);
            nearest = NameToString(c.get<FName>("BoneName"));
        }
    }

    const combat::ContactPlace place = combat::PlaceOfContact(struck == mesh, struck && OuterOf(struck) == willie, measured, offCm);
    const std::string part = PartName(struck);
    if (!tally_.Add(place, part + " bone " + struckBone, measured, offCm)) return;

    FVector impulse{};
    if (o.impulse >= 0) std::memcpy(&impulse, base + o.impulse, sizeof impulse);
    BodyBones bones;
    combat::BodyOffset b;
    char where[64] = "?";
    if (hasPoint && mesh && ReadBones(mesh, bones) && OnBody(bones, at, b))
        std::snprintf(where, sizeof where, "fwd %.0f right %.0f up %.0f", b.forward, b.right, b.up);
    char off[32] = "?";
    if (measured) std::snprintf(off, sizeof off, "%.1f cm", offCm);
    HSM_LOG("probe: e%u%s %s: %s bone %s by %s bone %s, %s from the body (nearest %s), at %s, impulse %.0f", entity, down ? " down" : "",
            combat::ContactPlaceName(place), part.c_str(), struckBone.c_str(), PartName(by).c_str(),
            NameToString(selfIsStandIn ? otherBone : selfBone).c_str(), off, nearest.c_str(), where, Between(impulse, FVector{}));
}

void HitProbe::Census(uint8_t entity, UObject* willie, UObject* mesh) {
    if (!on_ || !willie || !mesh) return;
    if (!primitive_) primitive_ = FindClass("PrimitiveComponent");
    if (!primitive_) return;
    BodyBones bones;
    if (!ReadBones(mesh, bones)) return;
    UObject* box = ObjField(willie, "Box1");
    Seen& seen = census_[entity];
    // Only state counts as a change: a part's place, in where, moves with the body and would use up the lines.
    auto note = [&](const std::string& key, const std::string& state, const std::string& where) {
        std::string& last = seen.state[key];
        if (last == state) return;
        last = state;
        if (seen.lines >= kCensusLines) {
            if (!seen.full) HSM_LOG("probe: census e%u: line limit reached for this level", entity);
            seen.full = true;
            return;
        }
        ++seen.lines;
        HSM_LOG("probe: census e%u %s %s%s", entity, key.c_str(), state.c_str(), where.c_str());
    };
    int total = 0, colliding = 0;
    const std::vector<UObject*> actors = CarriedBy(willie);
    for (UObject* actor : actors)
        for (UObject* comp : PartsOf(actor, primitive_)) {
            ++total;
            Call enabled(comp, "GetCollisionEnabled");
            if (!enabled.invoke()) continue;
            const int mode = enabled.get<uint8_t>("ReturnValue");
            if (mode == 0) continue;
            ++colliding;
            Call type(comp, "GetCollisionObjectType");
            Call visible(comp, "IsVisible");
            Call simulating(comp, "IsSimulatingPhysics");
            simulating.setName("BoneName", FName{});
            // On a skeletal part this is its own pelvis, on anything else the component itself.
            FVector at{};
            combat::BodyOffset b;
            char where[48] = " at ?";
            auto bucket = [](double cm) { return std::floor(cm / kCensusBucketCm) * kCensusBucketCm; };
            if (BoneAt(comp, "pelvis", at) && OnBody(bones, at, b))
                std::snprintf(where, sizeof where, " at fwd %.0f right %.0f up %.0f", bucket(b.forward), bucket(b.right), bucket(b.up));
            char state[224];
            std::snprintf(state, sizeof state, "%s coll %d type %d pawn %d phys %d weapon %d bodycoll %d sheathed %d sim %d visible %d",
                          ObjectName(reinterpret_cast<UObject*>(ClassOf(comp))).c_str(), mode, type.invoke() ? type.get<uint8_t>("ReturnValue") : -1,
                          Response(comp, kPawn), Response(comp, kPhysicsBody), Response(comp, kWeaponChannel),
                          Response(comp, kBodyCollision), Response(comp, kSheathedWeapon),
                          simulating.invoke() && simulating.get<bool>("ReturnValue") ? 1 : 0,
                          visible.invoke() && visible.get<bool>("ReturnValue") ? 1 : 0);
            // Box1 hangs from a joint, so where it has got to is the point of watching it.
            const std::string name = ObjectName(OuterOf(comp)) + "/" + ObjectName(comp);
            if (comp == box) note(name, state + std::string(where), "");
            else note(name, state, where);
        }
    char summary[64];
    std::snprintf(summary, sizeof summary, "%d colliding of %d parts on %d actors", colliding, total, int(actors.size()));
    note("total", summary, "");
}

}  // namespace hsm::game
