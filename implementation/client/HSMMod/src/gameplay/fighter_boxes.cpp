#include "gameplay/fighter_boxes.h"
#include "combat/hsm_fighter_box.h"
#include "log.h"

namespace hsm::game {

using namespace hsm::ue;

namespace {
constexpr uint64_t kLogFirst = 10;
struct FNameArray { const FName* data = nullptr; int32_t num = 0, max = 0; };   // TArray<FName>
bool g_on = true;

UObject* ObjField(UObject* o, const char* name) {
    UObject** p = Field<UObject*>(o, name);
    return p && *p && !IsGarbage(*p) ? *p : nullptr;
}
}  // namespace

void SetFighterBoxFix(bool on) { g_on = on; }

void FighterBoxes::Clear() { nextMs_ = 0; }

// Willie_BP Lc591: the fighter's Mesh and its "Rigid Bones" (every bone with mass), the other's Box1, never expiring.
void FighterBoxes::ClearPair(UObject* bodyOf, UObject* boxOf) {
    UObject* mesh = ObjField(bodyOf, "Mesh");
    UObject* box = ObjField(boxOf, "Box1");
    const FNameArray* bones = Field<FNameArray>(bodyOf, "Rigid Bones");
    if (!mesh || !box || !bones || bones->num <= 0) return;
    if (!Call(lib_, "DisableCollision_SkeletalVsSingleBody").set<UObject*>("SkeletalMesh", mesh).set<FNameArray>("SkeletalMeshBones", *bones)
             .set<UObject*>("SingleBody", box).setReal("TimeToExpiration", -1).invoke())
        return;
    if (++cleared_ <= kLogFirst) HSM_LOG("fighters: %s's body no longer catches on the box behind %s", ObjectName(bodyOf).c_str(), ObjectName(boxOf).c_str());
}

void FighterBoxes::Tick(double nowMs, const std::vector<UObject*>& standIns, const std::vector<std::pair<uint8_t, UObject*>>& owned) {
    if (!g_on || standIns.empty() || nowMs < nextMs_) return;
    nextMs_ = nowMs + combat::kFighterBoxEveryMs;
    if (!lib_)
        if (UClass* lib = FindClass("CollisionDisablerFunctionLibrary")) lib_ = DefaultObject(lib);
    if (!lib_) return;
    std::vector<UObject*> fighters = standIns;
    for (const auto& [entity, willie] : owned) fighters.push_back(willie);
    for (const combat::FighterBoxPair& p : combat::FighterBoxPairs(standIns.size(), owned.size()))
        if (fighters[p.body] && fighters[p.box] && !IsGarbage(fighters[p.body]) && !IsGarbage(fighters[p.box])) ClearPair(fighters[p.body], fighters[p.box]);
}

}  // namespace hsm::game
