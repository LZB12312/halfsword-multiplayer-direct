#include "gameplay/travel.h"
#include "log.h"
#include "session/hsm_travel_gate.h"
#include "ue/hook.h"
#include <cstring>

namespace hsm::game {
using namespace hsm::ue;
namespace {

// UGameplayStatics::OpenLevel(const UObject*, FName, bool, FString) for build 24185754; both Blueprint thunks
// (OpenLevel, OpenLevelBySoftObjectPtr) end here. The FString is passed by hidden pointer and destroyed by the callee.
constexpr uintptr_t kOpenLevelRva = 0x35E3590;
using OpenLevelFn = void (*)(UObject*, FName, bool, void*);
OpenLevelFn g_original = nullptr;
OpenLevelFilter g_filter;
bool g_bypass = false;

void Detour(UObject* ctx, FName level, bool absolute, void* options) {
    if (!g_bypass && g_filter) {
        std::string name = NameToString(level), opts = ReadFString(options);
        if (!g_filter(name, absolute, opts)) {
            HSM_LOG("travel: blocked OpenLevel(%s)", name.c_str());
            // Swallowed: the callee would have destroyed the by-value FString. It is owned by the caller's
            // temporary here, which leaks only its small buffer; acceptable for a blocked menu action.
            return;
        }
    }
    g_original(ctx, level, absolute, options);
}

// Fields copied host -> client before travel. Only plain values (bool/byte/enum/int/float/double) are sent.
constexpr const char* kSettings[] = {
    "Free Mode Activated", "Current Game Mode enum", "Current Play Mode", "Current Combat Mode", "Free Mode Foes Amount",
    "Free Mode Tier", "Free Mode Carnage", "Free Mode Brawling", "Free Mode Opponents Height", "Free Mode Opponents Weight",
    "Free Mode Opponents Weapons Type", "Free Mode Rendom Equipment Tier", "Free Mode Blossfechten", "Sparring Partner ID",
    "Progression Multiplayer", "FreeMode Multiplayer",
    // The level scripts read these too (audit item 39): Lose Condition 3 spawns BP_Fence_Bags in Pit, Yard and Cellar;
    // Day Time (Progression) and Current FreeMode Time (Free Mode) pick the lighting sublevel and ambience.
    "Current Lose Condition", "Day Time", "Current FreeMode Time",
    // rev 40: Progression co-op keeps one calendar, the host's (session/hsm_career.h): every client's career takes its day too.
    "Day Count",
};
bool PlainType(const std::string& t) {
    return t == "BoolProperty" || t == "ByteProperty" || t == "EnumProperty" || t == "IntProperty" || t == "FloatProperty" || t == "DoubleProperty";
}
UObject* GameInstance() { auto v = FindInstances("GI_Settings_C"); return v.empty() ? nullptr : v[0]; }
}  // namespace

bool InstallTravelHook(OpenLevelFilter filter) {
    g_filter = std::move(filter);
    return HookNative(reinterpret_cast<void*>(ModuleBase() + kOpenLevelRva), reinterpret_cast<void*>(&Detour),
                      reinterpret_cast<void**>(&g_original), "UGameplayStatics::OpenLevel");
}

void OpenLevel(UObject* ctx, const std::string& level, bool absolute, const std::string& options) {
    static UObject* statics = nullptr;
    if (!statics) if (UObject* c = FindObject("GameplayStatics", "Class")) statics = DefaultObject(reinterpret_cast<UClass*>(c));
    FStringArgHolder opts(options);
    Call c(statics, "OpenLevel");
    c.set<UObject*>("WorldContextObject", ctx).setName("LevelName", MakeName(level)).set<bool>("bAbsolute", absolute).set("Options", opts.arg);
    g_bypass = true;
    c.invoke();
    g_bypass = false;
}

std::vector<Setting> CaptureGameSettings() {
    std::vector<Setting> out;
    UObject* gi = GameInstance();
    if (!gi) return out;
    for (const char* name : kSettings) {
        FProperty* p = FindProperty(ClassOf(gi), name);
        if (!p || !PlainType(PropertyType(p))) continue;
        Setting s; s.name = name;
        if (PropertyType(p) == "BoolProperty") { bool b = false; GetBool(gi, name, b); s.value = {uint8_t(b)}; }
        else {
            int32_t size = PropertySize(p);
            if (size <= 0 || size_t(size) > kMaxSettingBytes) continue;
            const uint8_t* src = reinterpret_cast<const uint8_t*>(gi) + PropertyOffset(p);
            s.value.assign(src, src + size);
        }
        out.push_back(std::move(s));
    }
    return out;
}

size_t ApplyGameSettings(const std::vector<Setting>& settings) {
    UObject* gi = GameInstance();
    if (!gi) return 0;
    size_t applied = 0;
    for (const Setting& s : settings) {
        bool allowed = false;
        for (const char* n : kSettings) if (s.name == n) allowed = true;
        if (!allowed) continue;                                // only the whitelist, never arbitrary fields
        FProperty* p = FindProperty(ClassOf(gi), s.name);
        if (!p || !PlainType(PropertyType(p))) continue;
        const std::string type = PropertyType(p);
        if (type == "BoolProperty") { if (SettingValueOk(SettingKind::Bool, s.value.data(), s.value.size()) && SetBool(gi, s.name, s.value[0] != 0)) ++applied; continue; }
        if (int32_t(s.value.size()) != PropertySize(p)) continue;
        const SettingKind kind = type == "FloatProperty" ? SettingKind::Float : type == "DoubleProperty" ? SettingKind::Double
                               : type == "IntProperty" || s.value.size() != 1 ? SettingKind::Int : SettingKind::Byte;   // L8: no NaN/Inf, no wild enum bytes
        if (!SettingValueOk(kind, s.value.data(), s.value.size())) continue;
        std::memcpy(reinterpret_cast<uint8_t*>(gi) + PropertyOffset(p), s.value.data(), s.value.size());
        ++applied;
    }
    return applied;
}

std::string ChangedGameSettings(const std::vector<Setting>& want) {
    const std::vector<Setting> now = CaptureGameSettings();
    std::string out;
    for (const Setting& w : want)
        for (const Setting& s : now)
            if (s.name == w.name && s.value != w.value) out += (out.empty() ? "" : ", ") + s.name;
    return out;
}

}  // namespace hsm::game
