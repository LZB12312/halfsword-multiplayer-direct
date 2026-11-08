#include "gameplay/gear.h"
#include "gameplay/owner_sync.h"
#include "log.h"
#include "match/hsm_gear_names.h"
#include "match/hsm_match.h"
#include "util/hsm_names.h"
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace hsm::game {
using namespace hsm::ue;
namespace {

constexpr const char* kGeneratorClass = "BP_Generator_Characters_Random_C";
constexpr const char* kWillieClass = "Willie_BP_C";
constexpr const char* kSaveGameClass = "SG_GameProgress_C";
constexpr const char* kSavePassport = "Free Mode Player Character 1";   // SG_GameProgress_C field carrying the blob
// Outside Free Mode the generator picks weapons within the tier's Equipment Budget (hsm::kTierBudget, shared with the
// lobby page's hint), so a zero budget (as SpawnBot uses) would starve the loadout.

UObject* GameplayStatics() {
    UObject* cls = FindObject("GameplayStatics", "Class");
    return cls ? DefaultObject(reinterpret_cast<UClass*>(cls)) : nullptr;
}

// Same serialization as Adapter::ExportPassport: the generated passport copied into a fresh SG_GameProgress_C.
bool SerializePassport(Adapter& a, UObject* statics, UClass* sgClass, const void* passport, UFunction* generate,
                       std::vector<uint8_t>& out) {
    if (!passport) { HSM_LOG("gear: Generate Character has no Character Passport"); return false; }
    FProperty* src = nullptr;
    for (FProperty* p : Params(generate)) if (SameNameAscii(PropertyName(p), "Character Passport")) src = p;
    Call c(statics, "CreateSaveGameObject");
    c.set<UClass*>("SaveGameClass", sgClass);
    UObject* sg = c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
    FProperty* dst = sg ? FindProperty(ClassOf(sg), kSavePassport) : nullptr;
    if (!src || !dst || PropertySize(src) != PropertySize(dst) || PropertyType(src) != PropertyType(dst)) {
        HSM_LOG("gear: passport property missing or mismatched in the generated loadout");
        return false;
    }
    CopyPropertyValue(dst, reinterpret_cast<uint8_t*>(sg) + PropertyOffset(dst), passport);
    return a.SaveToMemory(sg, out);
}

// The game's weapon-type filter: byte fields named like the Free Mode setting, wherever they turn out to live. The owner
// class is not in the reflection index, so every plausible holder is searched: the game instance (where the Free Mode
// settings are), the generator itself and the game manager. Set for one Generate Character call, then put back.
constexpr const char* kFilterFields[] = {"Free Mode Opponents Weapons Type", "NPC Weapons Type"};
constexpr const char* kGameManagerClass = "BP_GameManager_C";

struct FilterField { uint8_t* at = nullptr; uint8_t was = 0; };

class WeaponFilterScope {
public:
    // Looks for the fields on each owner; sets those found to `value`. found() is how many.
    WeaponFilterScope(std::initializer_list<std::pair<const char*, UObject*>> owners, uint8_t value) {
        for (const auto& [where, owner] : owners) {
            if (!owner || IsGarbage(owner)) continue;
            for (const char* name : kFilterFields) {
                FProperty* p = FindProperty(ClassOf(owner), name);
                if (!p || PropertySize(p) != 1 || PropertyArrayDim(p) != 1) continue;
                FilterField f; f.at = reinterpret_cast<uint8_t*>(owner) + PropertyOffset(p); f.was = *f.at;
                fields_.push_back(f);
                *f.at = value;
                if (!where_.empty()) where_ += ", ";
                where_ += std::string(where) + "." + name + " (was " + std::to_string(f.was) + ")";
            }
        }
    }
    ~WeaponFilterScope() { for (const FilterField& f : fields_) *f.at = f.was; }
    WeaponFilterScope(const WeaponFilterScope&) = delete;
    WeaponFilterScope& operator=(const WeaponFilterScope&) = delete;
    size_t found() const { return fields_.size(); }
    const std::string& where() const { return where_; }
private:
    std::vector<FilterField> fields_;
    std::string where_;
};

}  // namespace

std::vector<uint8_t> RandomPassportBlob(Adapter& a, int tier, WeaponFilter weapons) {
    std::vector<uint8_t> blob;
    const int rank = std::clamp(tier, 0, 7);
    UObject* statics = GameplayStatics();
    UClass* gen = FindClass(kGeneratorClass);
    UClass* willie = FindClass(kWillieClass);
    UClass* sgClass = FindClass(kSaveGameClass);
    if (!a.World() || !statics || !gen || !willie || !sgClass) {
        HSM_LOG("gear: random tier %d needs a world, GameplayStatics, %s, %s and %s", rank, kGeneratorClass, kWillieClass, kSaveGameClass);
        return blob;
    }

    // Each loadout stalls the host's frame about 110 ms (playtest 2026-11-05: every 100+ ms spike while hosting Pit matched one). Where it goes
    // decides what can be done about it, so the log says.
    const double t0 = SteadyNowMs();
    double tSpawn = t0, tMake = t0, tSave = t0;
    FTransform t;
    Call begin(statics, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", a.World()).set<UClass*>("ActorClass", gen).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 0).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* g = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!g) { HSM_LOG("gear: generator spawn failed (tier %d)", rank); return blob; }
    if (double* budget = Field<double>(g, "Equipment Budget")) *budget = kTierBudget[rank];
    Call fin(statics, "FinishSpawningActor");
    fin.set<UObject*>("Actor", g).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    g = fin.invoke() ? fin.get<UObject*>("ReturnValue") : nullptr;
    if (!g) { HSM_LOG("gear: FinishSpawningActor failed for the generator (tier %d)", rank); return blob; }

    Call make(g, "Generate Character");
    make.set<UClass*>("Actor Class", willie).set<uint8_t>("Nationality", 0).set<uint8_t>("Tier", uint8_t(rank))
        .set<bool>("Is Mercenary", false).set<bool>("Ignore Budget", false);
    bool ok = false;
    {
        // The Weapons row: Any leaves the game's fields alone; the others set them for this one call (put back at the
        // scope's end, even on failure).
        const uint8_t gameByte = GameWeaponTypeByte(weapons);
        std::unique_ptr<WeaponFilterScope> scope;
        if (weapons != WeaponFilter::Any) {
            std::vector<UObject*> gms = FindInstances(kGameManagerClass);
            scope = std::make_unique<WeaponFilterScope>(std::initializer_list<std::pair<const char*, UObject*>>{
                {"GameInstance", a.GameInstance()}, {"Generator", g}, {kGameManagerClass, gms.empty() ? nullptr : gms[0]}}, gameByte);
            static bool logged = false;
            if (!logged) {
                logged = true;
                if (scope->found()) HSM_LOG("gear: weapon filter fields found: %s", scope->where().c_str());
                else HSM_LOG("gear: weapon filter: none of '%s' / '%s' found on the game instance, the generator or %s; the Weapons row has no effect",
                             kFilterFields[0], kFilterFields[1], kGameManagerClass);
            }
            HSM_LOG("gear: weapon filter %s -> game byte %u on %zu field(s)", WeaponFilterName(weapons), unsigned(gameByte), scope->found());
        }
        tSpawn = SteadyNowMs();
        ok = make.invoke();
        tMake = SteadyNowMs();
    }
    if (ok) ok = SerializePassport(a, statics, sgClass, make.paramPtr("Character Passport"), make.function(), blob);
    else HSM_LOG("gear: Generate Character failed (tier %d)", rank);
    tSave = SteadyNowMs();
    a.DestroyActor(g);
    if (!ok) { blob.clear(); HSM_LOG("gear: no loadout generated for tier %d", rank); }
    else {
        HSM_LOG("gear: generated random tier %d loadout (%zu bytes) in %.1f ms: spawn %.1f, Generate Character %.1f, save %.1f, destroy %.1f", rank,
                blob.size(), SteadyNowMs() - t0, tSpawn - t0, tMake - tSpawn, tSave - tMake, SteadyNowMs() - tSave);
        if (weapons != WeaponFilter::Any) {            // what the filter produced: the asset names the passport carries
            std::string names;
            for (const std::string& n : BlobAssetNames(blob)) names += (names.empty() ? "" : " | ") + n;
            HSM_LOG("gear: filter %s (game byte %u) rolled assets: %s", WeaponFilterName(weapons), unsigned(GameWeaponTypeByte(weapons)),
                    names.empty() ? "(none found in the blob)" : names.c_str());
        }
    }
    return blob;
}

}  // namespace hsm::game
