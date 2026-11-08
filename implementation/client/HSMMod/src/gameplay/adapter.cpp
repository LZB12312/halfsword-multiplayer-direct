#include "gameplay/adapter.h"
#include <windows.h>
#include "gameplay/combat_sync.h"
#include "gameplay/damage_pin.h"
#include "hsm_chat.h"
#include "hsm_owner.h"
#include "log.h"
#include "match/hsm_roles.h"
#include "match/hsm_match.h"
#include "match/hsm_modes.h"
#include "match/hsm_team_kit.h"
#include "session/hsm_ground_clear.h"
#include "session/hsm_gvas.h"
#include "sync/hsm_gore.h"
#include "ue/hook.h"
#include "ue/script_hook.h"
#include "util/hsm_names.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <map>
#include <set>

namespace hsm::game {
using namespace hsm::ue;
namespace {

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };
constexpr const char* kWillieClass = "Willie_BP_C";
constexpr const char* kZombieClass = "Willie_BP_Zombie_C";   // the Zombies format's bots: a child of Willie_BP_C with the rotten look, no new fields
constexpr const char* kGameInstanceClass = "GI_Settings_C";
constexpr const char* kSaveGameClass = "SG_GameProgress_C";
constexpr const char* kSpawnerClass = "BP_SpawnerPoint_Willies_C";
constexpr const char* kBridgeBoardClass = "BP_Structure_Board_Destructible_1X6M_C";
constexpr const char* kPassportStruct = "Str_Passport_Character1";
constexpr const char* kEquipmentStruct = "Str_SubPassport_Equipment";            // the passport's Equipment member (docs/REVERSE_ENGINEERING.md section 8)
constexpr const char* kPassportFreeMode = "Free Mode Player Character 1";      // GI_Settings, Free Mode
constexpr const char* kPassportProgression = "Player Character";              // GI_Settings, Progression career fighter
constexpr const char* kPassportDefault = "Free Mode Player Character 2";      // GI_Settings, stock player-2 slot
constexpr const char* kSavePassport = "Free Mode Player Character 1";         // SG_GameProgress_C field carrying the blob

// Input event names as generated for Willie_BP from DefaultInput.ini mappings (order = protocol enums).
constexpr const char* kAxisNames[kAxisCount] = {
    "Move Forward / Backward", "Move Right / Left", "Turn Right / Left Mouse", "Look Up / Down Mouse",
    "Turn Right / Left Gamepad", "Look Up / Down Gamepad", "Right Arm Axis", "Left Arm Axis",
    "Right Guard Axis", "Left Guard Axis",
};
constexpr const char* kActionNames[kActionCount] = {
    "Grab Left", "Grab Right", "Guard Left", "Guard Right", "Thrust", "Thrust Gamepad", "Jump", "Crouch Key",
    "Crouch Hold", "Run", "Swap Hands", "Toggle Camera Lock", "Key Forward", "Key Back", "Key Left", "Key Right",
    "Talk", "RightMouseButton", "MiddleMouseButton",
};
// Simulated body bones (UE mannequin layout used by the Willie skeleton). Missing bones are skipped.
constexpr const char* kBoneCandidates[] = {
    "pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05", "neck_01", "neck_02", "head",
    "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
    "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r",
};
constexpr const char* kPartHealth[7] = { "Head Health", "Body Upper Health", "Body Lower Health", "Arm_R Health", "Arm_L Health", "Leg_R Health", "Leg_L Health" };
// VitalsMsg.state bit i is field i (protocol/hsm_owner.h kVitalsState*).
constexpr const char* kStateBoolFields[] = { "Downed", "Any_Guarding", "R_Guarding", "L_Guarding", "Is Crouched",
                                             "R_Thrusting", "Threatening Stance", "Give Up" };
static_assert(std::size(kStateBoolFields) == 8, "one name per state bit");

// Velocity tracking gains: reach the target over ~tau seconds, with caps so a bad sample cannot fling bodies.
constexpr double kTau = 0.05, kMaxLinear = 3000.0, kMaxAngularDeg = 1500.0;   // 50 ms: puppets keep up with fast swings

int TrailingNumber(const std::string& s) {
    size_t i = s.size(); while (i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1]))) --i;
    return i < s.size() ? std::atoi(s.c_str() + i) : -1;
}
double Clamp(double v, double lim) { return v > lim ? lim : v < -lim ? -lim : v; }

FQuat Mul(const FQuat& a, const FQuat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
FQuat Conj(const FQuat& q) { return { -q.x, -q.y, -q.z, q.w }; }
}  // namespace

bool Adapter::Resolve(UObject* worldContext) {
    world_ = worldContext;
    if (!statics_) if (UObject* c = FindObject("GameplayStatics", "Class")) statics_ = DefaultObject(reinterpret_cast<UClass*>(c));
    return statics_ != nullptr && world_ != nullptr;
}

UObject* Adapter::PlayerWillie(int localIndex) {
    Call c(statics_, "GetPlayerCharacter");
    c.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", localIndex);
    if (!c.invoke()) return nullptr;
    UObject* ch = c.get<UObject*>("ReturnValue");
    if (!ch) return nullptr;
    if (!willieClass_) {
        UClass* cls = ClassOf(ch);
        if (SameNameAscii(ObjectName(reinterpret_cast<UObject*>(cls)), kWillieClass)) willieClass_ = cls;
    }
    return willieClass_ && IsA(ch, willieClass_) ? ch : nullptr;
}

namespace {
UObject* WidgetLibrary() {
    static UObject* wbl = nullptr;
    if (!wbl) if (UObject* cls = FindObject("WidgetBlueprintLibrary", "Class")) wbl = DefaultObject(reinterpret_cast<UClass*>(cls));
    return wbl;
}
}  // namespace

// Same calls the game makes when a menu closes (decomp UI/UI_DeathDoor.cpp:118-119): our session travel skips the
// stock menu flow that would otherwise leave the menu's UI input mode and cursor behind.
void Adapter::RestoreGameInput() {
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", 0);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) return;
    inputTouched_ = true;
    Call mode(WidgetLibrary(), "SetInputMode_GameOnly");
    mode.set<UObject*>("PlayerController", ctrl).set<bool>("bFlushInput", false).invoke();
    SetBool(ctrl, "bShowMouseCursor", false);
}

namespace {
bool InViewportNow(UObject* w) {
    Call v(w, "IsInViewport");
    return v.invoke() && v.get<bool>("ReturnValue");
}
}  // namespace

// The stock pause menu is UI_Pause_Eng (decomp Willie_BP.cpp:11091). Matched by the substring "UI_Pause" so a different
// Blueprint suffix still works; the class name found is logged once. A closed menu can linger until garbage collection,
// so only a widget IsInViewport counts. Esc or a paused game arms a 2 s window of searches, kept up while the menu is open.
bool Adapter::PauseMenuOpen() {
    const uint32_t now = GetTickCount();
    if (pauseCheckAt_ && now - pauseCheckAt_ < 100) return pauseOpen_;
    pauseCheckAt_ = now;
    if (pauseOpen_ || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) || GamePaused()) pauseArmedUntil_ = now + 2000;
    if (pauseOpen_ && pauseWidget_ && ObjectAt(pauseWidgetIndex_) == pauseWidget_ && InViewportNow(pauseWidget_)) return true;   // still the one
    if (int32_t(pauseArmedUntil_ - now) <= 0 || (pauseScanAt_ && now - pauseScanAt_ < 250)) return pauseOpen_;
    pauseScanAt_ = now;
    bool found = false;
    pauseWidget_ = nullptr;
    UClass* uw = FindClass("UserWidget");
    for (int32_t i = 0, n = ObjectCount(); i < n && uw && !found; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || !IsA(o, uw)) continue;
        const std::string cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(o)));
        if (cls.find("UI_Pause") == std::string::npos) continue;
        Call v(o, "IsInViewport");
        if (!v.invoke() || !v.get<bool>("ReturnValue")) continue;
        found = true;
        pauseWidget_ = o; pauseWidgetIndex_ = i;
        if (!pauseLogged_) { pauseLogged_ = true; HSM_LOG("adapter: pause menu widget class '%s'", cls.c_str()); }
    }
    pauseOpen_ = found;
    return pauseOpen_;
}

UObject* Adapter::PauseMenu() { return PauseMenuOpen() && pauseWidget_ && ObjectAt(pauseWidgetIndex_) == pauseWidget_ ? pauseWidget_ : nullptr; }

// The click handler of the stock Continue button is a Blueprint function bound to its OnClicked, named like
// "BndEvt__UI_Pause_Eng_<button>_K2Node_ComponentBoundEvent_N_OnButtonClickedEvent__DelegateSignature". The one whose
// name has Continue (or Resume) and Clicked is run; the names found are logged once so a different build shows what it has.
bool Adapter::ContinueFromPauseMenu() {
    UObject* menu = PauseMenu();
    if (!menu) return true;
    static bool logged = false;
    UFunction* handler = nullptr;
    auto has = [](const std::string& name, const char* word) {
        std::string a = name, b = word;
        for (char& c : a) c = char(tolower(static_cast<unsigned char>(c)));
        for (char& c : b) c = char(tolower(static_cast<unsigned char>(c)));
        return a.find(b) != std::string::npos;
    };
    for (UFunction* f : FunctionsWithPrefix(ClassOf(menu), "BndEvt__")) {
        const std::string name = ObjectName(reinterpret_cast<UObject*>(f));
        if (!logged) HSM_LOG("adapter: pause menu handler %s", name.c_str());
        if (!handler && (has(name, "Continue") || has(name, "Resume")) && has(name, "Click")) handler = f;
    }
    if (!logged) { logged = true; HSM_LOG("adapter: pause menu Continue handler %s", handler ? ObjectName(reinterpret_cast<UObject*>(handler)).c_str() : "not found"); }
    if (handler) Call(menu, handler).invoke();
    if (!handler || InViewportNow(menu)) {
        Call(menu, "RemoveFromParent").invoke();
        if (GamePaused()) SetGamePaused(false);
        RestoreGameInput();
        HSM_LOG("adapter: pause menu removed by the mod (%s)", handler ? "Continue left it up" : "no Continue handler");
    }
    pauseOpen_ = false; pauseWidget_ = nullptr; pauseCheckAt_ = 0; pauseScanAt_ = 0;
    return true;
}

void Adapter::ShowPauseCursor() {
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", 0);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) return;
    Call mode(WidgetLibrary(), "SetInputMode_GameAndUIEx");
    mode.set<UObject*>("PlayerController", ctrl).set<UObject*>("InWidgetToFocus", nullptr).set<uint8_t>("InMouseLockMode", 0)
        .set<bool>("bHideCursorDuringCapture", false).set<bool>("bFlushInput", false).invoke();
    SetBool(ctrl, "bShowMouseCursor", true);
}

bool Adapter::MenuInputBack() {
    if (!inputTouched_) return false;
    inputTouched_ = false;
    inputCaptured_ = false;
    ShowPauseCursor();
    return true;
}

// The call the game makes when its death screen opens (decomp UI/UI_DeathDoor.cpp:226): UI-only, no widget focused, the
// mouse not locked (EMouseLockMode::DoNotLock), the keys held now flushed (a running fighter stops). The viewport then
// ignores keys, so G (inventory), R (slow motion) and Esc (pause) reach neither the fighter nor the pause menu; the
// chat reads the OS key state and is unaffected.
void Adapter::CaptureInput(bool on) {
    inputCaptured_ = on;
    if (!on) { if (PauseMenuOpen()) ShowPauseCursor(); else RestoreGameInput(); return; }   // closing the chat must not take the pause menu's cursor
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", 0);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) return;
    inputTouched_ = true;
    Call mode(WidgetLibrary(), "SetInputMode_UIOnlyEx");
    mode.set<UObject*>("PlayerController", ctrl).set<UObject*>("InWidgetToFocus", nullptr).set<uint8_t>("InMouseLockMode", 0)
        .set<bool>("bFlushInput", true).invoke();
}

// One LineTraceSingle on the visibility channel from `start` to `end`, looking through `ignore`: true on a hit, with
// its impact point and the hit actor's name when asked for.
bool Adapter::TraceLine(const FVector& start, const FVector& end, const std::vector<UObject*>& ignore, FVector* impact, std::string* hitName) {
    static UObject* ksl = nullptr;
    if (!ksl) if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    struct ActorArray { UObject** data; int32_t num, max; };   // TArray<AActor*> view over our vector; the engine only reads it
    std::vector<UObject*> ignored = ignore;
    ActorArray skip{ignored.data(), int32_t(ignored.size()), int32_t(ignored.size())};
    Call t(ksl, "LineTraceSingle");
    t.set<UObject*>("WorldContextObject", world_).set("Start", start).set("End", end)
     .set<uint8_t>("TraceChannel", 0).set<bool>("bTraceComplex", false).set("ActorsToIgnore", skip).set<uint8_t>("DrawDebugType", 0)
     .set<bool>("bIgnoreSelf", true).set<float>("DrawTime", 0.f);
    if (!t.invoke() || !t.get<bool>("ReturnValue")) return false;
    const uint8_t* hit = static_cast<const uint8_t*>(t.paramPtr("OutHit"));
    if (!hit) return false;
    if (impact) std::memcpy(impact, hit + 40, sizeof *impact);   // FHitResult::ImpactPoint
    if (hitName) {
        Call b(statics_, "BreakHitResult");
        b.setFrom("Hit", hit);
        UObject* actor = b.invoke() ? b.get<UObject*>("HitActor") : nullptr;
        *hitName = actor ? ObjectName(actor) : std::string("nothing");
    }
    return true;
}

bool Adapter::FloorZ(const FVector& at, const std::vector<UObject*>& ignore, double& z, std::string* hitName) {
    FVector ip;
    if (!TraceLine(FVector{at.x, at.y, at.z + 150}, FVector{at.x, at.y, at.z - 600}, ignore, &ip, hitName)) return false;
    if (!std::isfinite(ip.z)) return false;
    z = ip.z;
    return true;
}

// Where the player's camera is now: the game's own camera, or our follow camera while spectating.
bool Adapter::CameraLocation(FVector& out) {
    Call pc(statics_, "GetPlayerCameraManager");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", 0);
    UObject* cm = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!cm) return false;
    Call loc(cm, "GetCameraLocation");
    if (!loc.invoke()) return false;
    out = loc.get<FVector>("ReturnValue");
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

bool Adapter::LineClear(const FVector& from, const FVector& to, const std::vector<UObject*>& ignore, std::string* hitName) {
    return !TraceLine(from, to, ignore, nullptr, hitName);
}

bool Adapter::BoneLocation(UObject* willie, const char* bone, FVector& out) {
    UObject* mesh = willie ? MeshOf(willie) : nullptr;
    if (!mesh || !bone) return false;
    Call c(mesh, "GetSocketLocation");
    c.setName("InSocketName", MakeName(bone));
    if (!c.invoke()) return false;
    out = c.get<FVector>("ReturnValue");
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

static UObject* NavigationSystem() {
    static UObject* nav = nullptr;
    if (!nav) if (UObject* cls = FindObject("NavigationSystemV1", "Class")) nav = DefaultObject(reinterpret_cast<UClass*>(cls));
    return nav;
}

// A navigable point near `aim` that the navmesh connects to `from` (the host). The Abyss' level script places its
// enemies with K2_GetRandomLocationInNavigableRadius (decomp Maps/Abyss_Map_Open_EA.cpp:997), which also answers on
// navmesh islands the fighters cannot walk to: test 2026-10-25, the Yard's ring spawns landed outside its fence. So
// the points are drawn reachable from `from` within its distance to `aim` plus the tolerance, and the one nearest
// `aim` is kept if it is within `tolerance` of it.
bool Adapter::ReachablePointNear(const FVector& from, const FVector& aim, double tolerance, FVector& out) {
    UObject* nav = NavigationSystem();
    if (!nav || !world_) return false;
    const double reach = std::hypot(aim.x - from.x, aim.y - from.y) + tolerance;
    double best = tolerance * tolerance;
    bool found = false;
    for (int i = 0; i < 24; ++i) {
        Call c(nav, "K2_GetRandomReachablePointInRadius");
        c.set<UObject*>("WorldContextObject", world_).set("Origin", from).setReal("Radius", reach)
         .set<UObject*>("NavData", nullptr).set<UObject*>("FilterClass", nullptr);
        if (!c.invoke() || !c.get<bool>("ReturnValue")) return found;
        const FVector v = c.get<FVector>("RandomLocation");
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) continue;
        const double d2 = (v.x - aim.x) * (v.x - aim.x) + (v.y - aim.y) * (v.y - aim.y);
        if (d2 <= best) { best = d2; out = v; found = true; }
    }
    return found;
}

// What a floor trace for a spawn must look through (test 5: it hit the fighter already standing at the spot): every
// fighter with its attached actors (armor pieces, held and sheathed weapons) and every weapon lying around.
std::vector<UObject*> Adapter::BodiesAndGear() {
    std::vector<UObject*> out = WeaponActors();
    for (UObject* w : AllWillies()) {
        out.push_back(w);
        Call att(w, "GetAttachedActors");
        att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", true);
        FObjArray* arr = att.invoke() ? static_cast<FObjArray*>(att.paramPtr("OutActors")) : nullptr;
        if (arr && arr->num > 0 && arr->num <= 256)
            for (int32_t i = 0; i < arr->num; ++i) if (arr->data[i]) out.push_back(arr->data[i]);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool Adapter::ControlRotation(int localIndex, float& yaw, float& pitch) {
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", localIndex);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) return false;
    Call r(ctrl, "GetControlRotation");
    if (!r.invoke()) return false;
    FRotator rot = r.get<FRotator>("ReturnValue");
    yaw = float(std::remainder(rot.yaw, 360.0)); pitch = float(std::remainder(rot.pitch, 360.0));
    return std::isfinite(yaw) && std::isfinite(pitch);
}

bool Adapter::SetControlRotation(int localIndex, float yaw, float pitch) {
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", localIndex);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    return ctrl && Call(ctrl, "SetControlRotation").set("NewRotation", FRotator{pitch, yaw, 0}).invoke();
}

UObject* Adapter::GameInstance() {
    // A full scan of the object array (hundreds of thousands) each call, and the host's DriveFighter asks for the mouse
    // look settings once per driven fighter per frame: the instance is kept and checked at its slot (drive_lean).
    if (lean_ && gi_ && ObjectAt(giIndex_) == gi_ && ClassOf(gi_) == giClass_ && !IsGarbage(gi_)) return gi_;
    std::vector<UObject*> gis = FindInstances(kGameInstanceClass);
    UObject* gi = gis.empty() ? nullptr : gis[0];
    gi_ = gi; giIndex_ = gi ? ObjectIndex(gi) : -1; giClass_ = gi ? ClassOf(gi) : nullptr;
    return gi;
}

// HSMMP_tmp_<pid>_<n>.sav: a slot of another running instance (a local host + client test) is still in use.
static bool OwnerAlive(const wchar_t* file) {
    unsigned long pid = 0;
    if (std::swscanf(file, L"HSMMP_tmp_%lu_", &pid) != 1 || !pid) return false;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return alive;
}

// SaveGameToMemory/LoadGameFromMemory are C++-only in this engine build (not reflected, and inlined into the slot
// functions), so the blob goes through a temporary save slot: SaveGameToSlot writes exactly the bytes
// SaveGameToMemory would produce (GVAS header + object), which are read back from Saved/SaveGames and deleted.
std::wstring Adapter::SaveGamesDir() {
    if (!saveDir_.empty()) return saveDir_;
    UObject* ksl = nullptr;
    if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    Call dir(ksl, "GetProjectSavedDirectory");
    if (!dir.invoke()) return {};
    FStringArg rel = dir.get<FStringArg>("ReturnValue");
    Call abs(ksl, "ConvertToAbsolutePath");
    abs.set("Filename", rel);
    if (!abs.invoke()) return {};
    FStringArg full = abs.get<FStringArg>("ReturnValue");
    std::string path = ReadFString(&full);
    if (path.empty()) return {};
    { const std::u16string u = Utf16FromUtf8(path); saveDir_.assign(u.begin(), u.end()); }   // UTF-8 back to UTF-16 (a non-ASCII profile path)
    if (saveDir_.back() != L'/' && saveDir_.back() != L'\\') saveDir_ += L'/';
    saveDir_ += L"SaveGames/";
    // Sweep temporary slots a crashed session may have left in the player's save folder.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((saveDir_ + L"HSMMP_tmp_*.sav").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        int removed = 0;
        do { if (!OwnerAlive(fd.cFileName) && DeleteFileW((saveDir_ + fd.cFileName).c_str())) ++removed; } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (removed) HSM_LOG("adapter: removed %d leftover temporary save slot(s)", removed);
    }
    return saveDir_;
}

std::string Adapter::TempSlot() {
    char b[64];
    std::snprintf(b, sizeof b, "HSMMP_tmp_%lu_%u", GetCurrentProcessId(), ++slotCounter_);
    return b;
}

void Adapter::DeleteSlot(const std::string& slot) {
    FStringArgHolder name(slot);
    Call(statics_, "DeleteGameInSlot").set("SlotName", name.arg).set<int32_t>("UserIndex", 0).invoke();
}

bool Adapter::SaveToMemory(UObject* saveGame, std::vector<uint8_t>& out) {
    std::wstring dir = SaveGamesDir();
    if (dir.empty()) { HSM_LOG("adapter: Saved directory unknown"); return false; }
    // Two tries, each on a fresh slot: the save writer can fail once (0.7.1 logs: twice in one session, and the peers
    // then saw the default loadout); the step that failed is logged.
    for (int attempt = 1; attempt <= 2; ++attempt) {
        out.clear();
        std::string slot = TempSlot();
        FStringArgHolder name(slot);
        Call s(statics_, "SaveGameToSlot");
        s.set<UObject*>("SaveGameObject", saveGame).set("SlotName", name.arg).set<int32_t>("UserIndex", 0);
        const char* failed = !s.invoke() ? "call" : !s.get<bool>("ReturnValue") ? "the game's save" : nullptr;
        std::wstring file = dir + std::wstring(slot.begin(), slot.end()) + L".sav";
        if (!failed) {
            if (FILE* f = _wfopen(file.c_str(), L"rb")) {
                std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
                if (n > 0 && n <= 64L * 1024 * 1024) {
                    out.resize(size_t(n));
                    if (std::fread(out.data(), 1, out.size(), f) != out.size()) failed = "reading the file";
                } else failed = "the file's size";
                std::fclose(f);
            } else failed = "opening the file";
        }
        DeleteSlot(slot);
        if (!failed && !out.empty()) return true;
        HSM_LOG("adapter: SaveGameToSlot(%s) failed (%s)%s", slot.c_str(), failed ? failed : "empty", attempt == 1 ? "; trying again" : "");
    }
    out.clear();
    return false;
}

bool Adapter::ExportPassport(std::vector<uint8_t>& out) {
    UObject* gi = GameInstance();
    if (!gi) { HSM_LOG("adapter: export needs %s", kGameInstanceClass); return false; }
    bool freeMode = true;
    GetBool(gi, "Free Mode Activated", freeMode);
    const char* source = freeMode ? kPassportFreeMode : kPassportProgression;
    bool ok = SavePassportFrom(gi, FindProperty(ClassOf(gi), source), out);
    HSM_LOG("adapter: exported %s passport (%zu bytes)%s", freeMode ? "Free Mode" : "Progression", out.size(), ok ? "" : " FAILED");
    std::string why;
    if (ok && !hsm::CheckPassportSave(out.data(), out.size(), &why))        // peers would refuse it: a game update moved the save layout
        HSM_LOG("adapter: own passport export FAILS the loadout check (%s); other players will see the default loadout", why.c_str());
    return ok;
}

bool Adapter::ExportPassportOf(UObject* willie, std::vector<uint8_t>& out) {
    return willie && SavePassportFrom(willie, FindProperty(ClassOf(willie), "Character Passport"), out);
}

UObject* Adapter::NewSaveObject() {
    UClass* sgClass = FindClass(kSaveGameClass);
    if (!sgClass || !statics_) return nullptr;
    Call c(statics_, "CreateSaveGameObject");
    c.set<UClass*>("SaveGameClass", sgClass);
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

// ------------------------------------------------------------------ a Progression fight's end on the career (session/hsm_career.h)

bool Adapter::GetCareerInt(const char* field, int64_t& out) {
    UObject* gi = GameInstance();
    FProperty* p = gi ? FindProperty(ClassOf(gi), field) : nullptr;
    if (!p) return false;
    const uint8_t* at = reinterpret_cast<const uint8_t*>(gi) + PropertyOffset(p);
    if (PropertySize(p) == 8) { std::memcpy(&out, at, 8); return true; }
    if (PropertySize(p) == 4) { int32_t v = 0; std::memcpy(&v, at, 4); out = v; return true; }
    return false;
}

bool Adapter::SetCareerInt(const char* field, int64_t value) {
    UObject* gi = GameInstance();
    FProperty* p = gi ? FindProperty(ClassOf(gi), field) : nullptr;
    if (!p) return false;
    uint8_t* at = reinterpret_cast<uint8_t*>(gi) + PropertyOffset(p);
    if (PropertySize(p) == 8) { std::memcpy(at, &value, 8); return true; }
    if (PropertySize(p) == 4) { const int32_t v = int32_t(std::clamp<int64_t>(value, INT32_MIN, INT32_MAX)); std::memcpy(at, &v, 4); return true; }
    return false;
}

int Adapter::CareerGameMode() { UObject* gi = GameInstance(); const uint8_t* m = gi ? Field<uint8_t>(gi, "Current Game Mode enum") : nullptr; return m ? *m : -1; }
int Adapter::CareerCombatMode() { UObject* gi = GameInstance(); const uint8_t* m = gi ? Field<uint8_t>(gi, "Current Combat Mode") : nullptr; return m ? *m : -1; }

bool Adapter::ReadCareerRank(hsm::CareerRank& out) {
    UObject* gi = GameInstance();
    const uint8_t* tier = gi ? Field<uint8_t>(gi, "Player Tier") : nullptr;
    const uint8_t* perm = gi ? Field<uint8_t>(gi, "Permanent Tier") : nullptr;
    if (!tier || !perm || !GetCareerInt("Player Funds", out.funds) || !GetCareerInt("Reputation on Rank", out.reputation)) return false;
    out.tier = *tier; out.permanentTier = *perm;
    return true;
}

bool Adapter::WriteCareerRank(const hsm::CareerChange& change) {
    UObject* gi = GameInstance();
    uint8_t* tier = gi ? Field<uint8_t>(gi, "Player Tier") : nullptr;
    uint8_t* perm = gi ? Field<uint8_t>(gi, "Permanent Tier") : nullptr;
    if (!tier || !perm) return false;
    bool ok = SetCareerInt("Player Funds", change.after.funds) && SetCareerInt("Reputation on Rank", change.after.reputation);
    *tier = uint8_t(change.after.tier); *perm = uint8_t(change.after.permanentTier);
    if (change.tourneyWon) {
        // The stock Tourney win (decomp UI_NextFIght.cpp:146-153): the nemeses are settled, and the Tavern renews its people,
        // its fight board (the next rank's) and the merchant's stock. Their own Save Game calls stay blocked by the save guard.
        if (FProperty* nem = FindProperty(ClassOf(gi), "Nemesis Opponents")) ClearPropertyValue(nem, reinterpret_cast<uint8_t*>(gi) + PropertyOffset(nem));
        else ok = false;
        auto run = [&ok](Call& c, const char* what) { if (!c.valid() || !c.invoke()) { HSM_LOG("adapter: the Tourney's %s could not be called", what); ok = false; } };
        Call npcs(gi, "Refresh NPCs Event"); run(npcs, "Refresh NPCs Event");
        Call board(gi, "Event Generate Random Combat Events"); run(board, "Event Generate Random Combat Events");
        Call merchant(gi, "Renew Merchant"); run(merchant, "Renew Merchant");
    }
    return ok;
}

bool Adapter::SkipTimeStep() {
    UObject* gi = GameInstance();
    if (!gi) return false;
    Call c(gi, "Skip Time Event");   // GI_Settings.cpp:2013: at no price it always moves; after Night comes a new day
    c.setReal("Price", 0.0);
    return c.valid() && c.invoke();
}

// Copies one passport-typed property of `owner` into a fresh SG_GameProgress_C and serializes it.
bool Adapter::SavePassportFrom(UObject* owner, FProperty* src, std::vector<uint8_t>& out) {
    if (!src) return false;
    UObject* sg = NewSaveObject();
    FProperty* dst = sg ? FindProperty(ClassOf(sg), kSavePassport) : nullptr;
    if (!dst || PropertySize(src) != PropertySize(dst) || PropertyType(src) != PropertyType(dst)) {
        HSM_LOG("adapter: passport properties missing or mismatched (%s)", PropertyName(src).c_str()); return false;
    }
    CopyPropertyValue(dst, reinterpret_cast<uint8_t*>(sg) + PropertyOffset(dst), reinterpret_cast<uint8_t*>(owner) + PropertyOffset(src));
    return SaveToMemory(sg, out);
}

UObject* Adapter::LoadPassportSave(const std::vector<uint8_t>& blob) {
    std::string why;
    if (!hsm::CheckPassportSave(blob.data(), blob.size(), &why)) {     // before the engine creates and fills anything from it
        HSM_LOG("adapter: refused loadout blob (%zu bytes): %s", blob.size(), why.c_str());
        return nullptr;
    }
    UClass* sgClass = FindClass(kSaveGameClass);
    std::wstring dir = SaveGamesDir();
    if (dir.empty()) return nullptr;
    std::string slot = TempSlot();
    std::wstring file = dir + std::wstring(slot.begin(), slot.end()) + L".sav";
    FILE* f = _wfopen(file.c_str(), L"wb");
    if (!f) { HSM_LOG("adapter: cannot write temporary save slot"); return nullptr; }
    bool wrote = std::fwrite(blob.data(), 1, blob.size(), f) == blob.size();
    std::fclose(f);
    UObject* sg = nullptr;
    if (wrote) {
        FStringArgHolder name(slot);
        Call c(statics_, "LoadGameFromSlot");
        c.set("SlotName", name.arg).set<int32_t>("UserIndex", 0);
        sg = c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
    }
    DeleteSlot(slot);
    _wremove(file.c_str());                              // in case DeleteGameInSlot did not (bad data)
    if (!sg || !sgClass || !IsA(sg, sgClass)) { HSM_LOG("adapter: loadout blob is not a %s", kSaveGameClass); return nullptr; }
    return sg;
}

// Copies the passport into a deferred-spawned Willie: from the blob's SaveGame, or the stock player-2 slot.
bool Adapter::CopyPassport(UObject* willie, const SpawnOptions& o, double& height) {
    FProperty* dst = FindProperty(ClassOf(willie), "Character Passport");
    const uint8_t* from = static_cast<const uint8_t*>(o.passport);
    if (!from) {
        UObject* owner = nullptr; FProperty* src = nullptr;
        if (o.blob) { owner = LoadPassportSave(*o.blob); src = owner ? FindProperty(ClassOf(owner), kSavePassport) : nullptr; }
        if (!src) { owner = GameInstance(); src = owner ? FindProperty(ClassOf(owner), kPassportDefault) : nullptr; }
        if (!src || (dst && PropertySize(src) != PropertySize(dst))) { HSM_LOG("adapter: cannot copy passport"); return false; }
        from = reinterpret_cast<uint8_t*>(owner) + PropertyOffset(src);
    }
    if (!dst) { HSM_LOG("adapter: Willie has no Character Passport"); return false; }
    CopyPropertyValue(dst, reinterpret_cast<uint8_t*>(willie) + PropertyOffset(dst), from);
    height = 0.5;
    if (UObject* st = FindObject(kPassportStruct, "UserDefinedStruct"))
        if (FProperty* h = FindPropertyPrefix(st, "Height_"); h && PropertySize(h) == sizeof(double)) std::memcpy(&height, from + PropertyOffset(h), sizeof height);
    if (!std::isfinite(height)) height = 0.5;
    if (o.height >= 0.0 && std::isfinite(o.height) && !o.bot)      // a role's Height, at the spawn only: the passport copy the game reads at BeginPlay
        if (UObject* st = FindObject(kPassportStruct, "UserDefinedStruct"))
            if (FProperty* h = FindPropertyPrefix(st, "Height_"); h && PropertySize(h) == sizeof(double)) {
                height = std::clamp(o.height, 0.0, hsm::kMaxSpawnHeight);   // past 1.0: the Juggernaut's INI scale; the game's own height map runs unclamped
                std::memcpy(reinterpret_cast<uint8_t*>(willie) + PropertyOffset(dst) + PropertyOffset(h), &height, sizeof height);
            }
    ApplyBuild(willie, dst, o.bot, o.weight);
    if (bare_) StripEquipment(willie, dst);
    return true;
}

// Brawl: empties the maps of the passport's Equipment (armour in slots, weapons in slots, weapons in hands) on the
// spawned fighter's own copy, so it comes up with no armour and fists in both hands. The copy was made from the blob or
// the stock slot above; neither (nor the game instance's passport, nor any save) is touched.
void Adapter::StripEquipment(UObject* willie, FProperty* passport) {
    UObject* st = FindObject(kPassportStruct, "UserDefinedStruct");
    FProperty* eq = st ? FindPropertyPrefix(st, "Equipment_") : nullptr;
    UObject* sub = FindObject(kEquipmentStruct, "UserDefinedStruct");
    int cleared = 0;
    if (eq && sub) {
        uint8_t* at = reinterpret_cast<uint8_t*>(willie) + PropertyOffset(passport) + PropertyOffset(eq);
        for (FProperty* p : StructProperties(sub))
            if (PropertyType(p) == "MapProperty") { ClearPropertyValue(p, at + PropertyOffset(p)); ++cleared; }
    }
    static bool warned = false;
    if (!cleared && !warned) { warned = true; HSM_LOG("adapter: brawl: the passport's Equipment has no maps to clear; fighters keep their gear"); }
    HSM_LOG("adapter: %s spawned bare (%d equipment map(s) cleared)", ObjectName(willie).c_str(), cleared);
}

// The tier's build on the spawned fighter's own copy of the passport (never the save or the generator's). The Weight
// is the Muscle Rate in the game's Willie_BP: the body's mass scale runs 1..1.25 over 0..1 and the grip's hold limit
// 1..1.5, so two players with different Weights are different bodies (a Free Mode 2.0 made one fighter 33% heavier with a 60%
// stronger grip: #294 #295 #301). A player gets exactly the forced Weight (SetBuild); a bot, or any fighter with
// force_strength=0, its own clamped to 0..1.
void Adapter::ApplyBuild(UObject* willie, FProperty* passport, bool bot, double roleWeight) {
    UObject* st = FindObject(kPassportStruct, "UserDefinedStruct");
    FProperty* w = st ? FindPropertyPrefix(st, "Weight_") : nullptr;
    if (!w || PropertySize(w) != sizeof(double)) {
        static bool logged = false;
        if (!logged) HSM_LOG("adapter: the passport has no double Weight; builds stay as they are");
        logged = true;
        return;
    }
    uint8_t* at = reinterpret_cast<uint8_t*>(willie) + PropertyOffset(passport) + PropertyOffset(w);
    double own = 0;
    std::memcpy(&own, at, sizeof own);
    const bool role = roleWeight >= 0.0 && std::isfinite(roleWeight) && !bot;   // a role's Weight is the mode's rule: it holds with force_strength off too
    const bool force = (buildOn_ || role) && !bot;
    const double want = BuildWeight(own, role ? roleWeight : buildWeight_, force);
    if (want != own) std::memcpy(at, &want, sizeof want);          // a NaN own compares unequal too
    const std::string name = ObjectName(willie);
    if (force)
        HSM_LOG("adapter: %s build forced (Weight %.3f -> %.3f, tier %d, mass x%.2f, grip x%.2f)", name.c_str(), own, want, buildTier_,
                MassScaleFor(want), GripScaleFor(want));
    else if (want != own)
        HSM_LOG("adapter: %s build clamped (Weight %.3f -> %.3f, mass x%.2f, grip x%.2f)%s", name.c_str(), own, want, MassScaleFor(want), GripScaleFor(want),
                bot ? " (bot)" : "");
    else if (!bot)
        HSM_LOG("adapter: %s build kept (Weight %.3f, mass x%.2f, grip x%.2f)", name.c_str(), own, MassScaleFor(want), GripScaleFor(want));
}

// A weapon or foot's Kick Power: what the game multiplies a hit's push by (Willie_BP Deal Complex Damage). The piece a hand lets go of
// gets the game's value back, because anyone can pick it up next.
static void ApplyPiecePower(UObject* now, double multiplier, Adapter::ClassPiece& piece) {
    if (piece.what != now) {
        if (piece.what && ObjectAt(piece.index) == piece.what) if (double* p = Field<double>(piece.what, "Kick Power")) if (piece.power.seen) *p = piece.power.base;
        piece = Adapter::ClassPiece{};
        piece.what = now; piece.index = now ? ObjectIndex(now) : -1;
    }
    if (!now) return;
    if (double* p = Field<double>(now, "Kick Power")) *p = piece.power.Next(*p, multiplier);
}

// The game sets this from the Muscle Rate at a spawn and at a character reset, and nothing reads it back, so it changes the skin and no physics.
bool Adapter::SetMusclesLook(UObject* willie, double value) {
    UObject* const* const material = willie ? Field<UObject*>(willie, "Body Material") : nullptr;
    if (!material || !*material) return false;
    return Call(*material, "SetScalarParameterValue").setName("ParameterName", MakeName("Muscles")).set<float>("Value", float(value)).invoke();
}

void Adapter::ClearPain(UObject* willie) {
    static constexpr const char* kPainDoubleFields[] = {"Pain", "Pain Upper Body", "Pain Lower Body", "Pain Neck", "Pain Head", "Pain Leg L", "Pain Leg R", "Pain Arm L", "Pain Arm R"};
    if (!willie) return;
    for (const char* name : kPainDoubleFields) if (double* p = Field<double>(willie, name)) *p = 0.0;
    SetBool(willie, "Pain Shock", false);
}

// Strength and Muscle on this player's own fighter, from the lobby's build or their own. The same steps as the forced build (ApplyBuildLive).
void Adapter::ApplyClassWeight(UObject* willie, double base, const ClassSheet& sheet, ClassTracks& tracks) {
    double* rate = willie ? Field<double>(willie, "Muscle Rate") : nullptr;
    double* mass = willie ? Field<double>(willie, "Mass Scale (Set in BP)") : nullptr;
    if (!rate) return;
    const double want = ClassWeight(base, sheet), rateNow = ClassHoldRate(want, sheet, tracks.holding), newMass = ClassMassScale(want, sheet);
    const double oldMass = mass ? *mass : 0.0;
    // The game sets this field itself about 0.2 s into a fighter's life, along with every bone, and only from the Muscle Rate. So it's checked every
    // time: a field that's been reset gets the class's Weight back, on the bones it was just set on.
    const bool massOk = !mass || !std::isfinite(oldMass) || oldMass <= 0.0 || oldMass == newMass;
    if (tracks.weightSet == want && massOk && (*rate == want || *rate == tracks.rateWritten)) {
        if (*rate != rateNow) HSM_LOG("classes: grappler Muscle Rate %.2f (%.2f when not holding)", rateNow, want);
        *rate = tracks.rateWritten = rateNow;
        return;
    }
    *rate = want;
    tracks.weightSet = want;
    int bodies = 0;
    UObject* mesh = MeshOf(willie);
    if (mass && std::isfinite(oldMass) && oldMass > 0.0 && oldMass != newMass && mesh && ResolveBones(mesh)) {
        const double k = newMass / oldMass;
        for (const FName& bone : bones_) {
            Call g(mesh, "GetMassScale");
            if (!g.valid() || !g.setName("BoneName", bone).invoke()) continue;
            const float s = g.get<float>("ReturnValue");
            if (!std::isfinite(s) || s <= 0.f) continue;
            Call set(mesh, "SetMassScale");
            if (set.valid() && set.setName("BoneName", bone).set<float>("InMassScale", float(s * k)).invoke()) ++bodies;
        }
        *mass = newMass;
        tracks.massCheckIn = 30;
    }
    *rate = tracks.rateWritten = rateNow;
    HSM_LOG("classes: Muscle Rate %.2f on %s (base %.2f; class weight x%.2f, mass scale %.3f on %d bodies, grip x%.2f)", want, ObjectName(willie).c_str(), base, sheet.mass, newMass, bodies, GripScaleFor(want));
}

UObject* Adapter::HeldFighterMesh(UObject* willie) {
    if (!willie) return nullptr;
    for (const char* hand : {"R", "L"}) {
        bool grabbed = false;
        if (!GetBool(willie, std::string("Grabbed ") + hand, grabbed) || !grabbed) continue;
        UObject** const comp = Field<UObject*>(willie, std::string("Grab Component ") + hand);
        UObject* const owner = comp && *comp ? OuterOf(*comp) : nullptr;
        if (owner && owner != willie && Field<double>(owner, "Mass Scale (Set in BP)") && MeshOf(owner) == *comp) return *comp;
    }
    return nullptr;
}

// Grappler under host physics: what's held counts as a plain body with no armour while it's held, and every bone goes back to what it was on letting go.
void Adapter::LightenHeld(UObject* target, ClassTracks& tracks) {
    if (target == tracks.heldMesh) return;
    if (tracks.heldMesh && StillValid(tracks.heldMesh, tracks.heldIndex)) {
        for (const auto& [bone, scale] : tracks.heldScales) Call(tracks.heldMesh, "SetMassScale").setName("BoneName", bone).set<float>("InMassScale", scale).invoke();
        HSM_LOG("classes: grappler let go: %zu bodies back to their weight", tracks.heldScales.size());
    }
    tracks.heldMesh = nullptr; tracks.heldScales.clear();
    if (!target || !ResolveBones(target)) return;
    tracks.heldMesh = target; tracks.heldIndex = ObjectIndex(target);
    const float plain = float(MassScaleFor(kDefaultWeight));
    for (const FName& bone : bones_) {
        Call g(target, "GetMassScale");
        if (!g.valid() || !g.setName("BoneName", bone).invoke()) continue;
        const float s = g.get<float>("ReturnValue");
        if (!std::isfinite(s) || s <= 0.f) continue;
        tracks.heldScales.emplace_back(bone, s);
        Call(target, "SetMassScale").setName("BoneName", bone).set<float>("InMassScale", plain).invoke();
    }
    HSM_LOG("classes: grappler holding %s: %zu bodies at a plain fighter's weight", ObjectName(target).c_str(), tracks.heldScales.size());
}

void Adapter::ReadLiveStats(UObject* willie, double& run, double& strength) {
    run = strength = 0;
    if (!willie) return;
    if (double* const r = Field<double>(willie, "Running Speed Rate")) run = *r;
    if (double* const s = Field<double>(willie, "Muscle Power")) strength = *s;
}

void Adapter::ApplyClassStats(UObject* willie, const ClassSheet& sheet, ClassTracks& tracks) {
    if (!willie) return;
    if (tracks.who != willie) {
        // A new fighter. The hands' weapons outlive the last one, so they go back first.
        ApplyPiecePower(nullptr, 1.0, tracks.handR); ApplyPiecePower(nullptr, 1.0, tracks.handL);
        LightenHeld(nullptr, tracks);
        const bool logged = tracks.loggedMissing, lighten = tracks.lightenHeld;
        tracks = ClassTracks{};
        tracks.who = willie; tracks.loggedMissing = logged; tracks.lightenHeld = lighten;
    }
    UObject** const footR = Field<UObject*>(willie, "Foot R Weapon");
    UObject** const footL = Field<UObject*>(willie, "Foot L Weapon");
    UObject** const handR = Field<UObject*>(willie, "Weapon R");
    UObject** const handL = Field<UObject*>(willie, "Weapon L");
    ApplyPiecePower(footR ? *footR : nullptr, sheet.kick, tracks.footR);
    ApplyPiecePower(footL ? *footL : nullptr, sheet.kick, tracks.footL);
    ApplyPiecePower(handR ? *handR : nullptr, sheet.knockback, tracks.handR);
    ApplyPiecePower(handL ? *handL : nullptr, sheet.knockback, tracks.handL);
    UObject* const held = sheet.grappler ? HeldFighterMesh(willie) : nullptr;
    tracks.holding = held != nullptr;
    if (tracks.lightenHeld || tracks.heldMesh) LightenHeld(tracks.lightenHeld ? held : nullptr, tracks);
    double* const gameMass = Field<double>(willie, "Mass Scale (Set in BP)");
    // Three seconds on, what it came to: if this reads like a plain fighter, something undid the class's weight.
    if (tracks.massCheckIn > 0 && --tracks.massCheckIn == 0) {
        UObject* const mesh = MeshOf(willie);
        if (mesh && ResolveBones(mesh)) {
            Call g(mesh, "GetMassScale");
            if (g.valid() && g.setName("BoneName", bones_[0]).invoke()) HSM_LOG("classes: weight check: %s scale %.3f (class x%.2f, game's %.3f)", boneNames_[0].c_str(), g.get<float>("ReturnValue"), sheet.mass, gameMass ? *gameMass : 0.0);
        }
    }
    double* const run = Field<double>(willie, "Running Speed Rate");
    float* const walk = Field<float>(willie, "Walk Speed Rate Run");
    double* const strength = Field<double>(willie, "Muscle Power");
    double* const gripR = Field<double>(willie, "R Grab Force Limit");
    double* const gripL = Field<double>(willie, "L Grab Force Limit");
    double* const hands = Field<double>(willie, "Hands Rigidity (Gauntlets)");
    if (!tracks.loggedMissing && !(run && walk && strength && gripR && gripL && hands)) {
        tracks.loggedMissing = true;
        HSM_LOG("classes: fields missing on %s: run %d walk %d strength %d grip %d/%d hands %d", ObjectName(willie).c_str(), run != nullptr, walk != nullptr, strength != nullptr, gripR != nullptr, gripL != nullptr, hands != nullptr);
    }
    if (run) *run = tracks.run.Next(*run, sheet.run);
    if (walk) *walk = float(tracks.walk.Next(*walk, sheet.walk));
    if (strength) *strength = tracks.strength.Next(*strength, 1.0);   // only read: for the base stat check. It drives a severed body and nothing else
    if (gripR) *gripR = tracks.gripR.Next(*gripR, sheet.grip);
    if (gripL) *gripL = tracks.gripL.Next(*gripL, sheet.grip);
    if (hands) *hands = tracks.hands.Next(*hands, sheet.hands);
    if (double* const regen = Field<double>(willie, "Regen Rate")) *regen = tracks.regen.Next(*regen, sheet.recovery);
    if (double* const stamina = Field<double>(willie, "Stamina")) {
        const double* const health = Field<double>(willie, "Health");
        if (tracks.staminaLast >= 0.0) *stamina = ClassStaminaNext(tracks.staminaLast, *stamina, ClassEnduranceNow(sheet, health ? *health : 100.0));
        tracks.staminaLast = *stamina;
    }
    if (sheet.quickKicks) SetBool(willie, "Kick Cooldown", false);
    // Bone Snapping is what the game multiplies a hit by before asking whether a bone gives (Willie_BP Get Damage), so sturdier bones
    // divide it. Block Spine Breaking is what its dislocation check looks at for every limb.
    if (double* const snap = Field<double>(willie, "Bone Snapping")) *snap = tracks.bones.Next(*snap, 1.0 / sheet.bones);
    if (sheet.noDislocation) SetBool(willie, "Block Spine Breaking", true);
}

// The forced build on a live fighter: the game read the save's Weight at BeginPlay (Muscle Rate, UserConstructionScript)
// and set the mass scale from it 0.2 s later (Willie_BP: SetAllMassScale(1..1.25 over the Muscle Rate)); worn armour and
// held weapons then added their weight per bone, scaled by that same "Mass Scale (Set in BP)" (Add and Remove Weapon
// Weights). So every body's mass scale is multiplied by new/old, which keeps the armour's share. Before the 0.2 s set
// (mass scale still 0) only the Muscle Rate and the passport change, and the game builds the mass from them.
void Adapter::ApplyBuildLive(UObject* willie) {
    if (!willie || !buildOn_) return;
    bool zombie = false;
    if (GetBool(willie, "Is Zombie?", zombie) && zombie) return;
    UObject* st = FindObject(kPassportStruct, "UserDefinedStruct");
    FProperty* passport = FindProperty(ClassOf(willie), "Character Passport");
    FProperty* w = st ? FindPropertyPrefix(st, "Weight_") : nullptr;
    double* rate = Field<double>(willie, "Muscle Rate");
    double* mass = Field<double>(willie, "Mass Scale (Set in BP)");
    const std::string name = ObjectName(willie);
    if (!passport || !w || PropertySize(w) != sizeof(double) || !rate || !mass) {
        HSM_LOG("adapter: %s live build not set (passport %d, Weight %d, Muscle Rate %d, mass scale %d)", name.c_str(), passport != nullptr,
                w != nullptr, rate != nullptr, mass != nullptr);
        return;
    }
    uint8_t* at = reinterpret_cast<uint8_t*>(willie) + PropertyOffset(passport) + PropertyOffset(w);
    double own = 0;
    std::memcpy(&own, at, sizeof own);
    const double want = BuildWeight(own, buildWeight_, true);
    const double oldRate = *rate, oldMass = *mass, newMass = MassScaleFor(want);
    std::memcpy(at, &want, sizeof want);
    *rate = want;
    int bodies = 0;
    UObject* mesh = MeshOf(willie);
    if (std::isfinite(oldMass) && oldMass > 0.0 && mesh && ResolveBones(mesh)) {
        const double k = newMass / oldMass;
        for (const FName& bone : bones_) {
            Call g(mesh, "GetMassScale");
            if (!g.valid() || !g.setName("BoneName", bone).invoke()) continue;
            const float s = g.get<float>("ReturnValue");
            if (!std::isfinite(s) || s <= 0.f) continue;
            Call set(mesh, "SetMassScale");
            if (set.valid() && set.setName("BoneName", bone).set<float>("InMassScale", float(s * k)).invoke()) ++bodies;
        }
        *mass = newMass;
    }
    HSM_LOG("adapter: %s live build forced (Weight %.3f -> %.3f, Muscle Rate %.3f, tier %d, mass x%.2f -> x%.2f on %d bodies, grip x%.2f)",
            name.c_str(), own, want, oldRate, buildTier_, oldMass, newMass, bodies, GripScaleFor(want));
}

// #36 evidence: how the spawned fighter's head is set up (Face Type, the passport's Weight, the head material's
// 'Body Hidden' scalar: 1 hides the body texture on the head/neck), to read against a screenshot.
static void LogAppearance(UObject* w) {
    const int32_t* face = Field<int32_t>(w, "Face Type");
    double weight = -1;
    if (FProperty* passport = FindProperty(ClassOf(w), "Character Passport"))
        if (UObject* st = FindObject(kPassportStruct, "UserDefinedStruct"))
            if (FProperty* wp = FindPropertyPrefix(st, "Weight_"); wp && PropertySize(wp) == sizeof(double))
                std::memcpy(&weight, reinterpret_cast<uint8_t*>(w) + PropertyOffset(passport) + PropertyOffset(wp), sizeof weight);
    float hidden = -1;
    UObject* const* head = Field<UObject*>(w, "Head Material");
    if (head && *head) {
        Call get(*head, "K2_GetScalarParameterValue");
        get.setName("ParameterName", MakeName("Body Hidden"));
        if (get.valid() && get.invoke()) hidden = get.get<float>("ReturnValue");
    }
    HSM_LOG("adapter: %s appearance: Face Type %d, Weight %.3f, head 'Body Hidden' %.2f", ObjectName(w).c_str(), face ? int(*face) : -1, weight,
            double(hidden));
}

UObject* Adapter::SpawnFighter(const SpawnOptions& o) {
    const auto t0 = std::chrono::steady_clock::now();   // the line below says what the spawn took (round restarts: one a frame)
    UClass* willie = FindClass(o.fighterClass ? o.fighterClass : kWillieClass);   // Zombies' bots: the zombie class (match/hsm_fighter_class.h)
    if (!willie && o.fighterClass && std::string(o.fighterClass) == kZombieClass) {
        // Infection on an arena map: nothing there references the zombie Willie yet (pre24 tester feedback).
        willie = LoadClassBlocking("/Game/Character/Blueprints/Willie_BP_Zombie.Willie_BP_Zombie_C");
        HSM_LOG("adapter: zombie class %s", willie ? "loaded" : "could not be loaded; spawning the stock fighter");
        if (!willie) willie = FindClass(kWillieClass);
    }
    UClass* aiCtrl = o.brain && !o.stockBrain ? FindClass("AIController") : nullptr;
    if (!willie || (o.brain && !o.stockBrain && !aiCtrl)) { HSM_LOG("adapter: spawn classes missing"); return nullptr; }
    double rad = o.yawDeg * 0.017453292519943295;
    FTransform t; t.rotation = {0, 0, std::sin(rad / 2), std::cos(rad / 2)}; t.translation = o.at;
    Call begin(statics_, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", world_).set<UClass*>("ActorClass", willie).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 2).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* w = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!w) { HSM_LOG("adapter: BeginDeferredActorSpawnFromClass failed"); return nullptr; }
    double height = 0.5;
    CopyPassport(w, o, height);
    if (int32_t* team = Field<int32_t>(w, "Team Int")) *team = o.teamInt;
    if (!o.stockBrain) {
        if (UClass** cls = Field<UClass*>(w, "AIControllerClass")) *cls = aiCtrl;
        if (uint8_t* ap = Field<uint8_t>(w, "AutoPossessAI")) *ap = o.brain ? 3 : 0;   // EAutoPossessAI PlacedInWorldOrSpawned / Disabled
    }
    // Same height mapping as the stock spawner: lift 0..10 cm and scale 0.875..1. The game's own map is
    // MapRangeUnclamped, so a height past 1.0 (the Juggernaut's INI scale, match/hsm_roles.h) rides the same line: 3.8 -> 1.35.
    height = std::clamp(height, 0.0, hsm::kMaxSpawnHeight);
    t.translation.z += 10.0 * height;
    double sc = 0.875 + 0.125 * height; t.scale = {sc, sc, sc};
    if (height > 1.0 && !heightLogged_) {   // once: a fighter built past the stock cap, the tester's evidence
        heightLogged_ = true;
        HSM_LOG("adapter: spawned a fighter at height %.2f (scale %.3f)", height, sc);
    }
    // The career's body as the start body, as the stock spawner sets it (BP_SpawnerPoint_Willies.cpp:144); the fighter's
    // BeginPlay copies it onto its limbs and health (Willie_BP.cpp:14702-14710), after FinishSpawningActor below.
    if (o.careerBody) {
        UObject* gi = GameInstance();
        FProperty* src = gi ? FindProperty(ClassOf(gi), "Player Body Condition") : nullptr;
        FProperty* dst = FindProperty(ClassOf(w), "Start Body Condition");
        if (src && dst && PropertySize(src) == PropertySize(dst) && PropertyType(src) == PropertyType(dst))
            CopyPropertyValue(dst, reinterpret_cast<uint8_t*>(w) + PropertyOffset(dst), reinterpret_cast<const uint8_t*>(gi) + PropertyOffset(src));
        else HSM_LOG("adapter: the career's body could not be carried into the fighter (fields missing or mismatched); it starts whole");
    }
    // A career Brawl or Blossfechten fight: the stock spawner's kit flags, on every fighter as the stock spawner does
    // (session/hsm_career.h CareerSpawnKitFor); the fighter's BeginPlay leaves the weapons off by them.
    const hsm::CareerSpawnKit kit = hsm::CareerSpawnKitFor(FreeModeActive(), CareerCombatMode());
    if ((kit.pants && !SetBool(w, "Spawn in Pants", true)) || (kit.blossfechten && !SetBool(w, "Blossfechten Gear", true)))
        HSM_LOG("adapter: the fight's %s kit could not be set on the fighter (field missing)", kit.pants ? "Brawl" : "Blossfechten");
    else if (kit.pants || kit.blossfechten)   // a few fighters a fight: each one is the tester's evidence
        HSM_LOG("adapter: a career %s: this fighter starts %s", kit.pants ? "Brawl" : "Blossfechten", kit.pants ? "in pants, no weapons" : "in training gear");
    Call finish(statics_, "FinishSpawningActor");
    finish.set<UObject*>("Actor", w).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    UObject* done = finish.invoke() ? finish.get<UObject*>("ReturnValue") : nullptr;
    if (!done) { HSM_LOG("adapter: FinishSpawningActor failed"); return nullptr; }
    if (o.brain && !o.stockBrain) {
        UObject* ctrl = ControllerOf(done);
        if (!ctrl) HSM_LOG("adapter: spawned fighter has no controller");
        else SetBool(ctrl, "bSetControlRotationFromPawnOrientation", false);   // aim comes from the remote player
    }
    HSM_LOG("adapter: spawned %s at (%.0f, %.0f, %.0f)%s in %.1f ms", o.stockBrain ? "bot" : o.brain ? "fighter" : "puppet", o.at.x, o.at.y, o.at.z,
            o.blob || o.passport ? "" : " with default loadout", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    LogAppearance(done);
    return done;
}

UObject* Adapter::ControllerOf(UObject* pawn) {
    Call c(pawn, "GetController");
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

void Adapter::KeepRemoteFighter(UObject* willie, float yawDeg, float pitchDeg) {
    SetBool(willie, "Player", true);   // Willie computes Player once from local player 0/1; mod fighters are players too
    // No lock-on here (sync audit item 18): it reads the host's Camera Lock On and writes its own SetControlRotation over
    // the client's sent one (Willie_BP.cpp:16529, 16549). The client's camera is its own.
    if (double* lock = Field<double>(willie, "Lock on Strength")) *lock = 0.0;
    game::RefreshDamageCache(willie);   // its cached damage rate was set before that (gameplay/damage_pin.h)
    UObject* ctrl = nullptr;
    if (lean_) if (UObject** c = Field<UObject*>(willie, "Controller")) ctrl = *c;   // the pawn's own field: no GetController call
    if (!ctrl) ctrl = ControllerOf(willie);
    if (!ctrl) return;
    if (lean_) {
        // The rotation stays once set (hsm_drivelean.h ControlRotationGate); the table forgets fighters long gone.
        if (rotationGate_.size() > 32) rotationGate_.clear();
        const double nowMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!rotationGate_[willie].Needed(ctrl, yawDeg, pitchDeg, nowMs)) return;
    }
    Call(ctrl, "SetControlRotation").set("NewRotation", FRotator{pitchDeg, yawDeg, 0}).invoke();
}

bool Adapter::EnsureBrain(UObject* willie) {
    if (!willie) return false;
    if (ControllerOf(willie)) return true;
    UClass* aiCtrl = FindClass("AIController");
    if (UClass** cls = Field<UClass*>(willie, "AIControllerClass"); cls && aiCtrl) *cls = aiCtrl;
    Call(willie, "SpawnDefaultController").invoke();
    UObject* ctrl = ControllerOf(willie);
    if (ctrl) SetBool(ctrl, "bSetControlRotationFromPawnOrientation", false);   // aim comes from the remote player
    HSM_LOG("adapter: %s %s a controller for the hand-off", ObjectName(willie).c_str(), ctrl ? "got" : "could not get");
    return ctrl != nullptr;
}

// The game's mouse look scale (GI_Settings: Willie_BP.cpp:11407, 11542 multiply the mouse axes by it, Y also by the
// invert). Host physics: a client's mouse axes go out multiplied by its own, and the host divides its own back out
// (sync audit item 16).
bool Adapter::MouseLook(float& sensitivity, bool& invert) {
    UObject* gi = GameInstance();
    double* sens = gi ? Field<double>(gi, "Mouse Sensitivity") : nullptr;
    if (!sens || !std::isfinite(*sens)) return false;
    sensitivity = float(*sens);
    invert = false;
    GetBool(gi, "Invert Mouse", invert);
    return true;
}

// Willie_BP ReceiveTick (Willie_BP.cpp:16513): FInterpTo(Look Sensiticity, Any_Guarding ? 0.25 : 1, dt, Any_Guarding ? 0 : 5);
// an FInterpTo at speed 0 is the target at once.
std::vector<UObject*> Adapter::StuckTargets(UObject* willie) {
    std::vector<UObject*> out;
    if (!willie) return out;
    for (const char* hand : {"Weapon R", "Weapon L"}) {
        UObject** wp = Field<UObject*>(willie, hand);
        UObject* weapon = wp ? *wp : nullptr;
        if (!weapon || !StillValid(weapon, ObjectIndex(weapon))) continue;
        const auto* arr = Field<FObjArray>(weapon, "Stuck Constraints Array");
        if (!arr || !arr->data || arr->num <= 0 || arr->num > 256) continue;
        for (int32_t i = 0; i < arr->num; ++i) {
            UObject* k = arr->data[i];
            if (!k || !StillValid(k, ObjectIndex(k))) continue;
            bool on = false;
            UObject** hit = Field<UObject*>(k, "Hit Actor");
            if (hit && *hit && GetBool(k, "Activate Constraint", on) && on && std::find(out.begin(), out.end(), *hit) == out.end())
                out.push_back(*hit);
        }
    }
    return out;
}

void Adapter::TickLookSensitivity(UObject* willie, float dt) {
    double* sens = willie ? Field<double>(willie, "Look Sensiticity") : nullptr;
    if (!sens) return;
    bool guarding = false;
    GetBool(willie, "Any_Guarding", guarding);
    const double target = guarding ? 0.25 : 1.0;
    if (!std::isfinite(*sens) || guarding) { *sens = target; return; }
    const double step = std::clamp(double(dt) * 5.0, 0.0, 1.0);
    *sens += (target - *sens) * step;
    if (std::fabs(target - *sens) < 1e-4) *sens = target;
}

// Every fighter with Player set points player 0's audio listener at its own head once (a DoOnce, Willie_BP.cpp:19304-
// 19317), so under host physics each client's fighter took the host's ears (sync audit item 17). Points it back.
void Adapter::KeepAudioListener(UObject* mine) {
    UObject* ctrl = ControllerOf(mine);
    UObject** cam = Field<UObject*>(mine, "First Person Camera");
    if (ctrl && cam && *cam) Call(ctrl, "SetAudioListenerAttenuationOverride").set<UObject*>("AttachToComponent", *cam).invoke();   // location left zero, as the game's
}

void Adapter::DestroyActor(UObject* actor) {
    if (!actor) return;
    Call(actor, "K2_DestroyActor").invoke();
    static bool reported = false;   // the in-game proof that AllWillies can tell destroyed fighters by their flag
    if (!reported) { reported = true; HSM_LOG("adapter: a destroyed actor is %smarked as garbage", IsGarbage(actor) ? "" : "NOT "); }
}

// Each object is asked its own class's question: AActor::IsActorBeingDestroyed for an actor, UActorComponent::IsBeingDestroyed
// for a component, nothing for anything else. Neither thunk checks its class (shipping ProcessEvent does not either): the
// actor's reads byte 0x5D, which on a component is bit 40 of its ComponentTags array pointer, so a tagged mesh read as
// "being destroyed" on one machine and not on the other (docs/TWO_PLAYER_TEST_5.md section 1).
bool Adapter::BeingDestroyed(UObject* o) {
    static UClass* actorClass = nullptr;
    static UClass* componentClass = nullptr;
    static UFunction* actorFn = nullptr;
    static UFunction* componentFn = nullptr;
    // Prepared once (native classes, they outlive every level): this runs for each carried actor of each fighter every frame
    // (StillValid), and a Call looks its function and parameter up by name on every use.
    static PreparedCall actorCall, componentCall;
    if (!actorFn && (actorClass = FindClass("Actor"))) {
        actorFn = FindFunction(actorClass, "IsActorBeingDestroyed");
        if (actorFn) actorCall = PreparedCall(actorClass, "IsActorBeingDestroyed", {"ReturnValue"});
    }
    if (!componentFn && (componentClass = FindClass("ActorComponent"))) {
        componentFn = FindFunction(componentClass, "IsBeingDestroyed");
        if (componentFn) componentCall = PreparedCall(componentClass, "IsBeingDestroyed", {"ReturnValue"});
    }
    if (!o) return false;
    PreparedCall* pc = actorFn && actorCall.valid() && IsA(o, actorClass) ? &actorCall
                     : componentFn && componentCall.valid() && IsA(o, componentClass) ? &componentCall : nullptr;
    if (!pc) return false;
    return pc->invoke(o) && pc->take<bool>(0);
}

bool Adapter::StillValid(UObject* o, int32_t index) const {
    if (!o || index < 0 || ObjectAt(index) != o) return false;
    return !destroyAware_ || (!IsGarbage(o) && !BeingDestroyed(o));
}

// What a fighter carries: its held and sheathed weapons, quiver, feet and fists, and every actor attached to it (armour). The
// weapons are actors of their own and are not owned by the fighter; the list holds no duplicates and may hold other fighters.
std::vector<UObject*> Adapter::CarriedActors(UObject* willie) {
    std::vector<UObject*> carried;
    for (const char* f : {"Weapon R", "Weapon L", "Attached Bolt Quiver"})
        if (UObject** p = Field<UObject*>(willie, f); p && *p) carried.push_back(*p);
    // Also its feet, the fists of a cut-off upper body and its sheathed weapons: actors of their own, constrained to the
    // body, not attached (Willie_BP.cpp:18941, 18811, 7007). A slot variable can outlive its weapon's sheath, so a weapon
    // is taken only while its "Parent Actor" is still this fighter.
    for (const char* f : {"Foot R Weapon", "Foot L Weapon", "Sapwned R Hand Weapon Upper Body", "Sapwned L Hand Weapon Upper Body",
                          "Weapon Slot R 1", "Weapon Slot R 2", "Weapon Slot Back", "Weapon Slot L 1", "Weapon Slot L 2"})
        if (UObject** p = Field<UObject*>(willie, f); p && *p)
            if (UObject** parent = Field<UObject*>(*p, "Parent Actor"); parent && *parent == willie) carried.push_back(*p);
    Call att(willie, "GetAttachedActors");
    att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", true);
    if (att.invoke())
        if (FObjArray* arr = static_cast<FObjArray*>(att.paramPtr("OutActors")); arr && arr->num > 0 && arr->num <= 256)
            for (int32_t i = 0; i < arr->num; ++i) carried.push_back(arr->data[i]);
    std::sort(carried.begin(), carried.end());
    carried.erase(std::unique(carried.begin(), carried.end()), carried.end());
    return carried;
}

void Adapter::StopHair(UObject* willie, bool destroy) {
    UObject** p = Field<UObject*>(willie, "Hair");
    UObject* hair = p ? *p : nullptr;
    if (!hair || IsGarbage(hair) || BeingDestroyed(hair)) return;   // none, or already going: the game's Destroy Hair skips it too
    Call off(hair, "SetEnableSimulation");
    const bool stopped = off.valid() && off.set<bool>("bEnable", false).invoke();
    if (destroy) Call(hair, "K2_DestroyComponent").invoke();
    HSM_LOG("adapter: hair of %s simulation %s%s", ObjectName(willie).c_str(), stopped ? "off" : "NOT turned off", destroy ? ", destroyed" : "");
}

namespace {
constexpr double kParkMaxMs = 3000;   // a parked fighter is destroyed by then whatever the paint plugin says
double NowMsLocal() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

// HasAnyTasksQueuedUp on the fighter's VertexPaintDetectionComponent (REVERSE_ENGINEERING.md: check it before changing
// a painted mesh). False when the component or the call is missing.
bool Adapter::PaintTasksQueued(UObject* willie) {
    if (!paintClassLooked_) {
        paintClassLooked_ = true;
        paintClass_ = FindClass("VertexPaintDetectionComponent");
        HSM_LOG("adapter: paint component class %s (fighters %s for paint tasks before they are destroyed)", paintClass_ ? "ok" : "MISSING",
                paintClass_ ? "wait" : "do not wait");
    }
    if (!paintClass_) return false;
    Call get(willie, "GetComponentByClass");
    get.set<UClass*>("ComponentClass", paintClass_);
    UObject* comp = get.valid() && get.invoke() ? get.get<UObject*>("ReturnValue") : nullptr;
    if (!comp) return false;
    Call busy(comp, "HasAnyTasksQueuedUp");
    return busy.valid() && busy.invoke() && busy.get<bool>("ReturnValue");
}

void Adapter::FinishDestroy(Parked& pk, const char* why, double t0Ms) {
    int destroyed = 0;
    for (auto& [a, i] : pk.carried) {
        if (!StillValid(a, i)) continue;
        DestroyActor(a);
        ++destroyed;
    }
    const std::string name = ObjectName(pk.willie);
    if (StillValid(pk.willie, pk.index)) DestroyActor(pk.willie);
    HSM_LOG("adapter: destroyed %s and %d carried actor(s) in %.1f ms (%s)", name.c_str(), destroyed, NowMsLocal() - t0Ms, why);
}

void Adapter::DestroyFighter(UObject* willie, int32_t index) {
    if (!StillValid(willie, index)) return;
    for (const Parked& pk : parked_) if (pk.willie == willie && pk.index == index) return;   // already on its way
    const double t0 = NowMsLocal();
    Parked pk;
    pk.willie = willie; pk.index = index; pk.sinceMs = t0;
    UClass* fighter = FindClass(kWillieClass);
    for (UObject* a : CarriedActors(willie)) {
        if (!a || a == willie || IsA(a, fighter)) continue;
        pk.carried.push_back({a, ObjectIndex(a)});
    }
    StopHair(willie, true);
    if (!PaintTasksQueued(willie)) { FinishDestroy(pk, "now", t0); return; }
    // The plugin's queued task would run on the freed mesh (#72): out of sight until it is done (TickDeferredDestroys).
    Call(willie, "SetActorHiddenInGame").set<bool>("bNewHidden", true).invoke();
    Call(willie, "SetActorEnableCollision").set<bool>("bNewActorEnableCollision", false).invoke();
    for (auto& [a, i] : pk.carried) {
        if (!StillValid(a, i)) continue;
        Call(a, "SetActorHiddenInGame").set<bool>("bNewHidden", true).invoke();
        Call(a, "SetActorEnableCollision").set<bool>("bNewActorEnableCollision", false).invoke();
    }
    HSM_LOG("adapter: %s has paint tasks queued; parked out of sight until they are done (%zu carried actor(s))",
            ObjectName(willie).c_str(), pk.carried.size());
    parked_.push_back(std::move(pk));
}

void Adapter::TickDeferredDestroys() {
    const double now = NowMsLocal();
    for (auto it = parked_.begin(); it != parked_.end();) {
        Parked& pk = *it;
        if (!StillValid(pk.willie, pk.index)) { it = parked_.erase(it); continue; }   // gone with the level
        const bool waited = now - pk.sinceMs >= kParkMaxMs;
        if (!waited && PaintTasksQueued(pk.willie)) { ++it; continue; }
        FinishDestroy(pk, waited ? "parked; paint tasks still queued after the wait" : "parked until its paint tasks were done", pk.sinceMs);
        it = parked_.erase(it);
    }
}

FVector Adapter::ActorLocation(UObject* actor) {
    Call c(actor, "K2_GetActorLocation");
    return c.invoke() ? c.get<FVector>("ReturnValue") : FVector{};
}

double Adapter::ActorYaw(UObject* actor) {
    Call c(actor, "K2_GetActorRotation");
    return c.invoke() ? c.get<FRotator>("ReturnValue").yaw : 0.0;
}

// GameplayStatics::GetTimeSeconds: a double in UE5 (a float before); read as whichever the function declares.
double Adapter::WorldSeconds() {
    if (!statics_ || !world_) return -1;
    Call c(statics_, "GetTimeSeconds");
    c.set<UObject*>("WorldContextObject", world_);
    FProperty* rv = c.valid() ? FindProperty(c.function(), "ReturnValue") : nullptr;
    if (!rv || !c.invoke()) return -1;
    if (PropertySize(rv) == int32_t(sizeof(double))) return c.get<double>("ReturnValue");
    if (PropertySize(rv) == int32_t(sizeof(float))) return double(c.get<float>("ReturnValue"));
    return -1;
}

std::vector<FVector> Adapter::SpawnCandidates() {
    std::vector<FVector> out;
    for (UObject* a : FindInstances(kSpawnerClass)) {
        if (IsGarbage(a) || BeingDestroyed(a)) continue;
        const FVector v = ActorLocation(a);
        if (std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z) > 1.0) out.push_back(v);   // (0,0,0): destroyed, its root gone
    }
    return out;
}

std::vector<Adapter::ActorBox> Adapter::BridgeBoards() {
    std::vector<ActorBox> out;
    for (UObject* a : FindInstances(kBridgeBoardClass)) {
        if (IsGarbage(a) || BeingDestroyed(a)) continue;
        Call c(a, "GetActorBounds");
        c.set<bool>("bOnlyCollidingComponents", false).set<bool>("bIncludeFromChildActors", false);
        if (!c.invoke()) continue;
        out.push_back({c.get<FVector>("Origin"), c.get<FVector>("BoxExtent")});
    }
    return out;
}

// An actor of the loaded level a fallback may read: not being destroyed, a finite place (a destroyed one reads (0,0,0)).
static bool LevelActorAt(UObject* a, FVector& v, Adapter& ad) {
    if (IsGarbage(a) || Adapter::BeingDestroyed(a)) return false;
    v = ad.ActorLocation(a);
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z) > 1.0;
}

// role=server spawn anchor fallbacks (hsm_spawn_anchor.h): the level's PlayerStart actors.
std::vector<FVector> Adapter::PlayerStarts() {
    std::vector<FVector> out;
    for (UObject* a : FindInstances("PlayerStart")) {
        FVector v;
        if (LevelActorAt(a, v, *this)) out.push_back(v);
    }
    return out;
}

// A point on the navmesh: the level's origin projected onto it, else the middle of each NavMeshBoundsVolume (the nearest to
// the origin first) projected. The query box is wide: a map need not have its walkable floor at the origin.
bool Adapter::NavigablePoint(FVector& out) {
    UObject* nav = NavigationSystem();
    if (!nav || !world_) return false;
    std::vector<FVector> aims{FVector{}};
    for (UObject* a : FindInstances("NavMeshBoundsVolume")) {
        FVector v;
        if (LevelActorAt(a, v, *this)) aims.push_back(v);
    }
    std::sort(aims.begin() + 1, aims.end(), [](const FVector& l, const FVector& r) { return std::hypot(l.x, l.y) < std::hypot(r.x, r.y); });
    if (aims.size() > 9) aims.resize(9);
    for (const FVector& aim : aims) {
        Call c(nav, "K2_ProjectPointToNavigation");
        c.set<UObject*>("WorldContextObject", world_).set("Point", aim).set("QueryExtent", FVector{5000, 5000, 10000})
         .set<UObject*>("NavData", nullptr).set<UObject*>("FilterClass", nullptr);
        if (!c.invoke() || !c.get<bool>("ReturnValue")) continue;
        const FVector v = c.get<FVector>("ProjectedLocation");
        if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) { out = v; return true; }
    }
    return false;
}

// The last resort: the floor a downward trace finds under one of the level's static meshes, those nearest the origin first.
bool Adapter::StaticMeshFloor(const std::vector<UObject*>& ignore, FVector& out) {
    std::vector<FVector> at;
    for (UObject* a : FindInstances("StaticMeshActor")) {
        FVector v;
        if (LevelActorAt(a, v, *this)) at.push_back(v);
    }
    std::sort(at.begin(), at.end(), [](const FVector& l, const FVector& r) { return std::hypot(l.x, l.y) < std::hypot(r.x, r.y); });
    if (at.size() > 48) at.resize(48);
    for (const FVector& v : at)
        for (double lift : {0.0, 500.0}) {   // a floor slab's origin is at, or up to a few metres under, its top
            double z;
            if (FloorZ(FVector{v.x, v.y, v.z + lift}, ignore, z)) { out = FVector{v.x, v.y, z}; return true; }
        }
    return false;
}

void Adapter::Teleport(UObject* actor, const FVector& to) {
    Call c(actor, "K2_SetActorLocation");
    c.set("NewLocation", to).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
}

bool Adapter::BindInputs(UObject* willie) {
    if (!bindings_.empty()) return true;
    UClass* cls = ClassOf(willie);
    for (size_t i = 0; i < kAxisCount; ++i) {
        auto fns = FunctionsWithPrefix(cls, std::string("InpAxisEvt_") + kAxisNames[i] + "_K2Node");
        if (fns.empty()) { HSM_LOG("adapter: no axis event for %s", kAxisNames[i]); continue; }
        axisFn_[i] = fns[0];
        bindings_[fns[0]] = {true, i, false};
    }
    for (size_t i = 0; i < kActionCount; ++i) {
        auto fns = FunctionsWithPrefix(cls, std::string("InpActEvt_") + kActionNames[i] + "_K2Node");
        if (fns.empty()) continue;   // e.g. Guard Left/Right are mapped in config but unused by Willie
        // Each action node compiles to a pressed event and (optionally) a released event with a higher node index.
        std::sort(fns.begin(), fns.end(), [](UFunction* a, UFunction* b) {
            return TrailingNumber(ObjectName(reinterpret_cast<UObject*>(a))) < TrailingNumber(ObjectName(reinterpret_cast<UObject*>(b))); });
        pressFn_[i] = fns[0]; bindings_[fns[0]] = {false, i, true};
        if (fns.size() > 1) { releaseFn_[i] = fns[1]; bindings_[fns[1]] = {false, i, false}; }
    }
    HSM_LOG("adapter: bound %zu input events", bindings_.size());
    return !bindings_.empty();
}

float Adapter::ReadAxisParam(UFunction* fn, void* params) {
    FProperty* p = FindProperty(fn, "AxisValue");
    if (!p || !params) return 0.f;
    float v; std::memcpy(&v, static_cast<uint8_t*>(params) + PropertyOffset(p), sizeof v);
    return std::isfinite(v) ? v : 0.f;
}

void Adapter::ApplyAxis(UObject* willie, Axis a, float value) {
    if (UFunction* fn = axisFn_[size_t(a)]) Call(willie, fn).set<float>("AxisValue", value).invoke();
}

void Adapter::FireAction(UObject* willie, Action a, bool pressed) {
    UFunction* fn = pressed ? pressFn_[size_t(a)] : releaseFn_[size_t(a)];
    if (fn) Call(willie, fn).invoke();   // FKey parameter left zeroed; Willie's handlers do not read it
}

UObject* Adapter::MeshOf(UObject* willie) {
    UObject** mesh = Field<UObject*>(willie, "Mesh");
    return mesh ? *mesh : nullptr;
}

bool Adapter::ResolveBones(UObject* mesh) {
    if (bonesResolved_) return true;
    for (const char* b : kBoneCandidates) {
        FName n = MakeName(b);
        Call c(mesh, "GetBoneIndex");
        c.setName("BoneName", n).invoke();
        if (c.get<int32_t>("ReturnValue") >= 0) { bones_.push_back(n); boneNames_.push_back(b); }
    }
    bonesResolved_ = true;
    HSM_LOG("adapter: %zu/%zu body bones present", bones_.size(), sizeof kBoneCandidates / sizeof kBoneCandidates[0]);
    return !bones_.empty();
}

bool Adapter::ReadState(UObject* willie, uint8_t entity, WillieState& out) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || !ResolveBones(mesh)) return false;
    out = WillieState{}; out.entity = entity;
    bool b = false;
    if (GetBool(willie, "DED", b) && b) out.flags |= 1;
    if (GetBool(willie, "Fallen", b) && b) out.flags |= 2;
    if (GetBool(willie, "Headless", b) && b) out.flags |= 4;
    auto d = [&](const char* name) { double* p = Field<double>(willie, name); return p ? float(*p) : 0.f; };
    out.health = d("Health"); out.consciousness = d("Consciousness"); out.stamina = d("Stamina"); out.bleeding = d("Bleeding");
    for (size_t i = 0; i < 7; ++i) out.partHealth[i] = d(kPartHealth[i]);
    out.bodies.reserve(bones_.size());
    for (const FName& bone : bones_) {
        Call c(mesh, "GetSocketTransform");
        c.setName("InSocketName", bone).set<uint8_t>("TransformSpace", 0).invoke();   // RTS_World
        FTransform t = c.get<FTransform>("ReturnValue");
        BodyState s; s.px = float(t.translation.x); s.py = float(t.translation.y); s.pz = float(t.translation.z);
        s.rot = PackQuat(float(t.rotation.x), float(t.rotation.y), float(t.rotation.z), float(t.rotation.w));
        out.bodies.push_back(s);
    }
    return true;
}

bool Adapter::ReadVitals(UObject* willie, uint8_t entity, VitalsMsg& out) {
    double* health = willie ? Field<double>(willie, "Health") : nullptr;
    if (!health) return false;
    out = VitalsMsg{}; out.entity = entity;
    auto d = [&](const char* name) { double* p = Field<double>(willie, name); return p && std::isfinite(*p) ? float(*p) : 0.f; };
    out.health = d("Health"); out.consciousness = d("Consciousness"); out.stamina = d("Stamina"); out.bleeding = d("Bleeding");
    for (size_t i = 0; i < 7; ++i) out.partHealth[i] = d(kPartHealth[i]);
    bool b = false;
    if (GetBool(willie, "DED", b) && b) out.flags |= 1;
    if (GetBool(willie, "Fallen", b) && b) out.flags |= 2;
    if (Field<double>(willie, "Consciousness") && out.consciousness <= 0.f) out.flags |= 4;
    out.severed = ReadSeveredMask(willie);
    // Bit per dislocatable joint, in sync/hsm_gore.h's order (Willie_BP.cpp:1250-1255): the stand-ins free that joint.
    constexpr const char* kDislocatedBoolFields[] = { "Arm R Dislocated", "Arm L Dislocated", "Leg R Dislocated",
                                                      "Leg L Dislocated", "Neck Dislocated", "Spine Dislocated" };
    static_assert(std::size(kDislocatedBoolFields) == sync::kGoreJoints, "one name per gore joint bit");
    for (uint32_t j = 0; j < sync::kGoreJoints; ++j)
        if (GetBool(willie, kDislocatedBoolFields[j], b) && b) out.dislocated |= 1u << j;
    // What other fighters' code reads on this fighter (sync audit items 1, 9, 23, 30; Willie_BP.cpp:400-1310).
    for (size_t i = 0; i < std::size(kStateBoolFields); ++i)
        if (GetBool(willie, kStateBoolFields[i], b) && b) out.state |= uint8_t(1u << i);
    out.weaponSkill = d("Weapon Skill (Temp)"); out.tonus = d("All Body Tonus"); out.noise = d("Total Noise");
    out.light = d("In Light Meter"); out.exhaustion = d("Exhaustion");
    if (FVector* md = Field<FVector>(willie, "Movement Dislocation")) {
        const double m = std::sqrt(md->x * md->x + md->y * md->y + md->z * md->z);
        out.moveDislocation = std::isfinite(m) ? float(m) : 0.f;
    }
    return true;
}

// The owner's state on its stand-in, for the game code that reads another fighter: the attacker's parry roll
// (ModularWeaponBP:918 reads Weapon Skill, Fallen and Any_Guarding), bots' target choice (Fallen, Downed, Give Up), and
// their sight and hearing (noise, light, movement, crouch). The stand-in's tick is blocked, so nothing overwrites them.
// All Body Tonus is left out: the rig zeroes it every frame, and only the parry roll needs it (WriteParryState).
void Adapter::ApplyStandInVitals(UObject* willie, const VitalsMsg& v) {
    auto put = [&](const char* name, float x) { if (double* p = Field<double>(willie, name)) *p = x; };
    put("Health", v.health); put("Consciousness", v.consciousness); put("Stamina", v.stamina); put("Bleeding", v.bleeding);
    for (size_t i = 0; i < 7; ++i) put(kPartHealth[i], v.partHealth[i]);
    put("Weapon Skill (Temp)", v.weaponSkill); put("Total Noise", v.noise); put("In Light Meter", v.light);
    put("Exhaustion", v.exhaustion);
    if (FVector* md = Field<FVector>(willie, "Movement Dislocation")) *md = FVector{ v.moveDislocation, 0, 0 };   // read as VSize
    SetBool(willie, "Fallen", (v.flags & 2u) != 0);
    for (size_t i = 0; i < std::size(kStateBoolFields); ++i) SetBool(willie, kStateBoolFields[i], (v.state >> i) & 1u);
}

// The owner's tonus as the attacker's parry roll is about to read it (ModularWeaponBP:918: Consciousness + All Body
// Tonus >= 150; guard, skill and Fallen are already on the stand-in). The rig zeroes it again on the next frame.
void Adapter::WriteParryTonus(UObject* willie, const VitalsMsg& v) {
    if (double* p = Field<double>(willie, "All Body Tonus")) *p = v.tonus;
}

// Willie_BP Get_Up_Pal (W:4718), as a teammate's grab runs it on its pal: bleeding stopped, health at least 20, give-up
// cleared, and a bot's surrender and retreat reset. Being Grabbed is GrabSync's (the hold is still on).
void Adapter::Revive(UObject* willie) {
    if (double* p = Field<double>(willie, "Bleeding")) *p = 0;
    if (double* p = Field<double>(willie, "Health"); p && *p < 20) *p = 20;
    SetBool(willie, "Give Up", false);
    SetBool(willie, "Give Up 2 (Temp)", false);
    if (UObject* ai = ControllerOf(willie); ai && ObjectName(reinterpret_cast<UObject*>(ClassOf(ai))).rfind("AI_BP", 0) == 0) {
        SetBool(ai, "My Give Up", false);
        SetBool(ai, "Retreat", false);
        if (double* p = Field<double>(ai, "Give Up Meter")) *p = 0;
        if (double* p = Field<double>(ai, "Retreat Intent")) *p = 0;
    }
}

// Pushes are solver contact in the game (Get_Damage applies no impulse): the one the sender's stand-in took, on my body.
bool Adapter::Push(UObject* willie, const std::string& bone, const FVector& impulse) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || bone.empty()) return false;
    return Call(mesh, "AddImpulse").set<FVector>("Impulse", impulse).setName("BoneName", MakeName(bone)).set<bool>("bVelChange", false).invoke();
}

void Adapter::DriveTowards(UObject* willie, const WillieState& target, float dt) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || !ResolveBones(mesh) || dt <= 0.f) return;
    size_t n = std::min(bones_.size(), target.bodies.size());
    for (size_t i = 0; i < n; ++i) {
        Call get(mesh, "GetSocketTransform");
        get.setName("InSocketName", bones_[i]).set<uint8_t>("TransformSpace", 0).invoke();
        FTransform cur = get.get<FTransform>("ReturnValue");
        const BodyState& tb = target.bodies[i];
        FVector lin{ Clamp((tb.px - cur.translation.x) / kTau, kMaxLinear), Clamp((tb.py - cur.translation.y) / kTau, kMaxLinear),
                     Clamp((tb.pz - cur.translation.z) / kTau, kMaxLinear) };
        float qx, qy, qz, qw; UnpackQuat(tb.rot, qx, qy, qz, qw);
        FQuat err = Mul(FQuat{qx, qy, qz, qw}, Conj(cur.rotation));
        if (err.w < 0) err = { -err.x, -err.y, -err.z, -err.w };
        double s = std::sqrt(err.x * err.x + err.y * err.y + err.z * err.z);
        FVector ang{};
        if (s > 1e-6) {
            double angleDeg = 2.0 * std::atan2(s, err.w) * 57.29577951308232;
            double k = angleDeg / kTau / s;
            ang = { Clamp(err.x * k, kMaxAngularDeg), Clamp(err.y * k, kMaxAngularDeg), Clamp(err.z * k, kMaxAngularDeg) };
        }
        Call(mesh, "SetPhysicsLinearVelocity").set("NewVel", lin).set<bool>("bAddToCurrent", false).setName("BoneName", bones_[i]).invoke();
        Call(mesh, "SetPhysicsAngularVelocityInDegrees").set("NewAngVel", ang).set<bool>("bAddToCurrent", false).setName("BoneName", bones_[i]).invoke();
    }
}

// Each bone's velocity moves `blend` of the way toward the one DriveTowards would set; angular motion is left to the
// local simulation, so an input-simulated fighter is pulled back gradually instead of snapping.
void Adapter::NudgeTowards(UObject* willie, const WillieState& target, float blend) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || !ResolveBones(mesh)) return;
    size_t n = std::min(bones_.size(), target.bodies.size());
    for (size_t i = 0; i < n; ++i) {
        Call get(mesh, "GetSocketTransform");
        get.setName("InSocketName", bones_[i]).set<uint8_t>("TransformSpace", 0).invoke();
        FTransform cur = get.get<FTransform>("ReturnValue");
        Call vel(mesh, "GetPhysicsLinearVelocity");
        vel.setName("BoneName", bones_[i]).invoke();
        FVector v = vel.get<FVector>("ReturnValue");
        const BodyState& tb = target.bodies[i];
        FVector want{ Clamp((tb.px - cur.translation.x) / kTau, kMaxLinear), Clamp((tb.py - cur.translation.y) / kTau, kMaxLinear),
                      Clamp((tb.pz - cur.translation.z) / kTau, kMaxLinear) };
        FVector out{ v.x + (want.x - v.x) * blend, v.y + (want.y - v.y) * blend, v.z + (want.z - v.z) * blend };
        Call(mesh, "SetPhysicsLinearVelocity").set("NewVel", out).set<bool>("bAddToCurrent", false).setName("BoneName", bones_[i]).invoke();
    }
}

bool Adapter::FirstBodyPosition(UObject* willie, FVector& out) {
    UObject* mesh = MeshOf(willie);
    if (!mesh || !ResolveBones(mesh) || bones_.empty()) return false;
    Call get(mesh, "GetSocketTransform");
    get.setName("InSocketName", bones_[0]).set<uint8_t>("TransformSpace", 0);
    if (!get.invoke()) return false;
    out = get.get<FTransform>("ReturnValue").translation;
    return true;
}

void Adapter::ApplyVitals(UObject* willie, const WillieState& s) {
    auto put = [&](const char* name, float v) { if (double* p = Field<double>(willie, name)) *p = v; };
    put("Health", s.health); put("Consciousness", s.consciousness); put("Stamina", s.stamina); put("Bleeding", s.bleeding);
    for (size_t i = 0; i < 7; ++i) put(kPartHealth[i], s.partHealth[i]);
}

void Adapter::TriggerDeath(UObject* willie) { Call(willie, "Death").invoke(); }

}  // namespace hsm::game

namespace hsm::game {
namespace {
constexpr const char* kWeaponClass = "ModularWeaponBP_C";
constexpr const char* kBodyWeaponClasses[] = {"Weapon_Fists_C", "Weapon_Feet_C"};   // Willie_BP.cpp:18811, 18941
constexpr const char* kArmorClass = "BP_Armor_Master_C";
constexpr const char* kArmorStruct = "Str_Passport_Armor1";
constexpr const char* kStockScreens[] = {"UI_DED_C", "UI_DeathDoor_C", "UI_WinScreen_C", "UI_WIN_C"};
// Owner sync only: these pause the game (UI_Lose, UI_GiveUp, UI_NextFIght all call SetGamePaused(true)), which stops
// that player's stream, and offer "return to tavern / another fight". Host sync keeps its old list.
constexpr const char* kOwnerMatchScreens[] = {"UI_Lose_C", "UI_GiveUp_C", "UI_NextFIght_C"};
bool g_suppressScreens = false;
bool g_suppressOwnerScreens = false;
bool g_screenHooks = false;
std::vector<std::pair<UObject*, int32_t>> g_suppressed;

// A pause menu other than `self` is in the viewport (see PauseMenuOpen for the class match).
bool OtherPauseMenuOnScreen(UObject* self) {
    UClass* uw = FindClass("UserWidget");
    for (int32_t i = 0, n = ObjectCount(); i < n && uw; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || o == self || IsDefaultObject(o) || !IsA(o, uw)) continue;
        if (ObjectName(reinterpret_cast<UObject*>(ClassOf(o))).find("UI_Pause") == std::string::npos) continue;
        Call v(o, "IsInViewport");
        if (v.invoke() && v.get<bool>("ReturnValue")) return true;
    }
    return false;
}   // widgets whose Construct was skipped; removed next tick
bool ClassNamed(UObject* o, const char* name) { return o && SameNameAscii(ObjectName(reinterpret_cast<UObject*>(ClassOf(o))), name); }
}  // namespace

bool Adapter::IsWillie(UObject* o) {
    if (!willieClass_) willieClass_ = FindClass(kWillieClass);
    return o && willieClass_ && IsA(o, willieClass_);
}

bool Adapter::FreeModeActive() { bool f = true; UObject* gi = GameInstance(); if (gi) GetBool(gi, "Free Mode Activated", f); return f; }

// The stock Free->Progression switch (the Fencer dialog's mode button) sets Free Mode off, Play Mode 0 and Crash Open
// off, and the Tavern hub's own reset sets Game Mode 0, Combat Mode 1 and Lose Condition 0. Clearing Free Mode alone
// left Play Mode at 1 after a lobby fight: the hub hid its mercenaries, merchant, smiths and sleep and the whole
// Progression flow ran as Free Mode (#616). The same fields the stock entry sets, in the stock entry's spirit.
void Adapter::ClearFreeMode() {
    if (UObject* gi = GameInstance()) {
        SetBool(gi, "Free Mode Activated", false);
        if (uint8_t* p = Field<uint8_t>(gi, "Current Play Mode")) *p = 0;
        if (uint8_t* m = Field<uint8_t>(gi, "Current Game Mode enum")) *m = 0;
        if (uint8_t* cm = Field<uint8_t>(gi, "Current Combat Mode")) *cm = 1;
        if (uint8_t* lc = Field<uint8_t>(gi, "Current Lose Condition")) *lc = 0;
        SetBool(gi, "Crash Open", false);
        HSM_LOG("adapter: Progression entry reset (Play Mode 0, Game Mode 0, Combat Mode 1, Lose Condition 0, Crash Open off)");
    }
}

void Adapter::PrepareFreeModeFight(int foes) {
    UObject* gi = GameInstance();
    if (!gi) return;
    SetBool(gi, "Free Mode Activated", true);
    if (foes >= 0) if (int32_t* f = Field<int32_t>(gi, "Free Mode Foes Amount")) *f = foes;
    bool carnage = false;   // the level manager's own Free Mode combat mode (decomp BP_LevelManager.cpp:67)
    GetBool(gi, "Free Mode Carnage", carnage);
    if (uint8_t* cm = Field<uint8_t>(gi, "Current Combat Mode")) *cm = carnage ? 3 : 1;
    if (uint8_t* p = Field<uint8_t>(gi, "Current Play Mode")) *p = 1;
    if (uint8_t* m = Field<uint8_t>(gi, "Current Game Mode enum")) *m = 1;
}

// ---------------------------------------------------------------- combat scenarios (match/hsm_scenarios.h)
namespace {
bool ReadableMem(const void* p, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    const char* c = static_cast<const char*>(p);
    const char* end = c + len;
    while (c < end) {
        if (!VirtualQuery(c, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        c = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}
constexpr size_t kArrayInner = 0x70;     // FArrayProperty::Inner follows FProperty (0x70 bytes in this build; mod_maps.cpp)
struct ScriptArray { const uint8_t* data; int32_t num, max; };
// A TArray property of one-byte elements (an enum or byte array) as "1,3"; "?" when it is not what it should be.
std::string ByteArrayText(UObject* o, const char* name) {
    FProperty* p = FindProperty(ClassOf(o), name);
    if (!p) return "none";
    if (PropertyType(p) != "ArrayProperty" || PropertySize(p) != int32_t(sizeof(ScriptArray))) return "?";
    const void* const* slot = reinterpret_cast<const void* const*>(reinterpret_cast<const char*>(p) + kArrayInner);
    if (!ReadableMem(slot, sizeof(void*)) || !*slot || !ReadableMem(*slot, 0x80)) return "?";
    const FProperty* inner = static_cast<const FProperty*>(*slot);
    const std::string innerType = PropertyType(inner);
    if (PropertySize(inner) != 1 || (innerType != "ByteProperty" && innerType != "EnumProperty")) return "?";
    const ScriptArray* a = reinterpret_cast<const ScriptArray*>(reinterpret_cast<const uint8_t*>(o) + PropertyOffset(p));
    if (!ReadableMem(a, sizeof *a) || a->num < 0 || a->num > 64 || (a->num && (!a->data || !ReadableMem(a->data, size_t(a->num))))) return "?";
    std::string out;
    for (int32_t i = 0; i < a->num; ++i) out += (i ? "," : "") + std::to_string(unsigned(a->data[i]));
    return out.empty() ? "none" : out;
}
// An FText living at `t` as UTF-8; "?" when it cannot be read.
std::string TextValue(const void* t) {
    static UObject* lib = nullptr;
    if (!lib) if (UObject* cls = FindObject("KismetTextLibrary", "Class")) lib = DefaultObject(reinterpret_cast<UClass*>(cls));
    if (!lib || !ReadableMem(t, sizeof(FText))) return "?";
    Call str(lib, "Conv_TextToString");
    str.set("InText", *static_cast<const FText*>(t));
    if (!str.invoke()) return "?";
    return ReadFString(str.paramPtr("ReturnValue"));
}
std::string TextProp(UObject* o, const char* name) {
    FProperty* p = FindProperty(ClassOf(o), name);
    if (!p || PropertyType(p) != "TextProperty" || PropertySize(p) != int32_t(sizeof(FText))) return "?";
    return TextValue(reinterpret_cast<const uint8_t*>(o) + PropertyOffset(p));
}
constexpr const char* kCombatEventClasses[] = {"BP_CombatEvent_Alley_C", "BP_CombatEvent_Cellar_C", "BP_CombatEvent_Forest_C", "BP_CombatEvent_Hall_C",
                                               "BP_CombatEvent_Pit_C", "BP_CombatEvent_Slums_C", "BP_CombatEvent_Yard_C", "BP_CombatEvent_Tourney_C",
                                               "BP_CombatEvent_TourneyBaron_C"};
}  // namespace

bool Adapter::ApplyScenario(const hsm::ScenarioWorld& w) {
    UObject* gi = GameInstance();
    if (!gi) return false;
    if (!scenarioSaved_.saved) {                                   // the host's own Free Mode values, kept once
        scenarioSaved_.saved = true;
        GetBool(gi, "Free Mode Carnage", scenarioSaved_.carnage);
        GetBool(gi, "Free Mode Brawling", scenarioSaved_.brawling);
        if (uint8_t* l = Field<uint8_t>(gi, "Current Lose Condition")) { scenarioSaved_.hasLose = true; scenarioSaved_.lose = *l; }
    }
    bool any = false;
    if (w.carnage) any |= SetBool(gi, "Free Mode Carnage", true);
    if (w.brawling) any |= SetBool(gi, "Free Mode Brawling", true);
    if (w.loseCondition >= 0) if (uint8_t* l = Field<uint8_t>(gi, "Current Lose Condition")) { *l = uint8_t(w.loseCondition); any = true; }
    return any;
}

void Adapter::RestoreScenario() {
    if (!scenarioSaved_.saved) return;
    if (UObject* gi = GameInstance()) {
        SetBool(gi, "Free Mode Carnage", scenarioSaved_.carnage);
        SetBool(gi, "Free Mode Brawling", scenarioSaved_.brawling);
        if (scenarioSaved_.hasLose) if (uint8_t* l = Field<uint8_t>(gi, "Current Lose Condition")) *l = scenarioSaved_.lose;
    }
    scenarioSaved_ = ScenarioSaved{};
}

// The fence of the Fenced-ring scenario (#706): BP_LevelManager_C::Clean_Up_and_Spawn_Actors resets "Current Lose
// Condition" on every level load (Free Mode on) and destroys the BP_Fence_Bags unless it is 3 at that moment (decomp
// BP_LevelManager.cpp:66-91). Session's post hook on the level manager's Spawn Combatants calls this right after that
// wipe, ahead of the fence check in the same pass.
bool Adapter::KeepFenceLoseCondition() {
    if (UObject* gi = GameInstance())
        if (uint8_t* l = Field<uint8_t>(gi, "Current Lose Condition")) { *l = uint8_t(hsm::kLoseConditionFence); return true; }
    return false;
}

UObject* Adapter::SpawnFenceBags(const FVector& at, double radius) {
    UClass* cls = FindClass("BP_Fence_Bags_C");
    if (!cls || !statics_ || !world_) return nullptr;
    FTransform t; t.translation = at;
    Call begin(statics_, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", world_).set<UClass*>("ActorClass", cls).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 1).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* a = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!a) return nullptr;
    if (float* r = Field<float>(a, "Radius")) *r = float(radius);   // a Float property (check_reflection)
    Call finish(statics_, "FinishSpawningActor");
    finish.set<UObject*>("Actor", a).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    return finish.invoke() ? finish.get<UObject*>("ReturnValue") : nullptr;
}

bool Adapter::IsZombieBody(UObject* w) {
    return w && ObjectName(reinterpret_cast<UObject*>(ClassOf(w))) == kZombieClass;
}

bool Adapter::FenceBagsExist() { return !FindInstances("BP_Fence_Bags_C").empty(); }

int Adapter::LogCombatEvents(const std::string& map) {
    int lines = 0;
    for (const char* cls : kCombatEventClasses) {
        std::vector<UObject*> objs = FindInstances(cls);
        bool isDefault = false;
        if (objs.empty()) {                                        // none placed in this level: the class defaults say what it offers
            UClass* c = FindClass(cls);
            if (!c) continue;
            if (UObject* d = DefaultObject(c)) { objs.push_back(d); isDefault = true; }
        }
        for (UObject* o : objs) {
            if (!o) continue;
            HSM_LOG("scenario: map %s: combat event %s \"%s\" modes %s lose conditions %s%s", map.c_str(), cls, TextProp(o, "Scenario Name").c_str(),
                    ByteArrayText(o, "Available Combat Modes").c_str(), ByteArrayText(o, "Available Lose Conditions").c_str(), isDefault ? " (class defaults)" : "");
            ++lines;
        }
    }
    return lines;
}

// ---------------------------------------------------------------- Progression: the next fights the hub offers
namespace {
constexpr size_t kMaxOffered = 16;
constexpr const char* kGameManagerClass = "BP_GameManager_C";   // the level's own manager; its Generate Combat Events refills the list
// The career's "Available Combat Events" (Str_Passport_CombatEvent1 elements): on the save object or the game instance.
// The mod's own temporary save objects hold an empty list, so the longest list found is the career's.
struct EventList { UObject* owner = nullptr; const uint8_t* data = nullptr; int32_t num = 0, stride = 0; };
EventList FindEventList(UObject* gi) {
    EventList best;
    std::vector<UObject*> holders = FindInstances(kSaveGameClass);
    if (gi) holders.push_back(gi);
    for (UObject* o : holders) {
        FProperty* p = o ? FindProperty(ClassOf(o), "Available Combat Events") : nullptr;
        if (!p || PropertyType(p) != "ArrayProperty" || PropertySize(p) != int32_t(sizeof(ScriptArray))) continue;
        const void* const* slot = reinterpret_cast<const void* const*>(reinterpret_cast<const char*>(p) + kArrayInner);
        if (!ReadableMem(slot, sizeof(void*)) || !*slot || !ReadableMem(*slot, 0x80)) continue;
        const FProperty* inner = static_cast<const FProperty*>(*slot);
        const int32_t stride = PropertySize(inner);
        if (PropertyType(inner) != "StructProperty" || stride <= 0 || stride > 4096) continue;
        const ScriptArray* a = reinterpret_cast<const ScriptArray*>(reinterpret_cast<const uint8_t*>(o) + PropertyOffset(p));
        if (!ReadableMem(a, sizeof *a) || a->num <= 0 || a->num > int32_t(kMaxOffered) || !a->data || !ReadableMem(a->data, size_t(a->num) * size_t(stride))) continue;
        if (a->num > best.num) best = {o, a->data, a->num, stride};
    }
    return best;
}
// Why one holder's "Available Combat Events" reads as it does (#621): the first of FindEventList's checks it fails, or
// the count it holds. Same checks, same order, so the line names the exact reason the list came back empty.
std::string EventListWhy(UObject* o) {
    FProperty* p = o ? FindProperty(ClassOf(o), "Available Combat Events") : nullptr;
    if (!p) return "property missing";
    if (PropertyType(p) != "ArrayProperty" || PropertySize(p) != int32_t(sizeof(ScriptArray))) return "not an array";
    const void* const* slot = reinterpret_cast<const void* const*>(reinterpret_cast<const char*>(p) + kArrayInner);
    if (!ReadableMem(slot, sizeof(void*)) || !*slot || !ReadableMem(*slot, 0x80)) return "inner property unreadable";
    if (PropertyType(static_cast<const FProperty*>(*slot)) != "StructProperty") return "inner not a struct";
    const ScriptArray* a = reinterpret_cast<const ScriptArray*>(reinterpret_cast<const uint8_t*>(o) + PropertyOffset(p));
    if (!ReadableMem(a, sizeof *a)) return "array unreadable";
    return "num=" + std::to_string(a->num);
}
// A GI integer as text, whatever width this build gives it (career money is Kismet Int64 here, Int in the decompiled
// index): 8 and 4 bytes are read, another size says so. "?" when the property is missing.
std::string GiIntText(const UObject* gi, const FProperty* p) {
    if (!gi || !p) return "?";
    const uint8_t* at = reinterpret_cast<const uint8_t*>(gi) + PropertyOffset(p);
    if (PropertySize(p) == 8) { int64_t v = 0; std::memcpy(&v, at, sizeof v); return std::to_string(v); }
    if (PropertySize(p) == 4) { int32_t v = 0; std::memcpy(&v, at, sizeof v); return std::to_string(v); }
    return std::to_string(PropertySize(p)) + " bytes";
}
// The fight list's state, one line per holder (the save objects and the game instance) plus the career's money state and
// whether the level's own game manager is there to refill it (#621). The host's first read of a hub visit only.
void LogEventHolders(UObject* gi) {
    std::vector<UObject*> holders = FindInstances(kSaveGameClass);
    if (gi) holders.push_back(gi);
    for (UObject* o : holders)
        if (o) HSM_LOG("progression: %s: Available Combat Events: %s", ObjectName(o).c_str(), EventListWhy(o).c_str());
    if (!gi) return;
    const uint8_t* tier = Field<uint8_t>(gi, "Player Tier");
    const std::string tierText = tier ? std::to_string(*tier) : "?";
    HSM_LOG("progression: GI: Player Funds %s, Player Tier %s, BP_GameManager_C %s", GiIntText(gi, FindProperty(ClassOf(gi), "Player Funds")).c_str(),
            tierText.c_str(), FindInstances(kGameManagerClass).empty() ? "missing" : "present");
}
// A text-like member of the event struct, as a name: the Scenario Name if it has one, else the first text, string or name member.
std::string EventName(const uint8_t* element, const std::vector<FProperty*>& members) {
    const FProperty* pick = nullptr;
    for (const FProperty* m : members) {
        const std::string t = PropertyType(m);
        if (t != "TextProperty" && t != "StrProperty" && t != "NameProperty") continue;
        if (!pick) pick = m;
        if (PropertyName(m).find("Scenario") != std::string::npos) { pick = m; break; }
    }
    if (!pick) return {};
    const uint8_t* at = element + PropertyOffset(pick);
    const std::string t = PropertyType(pick);
    if (t == "TextProperty") return PropertySize(pick) == int32_t(sizeof(FText)) ? TextValue(at) : std::string();
    if (t == "NameProperty") return ReadableMem(at, sizeof(FName)) ? NameToString(*reinterpret_cast<const FName*>(at)) : std::string();
    return ReadableMem(at, 16) ? ReadFString(at) : std::string();
}
}  // namespace

int Adapter::CareerArrayNum(const char* field) {
    UObject* gi = GameInstance();
    FProperty* p = gi ? FindProperty(ClassOf(gi), field) : nullptr;
    if (!p || PropertyType(p) != "ArrayProperty" || PropertySize(p) != int32_t(sizeof(ScriptArray))) return -1;
    const ScriptArray* a = reinterpret_cast<const ScriptArray*>(reinterpret_cast<const uint8_t*>(gi) + PropertyOffset(p));
    return ReadableMem(a, sizeof *a) && a->num >= 0 ? a->num : -1;
}

bool Adapter::GenerateTourneyChampion(int tier) {
    const std::vector<UObject*> managers = FindInstances(kGameManagerClass);
    if (managers.empty()) return false;
    Call c(managers.front(), "Generate Random Characters");
    c.set<int32_t>("Amount of Characters to Spawn", 1).setReal("Equipment Budget", hsm::TourneyChampionBudget(tier))
     .set<bool>("Champion", true).set<uint8_t>("Tier", uint8_t(std::clamp(tier, 0, hsm::kTopRank)));
    return c.valid() && c.invoke();
}

std::vector<std::string> Adapter::OfferedFights(bool freshVisit) {
    std::vector<std::string> out;
    UObject* gi = GameInstance();
    if (freshVisit) LogEventHolders(gi);                    // the host's first read this hub visit: why the list reads as it does
    EventList list = FindEventList(gi);
    if (freshVisit && !list.owner) {                        // empty everywhere: the stock regen-if-empty, once per hub visit (#621)
        const std::vector<UObject*> managers = FindInstances(kGameManagerClass);
        if (managers.empty()) HSM_LOG("progression: refill found no game manager");
        else if (Call(managers.front(), "Generate Combat Events").invoke()) {
            list = FindEventList(gi);
            HSM_LOG("progression: offered fights refilled (%d)", list.owner ? list.num : 0);
        } else HSM_LOG("progression: refill: Generate Combat Events could not be called");
    }
    if (!list.owner) return out;
    UObject* st = FindObject("Str_Passport_CombatEvent1", "UserDefinedStruct");
    const std::vector<FProperty*> members = st ? StructProperties(st) : std::vector<FProperty*>{};
    static bool logged = false;
    if (!logged) {                                                  // the spike: what the struct holds, for the next build
        logged = true;
        std::string m;
        for (const FProperty* p : members) m += (m.empty() ? "" : ", ") + PropertyName(p) + ":" + PropertyType(p);
        HSM_LOG("progression: Available Combat Events on %s: %d event(s), element %d bytes; struct members: %s", ObjectName(list.owner).c_str(), list.num, list.stride,
                m.empty() ? "(struct not found)" : m.c_str());
    }
    for (int32_t i = 0; i < list.num; ++i) {
        std::string n = EventName(list.data + size_t(i) * size_t(list.stride), members);
        out.push_back(n.empty() ? "Fight " + std::to_string(i + 1) : n);
    }
    return out;
}

// Starts the offered fight `index` through the game's own hub UI: the function it calls with the chosen combat event
// ("Go To Free Mode Event" on the Blueprint side), found on a live next-fight widget. Only a widget that is on screen is
// called, and only a function whose single input is the event struct; every candidate is logged. False: the caller tells
// the host to start it from the Fencer. `how` says what was called, or why not.
bool Adapter::StartOfferedFight(size_t index, std::string& how) {
    const EventList list = FindEventList(GameInstance());
    if (!list.owner || index >= size_t(list.num)) { how = "the offered list is gone"; return false; }
    const uint8_t* element = list.data + index * size_t(list.stride);
    static const char* const kWidgets[] = {"UI_List_MapSelect_C", "UI_NextFIght_C", "UI_NextFight_C"};
    static const char* const kFunctions[] = {"Go To Free Mode Event", "Go To Event", "Start Combat Event"};
    std::string seen;
    for (const char* wc : kWidgets) {
        for (UObject* w : FindInstances(wc)) {
            Call onScreen(w, "IsInViewport");
            if (!onScreen.invoke() || !onScreen.get<bool>("ReturnValue")) { seen += std::string(seen.empty() ? "" : "; ") + wc + " not on screen"; continue; }
            for (const char* fnName : kFunctions) {
                UFunction* fn = FindFunction(ClassOf(w), fnName);
                if (!fn) continue;
                std::vector<FProperty*> inputs;
                for (FProperty* p : Params(fn)) {
                    const uint64_t f = PropertyFlags(p);
                    if ((f & 0x80) && !(f & 0x400) && !(f & 0x100)) inputs.push_back(p);
                }
                seen += std::string(seen.empty() ? "" : "; ") + wc + "::" + fnName + "(" + std::to_string(inputs.size()) + " in)";
                if (inputs.size() != 1 || PropertyType(inputs[0]) != "StructProperty" || PropertySize(inputs[0]) != list.stride) continue;
                Call c(w, fn);
                c.setFrom(PropertyName(inputs[0]), element);
                if (!c.invoke()) continue;
                how = std::string(wc) + "::" + fnName;
                return true;
            }
        }
    }
    how = seen.empty() ? "no next-fight widget found" : seen;
    return false;
}

bool Adapter::EndErrandMode() {
    UObject* gi = GameInstance();
    uint8_t* m = gi ? Field<uint8_t>(gi, "Current Game Mode enum") : nullptr;
    if (!m || *m != 2) return false;
    *m = 1;
    return true;
}

int Adapter::FreeModeFoes() {
    UObject* gi = GameInstance();
    int32_t* f = gi ? Field<int32_t>(gi, "Free Mode Foes Amount") : nullptr;
    return f ? *f : -1;
}
void Adapter::SetFreeModeFoes(int foes) {
    if (UObject* gi = GameInstance()) if (int32_t* f = Field<int32_t>(gi, "Free Mode Foes Amount")) *f = foes;
}

bool Adapter::GamePaused() {
    Call c(statics_, "IsGamePaused");
    c.set<UObject*>("WorldContextObject", world_);
    return c.invoke() && c.get<bool>("ReturnValue");
}

void Adapter::SetGamePaused(bool paused) {
    Call(statics_, "SetGamePaused").set<UObject*>("WorldContextObject", world_).set<bool>("bPaused", paused).invoke();
}

bool Adapter::ResetTimeDilation() {
    Call g(statics_, "GetGlobalTimeDilation");
    g.set<UObject*>("WorldContextObject", world_);
    if (!g.invoke() || g.get<float>("ReturnValue") == 1.f) return false;
    Call(statics_, "SetGlobalTimeDilation").set<UObject*>("WorldContextObject", world_).set<float>("TimeDilation", 1.f).invoke();
    return true;
}

void Adapter::QuitGame() {
    UObject* ksl = nullptr;
    if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    Call q(ksl, "QuitGame");
    q.set<UObject*>("WorldContextObject", world_).set<UObject*>("SpecificPlayer", nullptr).set<uint8_t>("QuitPreference", 0).set<bool>("bIgnorePlatformRestrictions", true);
    if (!q.invoke()) ExitProcess(0);
}

// A Blueprint class that no loaded asset references yet (the zombie Willie on an arena map, for Infection): loaded by
// its object path through KismetSystemLibrary (MakeSoftClassPath -> Conv_SoftClassPathToSoftClassRef ->
// LoadClassAsset_Blocking). A hitch the first time; nullptr when the path does not resolve.
UClass* Adapter::LoadClassBlocking(const std::string& path) {
    UObject* ksl = nullptr;
    if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    if (!ksl) return nullptr;
    FStringArgHolder str(path);
    Call make(ksl, "MakeSoftClassPath");
    make.set("PathString", str.arg);
    if (!make.valid() || !make.invoke()) return nullptr;
    Call conv(ksl, "Conv_SoftClassPathToSoftClassRef");
    void* softPath = make.paramPtr("ReturnValue");
    if (!conv.valid() || !softPath) return nullptr;
    conv.setFrom("SoftClassPath", softPath);
    if (!conv.invoke()) return nullptr;
    Call load(ksl, "LoadClassAsset_Blocking");
    void* softRef = conv.paramPtr("ReturnValue");
    if (!load.valid() || !softRef) return nullptr;
    load.setFrom("AssetClass", softRef);
    return load.invoke() ? load.get<UClass*>("ReturnValue") : nullptr;
}

void Adapter::ConsoleCommand(const std::string& command) {
    UObject* ksl = nullptr;
    if (UObject* cls = FindObject("KismetSystemLibrary", "Class")) ksl = DefaultObject(reinterpret_cast<UClass*>(cls));
    FStringArgHolder cmd(command);
    Call(ksl, "ExecuteConsoleCommand").set<UObject*>("WorldContextObject", world_).set("Command", cmd.arg).set<UObject*>("SpecificPlayer", nullptr).invoke();
}

bool Adapter::IsDead(UObject* willie) { bool d = false; return GetBool(willie, "DED", d) && d; }

// Every weapon Blueprint derives from ModularWeaponBP_C (re/out/bp/_class_summary.tsv); the Willie's weapon fields are
// typed ModularWeaponBP_C, fists and feet included. Owner sync walks the class chain, comparing names by FName once the
// first match has shown it (FNames live as long as the process; class objects may be reloaded).
bool Adapter::IsWeapon(UObject* actor) {
    if (!subclassWeapons_) return ClassNamed(actor, kWeaponClass);
    for (UClass* c = actor ? ClassOf(actor) : nullptr; c; c = SuperOf(c)) {
        const FName n = ObjectFName(reinterpret_cast<UObject*>(c));
        if (weaponNameKnown_) {
            if (n.index == weaponName_.index && n.number == weaponName_.number) return true;
        } else if (SameNameAscii(NameToString(n), kWeaponClass)) {
            weaponName_ = n;
            weaponNameKnown_ = true;
            return true;
        }
    }
    return false;
}

bool Adapter::IsBodyWeapon(UObject* actor) {
    if (!actor) return false;
    const std::string cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(actor)));
    for (const char* body : kBodyWeaponClasses) if (cls == body) return true;
    return false;
}

UObject* Adapter::FighterOf(UObject* actor) {
    for (int depth = 0; actor && depth < 4; ++depth) {
        if (IsWillie(actor)) return actor;
        if (IsWeapon(actor)) {
            UObject** p = Field<UObject*>(actor, "Parent Actor");
            if (p && *p) return *p;
            UObject** last = Field<UObject*>(actor, "Last Parent");
            return last ? *last : nullptr;              // thrown weapons credit their last holder
        }
        Call c(actor, "GetAttachParentActor");
        actor = c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
    }
    return nullptr;
}

std::vector<UObject*> Adapter::AllWillies() {
    std::vector<UObject*> out;
    // Destroyed fighters stay in the object array until garbage collection; once the destroy is over only their
    // garbage flag tells them apart (the purges destroyed them again every second, test 2).
    // FindInstances matches class names exactly, so the zombie class (a child of Willie_BP_C, no new fields) is
    // scanned on its own: without it the Zombies format's bots are invisible to every scan (#561).
    for (const char* cls : {kWillieClass, kZombieClass})
        for (UObject* w : FindInstances(cls)) if (!destroyAware_ || (!IsGarbage(w) && !BeingDestroyed(w))) out.push_back(w);
    return out;
}

// One pass over the object array; each class is tested once per pass (class objects may be reloaded between passes).
std::vector<UObject*> Adapter::WeaponActors() {
    std::vector<UObject*> out;
    std::unordered_map<UClass*, bool> weaponClass;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o)) continue;
        auto it = weaponClass.find(ClassOf(o));
        if (it == weaponClass.end()) it = weaponClass.emplace(ClassOf(o), IsWeapon(o)).first;
        if (it->second && (!destroyAware_ || !BeingDestroyed(o))) out.push_back(o);
    }
    return out;
}

bool Adapter::IsModActor(UObject* a) {
    if (!a || modActors_.empty()) return false;
    std::erase_if(modActors_, [&](const auto& m) { return !StillValid(m.first, m.second); });   // gone ones leave the list
    for (const auto& m : modActors_) if (m.first == a) return true;
    return false;
}

Adapter::DropClear Adapter::DestroyDroppedWeapons(const std::function<bool(UObject*)>& levelStart) {
    DropClear out;
    for (UObject* w : WeaponActors()) {
        UObject** parent = Field<UObject*>(w, "Parent Actor");
        if (!parent) continue;                                   // not a modular weapon after all
        DropCandidate c;
        c.bodyWeapon = IsBodyWeapon(w);
        c.beingDestroyed = BeingDestroyed(w);
        c.modOwned = IsModActor(w);                              // the CTF flags: respawned by the mod when they are gone, but never cleared as drops
        c.hasParent = *parent != nullptr;
        if (c.hasParent) {
            UObject** right = Field<UObject*>(*parent, "Weapon R");
            UObject** left = Field<UObject*>(*parent, "Weapon L");
            c.inHand = (right && *right == w) || (left && *left == w);
        } else if (!c.bodyWeapon) {
            c.levelStart = levelStart && levelStart(w);
        }
        switch (ClearableDrop(c)) {
            case DropVerdict::Clear: DestroyActor(w); ++out.destroyed; break;
            case DropVerdict::KeepHeld: ++out.held; break;
            case DropVerdict::KeepSheathed: ++out.sheathed; break;
            case DropVerdict::KeepLevel: ++out.level; break;
            case DropVerdict::KeepOther: break;
        }
    }
    return out;
}

std::vector<UObject*> Adapter::LevelWeapons() {
    std::vector<UObject*> out;
    for (UObject* w : WeaponActors()) {
        if (IsBodyWeapon(w) || IsModActor(w)) continue;          // a flag is no level weapon: the map reset must not put it back or respawn it
        UObject** parent = Field<UObject*>(w, "Parent Actor");
        UObject** last = Field<UObject*>(w, "Last Parent");
        if (parent && !*parent && last && !*last) out.push_back(w);
    }
    return out;
}

FRotator Adapter::ActorRotation(UObject* actor) {
    Call c(actor, "K2_GetActorRotation");
    return c.invoke() ? c.get<FRotator>("ReturnValue") : FRotator{};
}

UObject* Adapter::SpawnActorAt(UClass* cls, const FVector& at, const FRotator& rot) {
    if (!cls || !statics_ || !world_) return nullptr;
    FTransform t; t.translation = at;
    Call begin(statics_, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", world_).set<UClass*>("ActorClass", cls).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 1).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* a = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!a) return nullptr;
    Call finish(statics_, "FinishSpawningActor");
    finish.set<UObject*>("Actor", a).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    a = finish.invoke() ? finish.get<UObject*>("ReturnValue") : nullptr;
    if (!a) return nullptr;
    Call move(a, "K2_SetActorLocationAndRotation");
    move.set<FVector>("NewLocation", at).set<FRotator>("NewRotation", rot).set<bool>("bSweep", false).set<bool>("bTeleport", true);
    move.invoke();
    return a;
}

namespace {
// An armour piece's class: BP_Armor_Master_C and its children carry both.
bool ArmorClass(UClass* cls) { return FindProperty(cls, "Armor Passport") && FindProperty(cls, "Armor Slot"); }
}

std::vector<UObject*> Adapter::LooseArmor() {
    std::vector<UObject*> out;
    std::unordered_map<UClass*, bool> armorClass;
    UClass* master = FindClass(kArmorClass);   // a class test by the chain: no property lookups by name over ~80k objects
    if (!master) return out;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o)) continue;
        auto it = armorClass.find(ClassOf(o));
        if (it == armorClass.end()) it = armorClass.emplace(ClassOf(o), IsA(o, master) && ArmorClass(ClassOf(o))).first;
        if (!it->second || (destroyAware_ && BeingDestroyed(o))) continue;
        Call parent(o, "GetAttachParentActor");
        if (parent.invoke() && !parent.get<UObject*>("ReturnValue")) out.push_back(o);
    }
    return out;
}

// ---------------------------------------------------------------- crossbows (experimental; the owner's crossbow test)
// The game's crossbow is a Blueprint ranged weapon (BP_Weapon_Ranged_Weapon_CrossbowLight_C) that fires BP_Weapon_Ranged_Projectle_Bolt1_C
// ("Projectle" is the game's spelling) from a bolt quiver the fighter wears ("Attached Bolt Quiver"). Names are from
// tools/reflection_index.json; the class objects exist only while a loaded asset references them.
namespace {
constexpr const char* kCrossbowClass = "BP_Weapon_Ranged_Weapon_CrossbowLight_C";
constexpr const char* kBoltClass = "BP_Weapon_Ranged_Projectle_Bolt1_C";
constexpr int32_t kCrossbowStartBolts = 8;

std::string Squash(const std::string& s) {      // the index's normal form: lowercase letters and digits only
    std::string o;
    for (char c : s) if (std::isalnum(static_cast<unsigned char>(c))) o.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    return o;
}

// The class by its exact name, else the Blueprint class whose name matches ignoring case and underscores (the game's real
// spelling may differ from the index's normalized one). Null: not loaded.
UClass* FindClassLoose(const char* exact) {
    if (UClass* c = FindClass(exact)) return c;
    const std::string want = Squash(exact);
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || Squash(ObjectName(o)) != want) continue;
        if (ObjectName(reinterpret_cast<UObject*>(ClassOf(o))).find("Class") != std::string::npos) return reinterpret_cast<UClass*>(o);
    }
    return nullptr;
}
}  // namespace

bool Adapter::CrossbowAvailable(bool log) {
    UClass* weapon = FindClassLoose(kCrossbowClass);
    UClass* bolt = FindClassLoose(kBoltClass);
    if (log)
        HSM_LOG("crossbow: class %s %s, bolt class %s %s", weapon ? ObjectName(reinterpret_cast<UObject*>(weapon)).c_str() : kCrossbowClass,
                weapon ? "found" : "missing", bolt ? ObjectName(reinterpret_cast<UObject*>(bolt)).c_str() : kBoltClass, bolt ? "found" : "missing");
    return weapon && bolt;
}

bool Adapter::GiveCrossbow(UObject* willie) {
    UClass* cls = willie ? FindClassLoose(kCrossbowClass) : nullptr;
    if (!cls) { HSM_LOG("crossbow: not given, the class is missing"); return false; }
    // An occupied right hand lets go first, as a fallen bot's loot does (a grip type of 0 would destroy the weapon).
    UObject** held = Field<UObject*>(willie, "Weapon R");
    uint8_t* grip = Field<uint8_t>(willie, "R_GripType_Current");
    if (held && *held && !IsBodyWeapon(*held) && grip && *grip != 0) {
        Call drop(willie, "Set Up Right Hand Weapon");
        drop.set<bool>("Dropped With no Damage", true);
        drop.invoke();
    }
    FVector at = ActorLocation(willie);
    at.z += 60;
    UObject* x = SpawnActorAt(cls, at, ActorRotation(willie));
    if (!x) { HSM_LOG("crossbow: not given, spawning %s failed", kCrossbowClass); return false; }
    Call take(willie, "Set Up Right Hand Weapon");
    take.set<UObject*>("Weapon R", x);
    const bool called = take.invoke();
    UObject** now = Field<UObject*>(willie, "Weapon R");
    const bool inHand = called && now && *now == x;

    // The quiver a reload draws from. "Attach Empty Quiver" spawns a BP_Quiver_Bolt_1_C (Projectile Count 0), attaches
    // it at the Quiver_Slot_L socket and stores it in the Willie's "Attached Bolt Quiver"; the crossbow's reload gates
    // on that actor's "Projectile Count". A fighter that already wears one keeps it, as Willie's own respawn path
    // checks the same field before spawning another.
    UObject** quiver = Field<UObject*>(willie, "Attached Bolt Quiver");
    if (!quiver || !*quiver || IsGarbage(*quiver) || (destroyAware_ && BeingDestroyed(*quiver))) {
        Call(willie, "Attach Empty Quiver").invoke();
        quiver = Field<UObject*>(willie, "Attached Bolt Quiver");
    }
    int32_t bolts = -1;
    if (quiver && *quiver) {
        bolts = kCrossbowStartBolts;   // GI Settings' "Carried Bolt Count" is the save file's setting, 0 without one
        if (UObject* gi = GameInstance())
            if (int32_t* carried = Field<int32_t>(gi, "Carried Bolt Count"))
                if (*carried > 0) bolts = *carried;
        if (int32_t* n = Field<int32_t>(*quiver, "Projectile Count")) *n = bolts;
        Call(*quiver, "Rearrange Projectiles").invoke();   // racks the bolts on the quiver mesh, as the game's respawn path does
        if (int32_t* inQuiver = Field<int32_t>(willie, "Bolts in Quiver")) *inQuiver = bolts;
    }

    // Hand it armed: "Event Add Bolt" spawns the bolt on the rails from the quiver above, "Event Load Bolt" seats it
    // (a zero-delay latent chain) and "String Loaded" is what fire gates on. A step that fails to invoke just leaves
    // the cocking to the game's own reload UI, which now has the quiver it needs.
    bool armed = false;
    if (inHand && bolts > 0) {
        Call add(x, "Event Add Bolt");
        Call load(x, "Event Load Bolt");
        armed = add.invoke() && load.invoke() && SetBool(x, "String Loaded", true);
    }
    HSM_LOG("adapter: crossbow given to %s: in hand %s, quiver %s, %d bolt(s), %s", ObjectName(willie).c_str(), inHand ? "yes" : "NO",
            quiver && *quiver ? "yes" : "NO", bolts, armed ? "armed" : "not armed");
    if (!inHand) DestroyActor(x);                   // never leave a loose crossbow behind a failed pick-up
    return inHand;
}

// ---------------------------------------------------------------- capture the flag (the owner's CTF test)
namespace {
// What a flag may be, in the order tried: long tools first (a long haft reads as a flag pole), then the lantern. The game has no
// banner weapon, and a polearm haft without its head is not creatable without module setup. Names go through FindClassLoose.
constexpr const char* kFlagClasses[] = {"BP_Weapon_Tool_Rake_C", "BP_Weapon_Tool_PitchforkA_C", "BP_Weapon_Tool_PitchforkB_C", "BP_Weapon_Tool_ShovelA_C",
                                        "BP_Weapon_Tool_ShovelB_C", "BP_Weapon_Tool_ScytheA_C", "BP_Weapon_Improv_Lantern_C"};

std::vector<UObject*> ComponentsOf(UObject* actor, UClass* cls) {
    std::vector<UObject*> parts;
    if (!actor || !cls) return parts;
    Call comps(actor, "K2_GetComponentsByClass");
    comps.set<UClass*>("ComponentClass", cls);
    if (!comps.invoke()) return parts;
    void* value = comps.paramPtr("ReturnValue");
    const FObjArray* arr = static_cast<const FObjArray*>(value);
    if (arr && arr->data && arr->num > 0 && arr->num <= 64) parts.assign(arr->data, arr->data + arr->num);
    if (FProperty* prop = FindProperty(comps.function(), "ReturnValue"); prop && value) ClearPropertyValue(prop, value);
    return parts;
}
}  // namespace

int Adapter::TintFlag(UObject* flag, int team) {
    if (!meshClass_) meshClass_ = FindClass("MeshComponent");
    int n = 0;
    for (UObject* part : ComponentsOf(flag, meshClass_)) n += TintMesh(part, KitSlot::Cloth, TeamKit::Full, team);
    return n;
}

void Adapter::FlagFreeze(UObject* flag, bool frozen) {
    UClass* prim = FindClass("PrimitiveComponent");
    if (!flag || !prim) return;
    std::vector<FlagBody>& saved = flagBodies_[flag];
    if (frozen) {
        if (saved.empty())
            for (UObject* c : ComponentsOf(flag, prim)) {
                FlagBody b; b.comp = c;
                Call col(c, "GetCollisionEnabled"); if (col.invoke()) b.collision = col.get<uint8_t>("ReturnValue");
                Call sim(c, "IsSimulatingPhysics"); if (sim.invoke()) b.simulating = sim.get<bool>("ReturnValue");
                saved.push_back(b);
            }
        for (const FlagBody& b : saved) {
            Call(b.comp, "SetSimulatePhysics").set<bool>("bSimulate", false).invoke();
            Call(b.comp, "SetCollisionEnabled").set<uint8_t>("NewType", 0).invoke();
        }
        return;
    }
    for (const FlagBody& b : saved) {
        Call(b.comp, "SetCollisionEnabled").set<uint8_t>("NewType", b.collision).invoke();
        if (b.simulating) Call(b.comp, "SetSimulatePhysics").set<bool>("bSimulate", true).invoke();
    }
}

UObject* Adapter::SpawnFlagActor(int team, const FVector& at, std::string* className) {
    if (!flagClass_) {
        for (const char* n : kFlagClasses) if (UClass* c = FindClassLoose(n)) { flagClass_ = c; flagClassName_ = ObjectName(reinterpret_cast<UObject*>(c)); break; }
        if (flagClass_) HSM_LOG("ctf: flag actor class %s", flagClassName_.c_str());
        else { HSM_LOG("ctf: no flag actor class is loaded (tried Rake, Pitchfork A/B, Shovel A/B, Scythe, Lantern); the flags are HUD markers only"); return nullptr; }
    }
    if (className) *className = flagClassName_;
    UObject* a = SpawnActorAt(flagClass_, at, FRotator{});
    if (!a) { HSM_LOG("ctf: spawning the flag actor %s failed", flagClassName_.c_str()); return nullptr; }
    FlagFreeze(a, true);
    modActors_.emplace_back(a, ObjectIndex(a));
    HSM_LOG("ctf: team %d flag spawned as %s at (%.0f, %.0f, %.0f): tinted %d parameter(s)", team, flagClassName_.c_str(), at.x, at.y, at.z, TintFlag(a, team));
    return a;
}

void Adapter::FlagPlace(UObject* flag, UObject* carrier, const FVector& at, double yawDeg) {
    if (!flag) return;
    if (carrier) {
        for (int h = 0; h < 2; ++h) {                      // out of a hand, the way a dropped weapon leaves it
            UObject** held = Field<UObject*>(carrier, h == 0 ? "Weapon R" : "Weapon L");
            if (!held || *held != flag) continue;
            Call c(carrier, h == 0 ? "Set Up Right Hand Weapon" : "Set Up Left Hand Weapon");
            c.set<bool>(h == 0 ? "Dropped With no Damage" : "Dropped with No Damage", true);
            c.invoke();
        }
    }
    Call parent(flag, "GetAttachParentActor");
    if (parent.invoke() && parent.get<UObject*>("ReturnValue"))
        Call(flag, "K2_DetachFromActor").set<uint8_t>("LocationRule", 1).set<uint8_t>("RotationRule", 1).set<uint8_t>("ScaleRule", 1).invoke();
    FlagFreeze(flag, true);
    Call move(flag, "K2_SetActorLocationAndRotation");
    move.set<FVector>("NewLocation", at).set<FRotator>("NewRotation", FRotator{0, yawDeg, 0}).set<bool>("bSweep", false).set<bool>("bTeleport", true);
    move.invoke();
}

bool Adapter::FlagHeld(UObject* willie, UObject* flag) {
    if (!willie || !flag) return false;
    for (const char* hand : {"Weapon R", "Weapon L"}) { UObject** held = Field<UObject*>(willie, hand); if (held && *held == flag) return true; }
    return false;
}

bool Adapter::FlagHold(UObject* willie, UObject* flag) {
    if (!willie || !flag) return false;
    for (int h = 0; h < 2; ++h) {
        UObject** held = Field<UObject*>(willie, h == 0 ? "Weapon R" : "Weapon L");
        if (!held || (*held && !IsBodyWeapon(*held))) continue;     // a weapon in this hand: not ours to drop
        FlagFreeze(flag, false);
        Call take(willie, h == 0 ? "Set Up Right Hand Weapon" : "Set Up Left Hand Weapon");
        take.set<UObject*>(h == 0 ? "Weapon R" : "Weapon L", flag);
        const bool called = take.valid() && take.invoke();
        UObject** now = Field<UObject*>(willie, h == 0 ? "Weapon R" : "Weapon L");
        if (called && now && *now == flag) return true;
        FlagFreeze(flag, true);
    }
    return false;
}

bool Adapter::FlagAttachedTo(UObject* carrier, UObject* flag) {
    Call parent(flag, "GetAttachParentActor");
    return carrier && flag && parent.invoke() && parent.get<UObject*>("ReturnValue") == carrier;
}

bool Adapter::FlagCarryOnBack(UObject* carrier, UObject* flag) {
    UObject* mesh = carrier ? MeshOf(carrier) : nullptr;
    if (!mesh || !flag) return false;
    FlagFreeze(flag, true);
    Call(flag, "K2_AttachToComponent").set<UObject*>("Parent", mesh).setName("SocketName", MakeName("Sheath_Slot_Back"))
        .set<uint8_t>("LocationRule", 2).set<uint8_t>("RotationRule", 2).set<uint8_t>("ScaleRule", 2).set<bool>("bWeldSimulatedBodies", false).invoke();
    return FlagAttachedTo(carrier, flag);
}

int Adapter::DropHeldWeapons(UObject* willie) {
    int dropped = 0;
    if (!willie) return 0;
    for (int h = 0; h < 2; ++h) {
        UObject** held = Field<UObject*>(willie, h == 0 ? "Weapon R" : "Weapon L");
        uint8_t* grip = Field<uint8_t>(willie, h == 0 ? "R_GripType_Current" : "L_GripType_Current");
        if (!held || !*held || IsBodyWeapon(*held) || !grip || *grip == 0) continue;
        Call c(willie, h == 0 ? "Set Up Right Hand Weapon" : "Set Up Left Hand Weapon");
        c.set<bool>(h == 0 ? "Dropped With no Damage" : "Dropped with No Damage", true);
        if (c.invoke()) ++dropped;
    }
    return dropped;
}

int Adapter::DropLoot(UObject* willie, uint32_t seed, int& weapons) {
    weapons = 0;
    if (!willie) return 0;
    for (int h = 0; h < 2; ++h) {   // the hand weapon drop the prop sync uses (a grip type of 0 would destroy the weapon)
        UObject** held = Field<UObject*>(willie, h == 0 ? "Weapon R" : "Weapon L");
        uint8_t* grip = Field<uint8_t>(willie, h == 0 ? "R_GripType_Current" : "L_GripType_Current");
        if (!held || !*held || IsBodyWeapon(*held) || !grip || *grip == 0) continue;
        Call c(willie, h == 0 ? "Set Up Right Hand Weapon" : "Set Up Left Hand Weapon");
        c.set<bool>(h == 0 ? "Dropped With no Damage" : "Dropped with No Damage", true);
        if (c.invoke()) ++weapons;
    }
    Call att(willie, "GetAttachedActors");
    att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", false);
    FObjArray* arr = att.invoke() ? static_cast<FObjArray*>(att.paramPtr("OutActors")) : nullptr;
    if (!arr || arr->num <= 0 || arr->num > 256) return 0;
    std::vector<UObject*> worn;
    for (int32_t i = 0; i < arr->num; ++i) {
        UObject* a = arr->data[i];
        if (!a || !ArmorClass(ClassOf(a)) || !Field<uint8_t>(a, "Armor Slot")) continue;
        if (ObjectName(reinterpret_cast<UObject*>(ClassOf(a))).find("Panties") != std::string::npos) continue;   // never shed
        worn.push_back(a);
    }
    std::sort(worn.begin(), worn.end());
    UObject* mesh = MeshOf(willie);
    int shed = 0;
    for (size_t k : LootPicks(worn.size(), kLootArmorPieces, seed)) {
        UObject* piece = worn[k];
        Call t(piece, "GetTransform");
        if (!t.invoke()) continue;
        Call remove(willie, "Remove Armor");
        remove.set<FTransform>("SpawnTransform", t.get<FTransform>("ReturnValue")).set<uint8_t>("Slot", *Field<uint8_t>(piece, "Armor Slot"))
            .set<UObject*>("Disable Collision With", mesh);
        ScriptHookBypass bypass;   // our own call (the Remove Armor observer still reports it)
        if (remove.valid() && remove.invoke()) ++shed;
    }
    return shed;
}

namespace {
struct LinearColor { float r, g, b, a; };

// One passport colour field takes the kit colour with its own alpha kept: FabricColor3's smuggles the steel type
// (Willie_BP reads floor(A*20)-1 out of it), and no colour's alpha is ours to change. An alpha that is not a finite
// number carries nothing, so 1 goes in there.
void PutColorKeepAlpha(uint8_t* field, const LinearColor& want) {
    LinearColor old, put = want;
    std::memcpy(&old, field, sizeof old);
    put.a = std::isfinite(old.a) ? old.a : 1.f;
    std::memcpy(field, &put, sizeof put);
}

// An armour piece's colour lives in its passport (the three fabric colours, and the leather colour on cloth); the
// game's own Setup Armor Material then pushes them into the layer-associated 'Color Tint' parameters of a material,
// which a by-name SetVectorParameterValue never reaches. So the passport is always rewritten (after the game reset a
// piece to its own colours a re-push costs the same as the first) and the setup is always run, once per material of
// the mesh, a plain material made dynamic first (what the piece's own construction script does). True when the setup
// ran; `pushed` (pieces) and `madeDynamic` (materials) feed the tester's log line.
bool RetintPassport(UObject* armor, FProperty* const (&colors)[3], FProperty* leather, const LinearColor& want, int& pushed, int& madeDynamic) {
    uint8_t* passport = Field<uint8_t>(armor, "Armor Passport");
    UObject** meshPtr = Field<UObject*>(armor, "Armor Mesh Skeletal");
    if (!passport || !meshPtr || !*meshPtr) return false;
    for (FProperty* c : colors) if (c) PutColorKeepAlpha(passport + PropertyOffset(c), want);
    if (leather) PutColorKeepAlpha(passport + PropertyOffset(leather), want);
    Call n(*meshPtr, "GetNumMaterials"); n.invoke();
    int32_t count = n.get<int32_t>("ReturnValue");
    bool ran = false;
    for (int32_t m = 0; m < count && m < 32; ++m) {
        Call gm(*meshPtr, "GetMaterial"); gm.set<int32_t>("ElementIndex", m).invoke();
        UObject* mat = gm.get<UObject*>("ReturnValue");
        if (!ClassNamed(mat, "MaterialInstanceDynamic")) {
            Call make(*meshPtr, "CreateDynamicMaterialInstance");      // SourceMaterial null: the slot's current material
            make.set<int32_t>("ElementIndex", m);
            mat = make.invoke() ? make.get<UObject*>("ReturnValue") : nullptr;
            if (mat) ++madeDynamic;
        }
        if (!mat) continue;
        Call setup(armor, "Setup Armor Material");
        setup.set<UObject*>("AddedArmorMesh", *meshPtr).setFrom("Passport", passport).set<UObject*>("SetupArmorDynamicMaterial", mat);
        if (setup.invoke()) ran = true;
    }
    if (ran) ++pushed;
    return ran;
}
bool IsArmorPiece(UObject* o) { return o && (ClassNamed(o, kArmorClass) || FindProperty(ClassOf(o), "Armor Passport")); }
}  // namespace

int Adapter::TintArmor(UObject* willie, float r, float g, float b) {
    Call att(willie, "GetAttachedActors");
    att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", true);
    if (!att.invoke()) return 0;
    FObjArray* arr = static_cast<FObjArray*>(att.paramPtr("OutActors"));
    UObject* st = FindObject(kArmorStruct, "UserDefinedStruct");
    if (!arr || !st || arr->num <= 0 || arr->num > 256) return 0;
    const LinearColor want{r, g, b, 1};
    FProperty* colors[] = {FindPropertyPrefix(st, "FabricColor1_"), FindPropertyPrefix(st, "FabricColor2_"), FindPropertyPrefix(st, "FabricColor3_")};
    FProperty* leather = FindPropertyPrefix(st, "LeatherColor_");
    int changed = 0, madeDynamic = 0;
    for (int32_t i = 0; i < arr->num; ++i) {
        UObject* armor = arr->data[i];
        if (!IsArmorPiece(armor)) continue;
        const KitSlot slot = SlotForArmorClass(ObjectName(reinterpret_cast<UObject*>(ClassOf(armor))));   // leather only on cloth, as the kit does
        RetintPassport(armor, colors, slot == KitSlot::Cloth ? leather : nullptr, want, changed, madeDynamic);
    }
    return changed;
}

// ---- Team kit (match/hsm_team_kit.h). The vector parameters of a material instance and of the instances it is made from,
// read from the reflected VectorParameterValues array (the overrides each level of the chain holds); the base material's
// own defaults are not listed there, so a parameter no instance overrides is not seen. Each distinct name is logged once
// per session with the slot kind it was seen on: the output of the owner's in-game spike.
namespace {
struct FArrayHeader { uint8_t* data = nullptr; int32_t num = 0, max = 0; };
struct VectorParamLayout {
    bool ok = false;
    int32_t nameOffset = 0, stride = 0;   // the parameter name's offset in an element (ParameterInfo.Name), the element size
};
const VectorParamLayout& ParamLayout() {
    static const VectorParamLayout l = [] {
        VectorParamLayout v;
        UObject* vpv = FindStruct("VectorParameterValue");
        UObject* info = FindStruct("MaterialParameterInfo");
        FProperty* pi = vpv ? StructMember(vpv, "ParameterInfo") : nullptr;
        FProperty* pv = vpv ? StructMember(vpv, "ParameterValue") : nullptr;
        FProperty* guid = vpv ? StructMember(vpv, "ExpressionGUID") : nullptr;
        FProperty* nm = info ? StructMember(info, "Name") : nullptr;
        if (!pi || !pv || !guid || !nm) return v;
        v.nameOffset = PropertyOffset(pi) + PropertyOffset(nm);
        int32_t end = 0;
        for (FProperty* p : {pi, pv, guid}) end = std::max(end, PropertyOffset(p) + PropertySize(p));
        v.stride = (end + 3) & ~3;
        v.ok = v.stride >= 32 && v.stride <= 128;
        return v;
    }();
    return l;
}

// Collects the distinct vector parameter names up the instance chain of `mat` (at most 6 levels).
void VectorParamNames(UObject* mat, std::vector<FName>& out) {
    const VectorParamLayout& L = ParamLayout();
    if (!L.ok) return;
    for (int depth = 0; mat && depth < 6; ++depth) {
        const FArrayHeader* h = Field<FArrayHeader>(mat, "VectorParameterValues");
        if (!h) break;   // a plain Material: its parameters are not reflected
        if (h->data && h->num > 0 && h->num <= 256)
            for (int32_t i = 0; i < h->num; ++i) {
                FName n{};
                std::memcpy(&n, h->data + size_t(i) * size_t(L.stride) + size_t(L.nameOffset), sizeof n);
                if (n.index <= 0 || NameToString(n).empty()) continue;
                bool known = false;
                for (const FName& o : out) if (o.index == n.index && o.number == n.number) { known = true; break; }
                if (!known && out.size() < 64) out.push_back(n);
            }
        UObject** parent = Field<UObject*>(mat, "Parent");
        mat = parent ? *parent : nullptr;
    }
}
}  // namespace

void Adapter::NoteKitParam(KitSlot slot, const std::string& name, bool tintable) {
    std::set<std::string>& seen = kitParamsSeen_[size_t(slot)];
    if (seen.size() >= 64 || !seen.insert(name).second) return;
    HSM_LOG("tint: %s material parameter \"%s\" (%s)", KitSlotName(slot), name.c_str(), tintable ? "colour, set" : "left alone");
}

// Sets the colour on every tintable vector parameter of every material of one mesh component. The dynamic material
// instance is made once: GetMaterial hands the component's own back on later calls. Returns how many were set.
int Adapter::TintMesh(UObject* mesh, KitSlot slot, TeamKit kit, int team) {
    if (!mesh) return 0;
    const Rgba plain = KitColor(kit, slot, team), trim = KitColor(kit, slot, team, true);
    if (plain.a == 0.f) return 0;
    Call n(mesh, "GetNumMaterials");
    if (!n.invoke()) return 0;
    const int32_t count = n.get<int32_t>("ReturnValue");
    int set = 0;
    for (int32_t m = 0; m < count && m < 16; ++m) {
        Call gm(mesh, "GetMaterial"); gm.set<int32_t>("ElementIndex", m);
        if (!gm.invoke()) continue;
        UObject* mat = gm.get<UObject*>("ReturnValue");
        if (!mat) continue;
        if (!ClassNamed(mat, "MaterialInstanceDynamic")) {
            Call make(mesh, "CreateDynamicMaterialInstance");      // SourceMaterial null: the slot's current material
            make.set<int32_t>("ElementIndex", m);
            mat = make.invoke() ? make.get<UObject*>("ReturnValue") : nullptr;
            if (!mat) continue;
        }
        std::vector<FName> names;
        VectorParamNames(mat, names);
        for (const FName& pn : names) {
            const std::string name = NameToString(pn);
            const bool ok = IsTintableParam(name);
            NoteKitParam(slot, name, ok);
            if (!ok) continue;
            const Rgba& c = IsTrimParam(name) && slot == KitSlot::Shield ? trim : plain;
            const LinearColor want{c.r, c.g, c.b, 1.f};
            Call put(mat, "SetVectorParameterValue");
            put.setName("ParameterName", pn).set("Value", want);
            if (put.invoke()) ++set;
        }
    }
    return set;
}

uint64_t Adapter::GearSignature(UObject* willie) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p) { h = (h ^ uint64_t(reinterpret_cast<uintptr_t>(p))) * 1099511628211ull; };
    Call att(willie, "GetAttachedActors");
    att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", true);
    if (att.invoke()) {
        const FObjArray* arr = static_cast<const FObjArray*>(att.paramPtr("OutActors"));
        if (arr && arr->data && arr->num > 0 && arr->num <= 256) for (int32_t i = 0; i < arr->num; ++i) mix(arr->data[i]);
    }
    for (const char* hand : {"Weapon R", "Weapon L"}) { UObject** held = Field<UObject*>(willie, hand); mix(held ? *held : nullptr); }
    return h;
}

namespace {
// The Willie fields the zombie traits touch (match/hsm_modes.h), resolved once per class.
struct InfectedFields {
    FProperty* zombie = nullptr;
    std::vector<int32_t> painDoubles;
    std::vector<FProperty*> painBools;
    FProperty* consciousness = nullptr;
    std::string consciousnessName;
};
const InfectedFields& InfectedFieldsFor(const UClass* cls) {
    static std::map<const UClass*, InfectedFields> cache;
    auto it = cache.find(cls);
    if (it != cache.end()) return it->second;
    InfectedFields f;
    if (FProperty* p = FindProperty(cls, kInfectedZombieFlag); p && PropertyType(p) == "BoolProperty") f.zombie = p;
    for (const char* n : kInfectedPainDoubles)
        if (FProperty* p = FindProperty(cls, n); p && PropertyType(p) == "DoubleProperty") f.painDoubles.push_back(PropertyOffset(p));
    for (const char* n : kInfectedPainBools)
        if (FProperty* p = FindProperty(cls, n); p && PropertyType(p) == "BoolProperty") f.painBools.push_back(p);
    for (const char* n : kInfectedConsciousnessFields)
        if (FProperty* p = FindProperty(cls, n); p && PropertyType(p) == "DoubleProperty") { f.consciousness = p; f.consciousnessName = n; break; }
    return cache.emplace(cls, std::move(f)).first->second;
}
}  // namespace

Adapter::InfectedTraits Adapter::ApplyInfectedTraits(UObject* willie, double baseline) {
    InfectedTraits t;
    t.baseline = baseline;
    if (!willie) return t;
    const InfectedFields& f = InfectedFieldsFor(ClassOf(willie));
    uint8_t* base = reinterpret_cast<uint8_t*>(willie);
    if (f.zombie) { t.zombieFlag = true; SetBoolValue(f.zombie, base + PropertyOffset(f.zombie), true); }
    for (int32_t off : f.painDoubles) { double zero = 0.0; std::memcpy(base + off, &zero, sizeof zero); ++t.painFields; }
    for (FProperty* p : f.painBools) { SetBoolValue(p, base + PropertyOffset(p), false); ++t.painFields; }
    if (f.consciousness) {
        t.consciousness = f.consciousnessName;
        double* c = reinterpret_cast<double*>(base + PropertyOffset(f.consciousness));
        if (!std::isfinite(t.baseline)) t.baseline = *c;
        *c = InfectedConsciousness(*c, InfectedConsciousnessFloor(t.baseline));
    }
    return t;
}

KitTally Adapter::TintKit(UObject* willie, int team, TeamKit kit) {
    KitTally t;
    if (!willie || kit == TeamKit::Off || KitColorsFor(team).base.a == 0.f) return t;
    std::vector<UObject*> actors;
    Call att(willie, "GetAttachedActors");
    att.set<bool>("bResetArray", true).set<bool>("bRecursivelyIncludeAttachedActors", true);
    if (att.invoke()) {
        const FObjArray* arr = static_cast<const FObjArray*>(att.paramPtr("OutActors"));
        if (arr && arr->data && arr->num > 0 && arr->num <= 256) actors.assign(arr->data, arr->data + arr->num);
    }
    for (const char* hand : {"Weapon R", "Weapon L"}) {     // held weapons are attached too, but reading the field costs nothing
        UObject** held = Field<UObject*>(willie, hand);
        if (held && *held && std::find(actors.begin(), actors.end(), *held) == actors.end()) actors.push_back(*held);
    }
    UObject* st = FindObject(kArmorStruct, "UserDefinedStruct");
    FProperty* colors[3] = {st ? FindPropertyPrefix(st, "FabricColor1_") : nullptr, st ? FindPropertyPrefix(st, "FabricColor2_") : nullptr,
                            st ? FindPropertyPrefix(st, "FabricColor3_") : nullptr};
    FProperty* leather = st ? FindPropertyPrefix(st, "LeatherColor_") : nullptr;
    if (!meshClass_) meshClass_ = FindClass("MeshComponent");
    int pushedPieces = 0, madeDynamic = 0;
    for (UObject* a : actors) {
        if (!a || IsGarbage(a)) continue;
        const std::string cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(a)));
        if (IsArmorPiece(a)) {
            const KitSlot slot = SlotForArmorClass(cls);
            const Rgba c = KitColor(kit, slot, team);
            if (c.a == 0.f) continue;
            // The passport path only: TintMesh's by-name writes never reach the layer-associated 'Color Tint' parameters
            // the game's own setup puts the passport into, and its blank dynamic instances would orphan the game's.
            if (st && RetintPassport(a, colors, slot == KitSlot::Cloth ? leather : nullptr, LinearColor{c.r, c.g, c.b, 1.f}, pushedPieces, madeDynamic))
                (slot == KitSlot::Plate ? t.plate : t.cloth) += 3;   // the passport's three fabric colours
        } else if (IsWeapon(a) && !IsBodyWeapon(a) && meshClass_) {
            if (IsProjectileClass(cls)) { ++t.skippedBlade; continue; }   // a crossbow's loaded bolt: never dressed
            bool flag = false;
            const bool shield = IsShieldClass(cls) || (GetBool(a, "Shield", flag) && flag);
            Call comps(a, "K2_GetComponentsByClass");
            comps.set<UClass*>("ComponentClass", meshClass_);
            if (!comps.invoke()) continue;
            void* value = comps.paramPtr("ReturnValue");
            const FObjArray* arr = static_cast<const FObjArray*>(value);
            std::vector<UObject*> parts;
            if (arr && arr->data && arr->num > 0 && arr->num <= 64) parts.assign(arr->data, arr->data + arr->num);
            if (FProperty* prop = FindProperty(comps.function(), "ReturnValue"); prop && value) ClearPropertyValue(prop, value);
            for (UObject* part : parts) {
                const KitSlot slot = SlotForWeaponPart(ObjectName(part), shield);
                if (!KitTints(kit, slot)) { if (slot == KitSlot::Blade) ++t.skippedBlade; continue; }
                (slot == KitSlot::Shield ? t.shield : t.grip) += TintMesh(part, slot, kit, team);
            }
        }
    }
    if (pushedPieces > 0 && kitPushSeen_.insert(team).second)   // once per team, like NoteKitParam's names: the tester's evidence
        HSM_LOG("adapter: kit %d: armour passport re-pushed on %d piece(s), %d material(s) made dynamic", team, pushedPieces, madeDynamic);
    return t;
}

UObject* Adapter::RespawnLocalPlayer(const SpawnOptions& o) {
    UObject* old = PlayerWillie(0);
    SpawnOptions puppet = o; puppet.brain = false; puppet.stockBrain = false;
    // In Progression the player's own fighter starts with the career's injuries (session/hsm_career.h).
    puppet.careerBody = hsm::CareerBodyAtSpawn(FreeModeActive(), CareerGameMode());
    UObject* w = SpawnFighter(puppet);
    if (!w) { HSM_LOG("adapter: local player respawn failed: the new fighter did not spawn"); return nullptr; }
    Call pc(statics_, "GetPlayerController");
    pc.set<UObject*>("WorldContextObject", world_).set<int32_t>("PlayerIndex", 0);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) { HSM_LOG("adapter: local player respawn failed: no player controller (new body destroyed)"); DestroyActor(w); return nullptr; }
    Call(ctrl, "Possess").set<UObject*>("InPawn", w).invoke();
    SetBool(w, "Player", true);
    game::RefreshDamageCache(w);   // as KeepRemoteFighter: the cached rate predates the Player flag
    if (old && old != w) {
        if (destroyAware_) DestroyFighter(old, ObjectIndex(old));   // owner sync: its feet and weapons go too (test 3 leaked them)
        else DestroyActor(old);
    }
    Call(ctrl, "ResetIgnoreInputFlags").invoke();   // the new body must take input whatever the death left behind
    if (inputCaptured_) CaptureInput(true);         // a line being typed keeps the keyboard (the new controller's too)
    else if (!PauseMenuOpen()) RestoreGameInput();   // under the open pause menu the cursor stays (ShowPauseCursor)
    HSM_LOG("adapter: local player respawned");
    return w;
}

// Abyss_Map_Open_EA_C::Spawn_NPC builds a procedural Abyss enemy (decomp Maps/Abyss_Map_Open_EA.cpp:175).
UObject* Adapter::SpawnAbyssNpc(const FVector& at) {
    std::vector<UObject*> level = FindInstances("Abyss_Map_Open_EA_C");
    if (level.empty()) return nullptr;
    FTransform t; t.translation = at;
    Call c(level[0], "Spawn NPC");
    c.set("SpawnTransform", t);
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

UObject* Adapter::SpawnBot(int rank, const FVector& at, double yawDeg, int teamInt) {
    UClass* gen = FindClass("BP_Generator_Characters_Random_C");
    UClass* willie = FindClass(kWillieClass);
    if (!gen || !willie) { HSM_LOG("adapter: bot generator not loaded"); return nullptr; }
    FTransform t; t.translation = at;
    Call begin(statics_, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", world_).set<UClass*>("ActorClass", gen).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 1).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* g = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!g) return nullptr;
    if (double* budget = Field<double>(g, "Equipment Budget")) *budget = 0;
    Call fin(statics_, "FinishSpawningActor");
    fin.set<UObject*>("Actor", g).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    g = fin.invoke() ? fin.get<UObject*>("ReturnValue") : nullptr;
    if (!g) return nullptr;
    Call make(g, "Generate Character");
    make.set<UClass*>("Actor Class", willie).set<uint8_t>("Nationality", 0).set<uint8_t>("Tier", uint8_t(std::clamp(rank, 0, 7)))
        .set<bool>("Is Mercenary", false).set<bool>("Ignore Budget", false);
    UObject* bot = nullptr;
    if (make.invoke()) {
        SpawnOptions o; o.at = at; o.yawDeg = yawDeg; o.teamInt = teamInt; o.brain = true; o.stockBrain = true; o.bot = true;
        o.passport = make.paramPtr("Character Passport");
        bot = o.passport ? SpawnFighter(o) : nullptr;
    }
    DestroyActor(g);
    return bot;
}

void Adapter::SuppressStockScreens(bool on, bool ownerMatch) {
    // Remove widgets suppressed since the last tick (never from inside their own construction).
    for (auto& [w, index] : g_suppressed) if (ObjectAt(index) == w) Call(w, "RemoveFromParent").invoke();
    g_suppressed.clear();
    g_suppressScreens = on;
    g_suppressOwnerScreens = on && ownerMatch;
    if (g_screenHooks) return;
    g_screenHooks = true;
    HookFunctionsMatching([](const std::string& n) { return n == "Construct"; }, [this](UObject* obj, UFunction*, void*) {
        // The main menu replays the intro video after 30 s idle (UI_Startup_Menu RetriggerableDelay -> UI_Video_Intro).
        // Skipping its Construct means no video, no sound and no input-mode change; the first-launch intro still plays.
        if (ClassNamed(obj, "UI_Video_Intro_C")) {
            bool played = false;
            if (UObject* gi = GameInstance()) GetBool(gi, "First Start Intro Played", played);
            if (!played) return false;
            g_suppressed.push_back({obj, ObjectIndex(obj)});
            return true;
        }
        // A second pause menu over the first: Willie's Esc action opens one whenever the world is not paused, and owner
        // sync keeps it unpaused (session.cpp LiftPauseFromWidgetTick), so each Esc stacked another menu. Only one stays.
        if (ObjectName(reinterpret_cast<UObject*>(ClassOf(obj))).find("UI_Pause") != std::string::npos && OtherPauseMenuOnScreen(obj)) {
            static uint32_t dupes = 0;
            if (++dupes % 50 == 1) HSM_LOG("adapter: a second pause menu was kept off the screen (%u time(s))", dupes);
            g_suppressed.push_back({obj, ObjectIndex(obj)});
            return true;
        }
        if (!g_suppressScreens) return false;
        for (const char* cls : kStockScreens) if (ClassNamed(obj, cls)) {
            HSM_LOG("adapter: suppressed stock %s during a session match", cls);
            g_suppressed.push_back({obj, ObjectIndex(obj)});
            return true;                                 // skip Construct: no slow motion, no input lock, no career reset
        }
        if (g_suppressOwnerScreens)
            for (const char* cls : kOwnerMatchScreens) if (ClassNamed(obj, cls)) {
                HSM_LOG("adapter: suppressed stock %s during an owner-sync match", cls);
                g_suppressed.push_back({obj, ObjectIndex(obj)});
                return true;                             // skip Construct: no pause, no tavern / next-fight menu
            }
        return false;
    });
}

}  // namespace hsm::game
