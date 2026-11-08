// Checks a peer's loadout blob before the engine sees it (pure, no engine types). The blob is a UE 5.4 SaveGame file
// (GVAS) of SG_GameProgress_C carrying the player's passport; LoadGameFromSlot creates whatever class the file names
// and fills it with the tagged-property serializer, which trusts the file, so a hostile blob could make every machine
// instantiate any class and feed it attacker bytes (security audit 2026-10-26, H1). Only a file shaped exactly like
// this build's own passport export passes: the header layout, the SG_GameProgress_C class path, and top-level
// properties that are SG_GameProgress_C's own with their exact types (layout read from the game's GameProgress.sav,
// Steam build 24185754), each at most once, then "None". Values inside those properties are not walked: the class
// and every top-level type are fixed, and the engine skips nested tags its structs do not have. A fresh save object
// writes only what differs from its defaults, so any subset of the list is fine.
#pragma once
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace hsm {

constexpr const char* kPassportSaveClass = "/Game/Blueprints/SaveGame/SG_GameProgress.SG_GameProgress_C";
constexpr const char* kPassportSaveProperty = "Free Mode Player Character 1";

struct GvasProperty { const char* name; const char* type; };
// SG_GameProgress_C's properties as the game writes them: name, then the type tree (UE 5.4 complete type names).
constexpr GvasProperty kGameProgressProperties[] = {
    {"Day Time", "ByteProperty(Enum_DayTime(/Game/Blueprints/Enumerator/Enum_DayTime))"},
    {"Day Count", "IntProperty"},
    {"Player Character", "StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),186c699a-4d6a-f0e8-1522-929b3295615b)"},
    {"Merchant Inventory", "StructProperty(Str_Inventory(/Game/Blueprints/Structure/Inventory/Str_Inventory),56ede05e-4678-b389-4f60-08b3af6d676b)"},
    {"Player Funds", "IntProperty"},
    {"Available Weapons 1H", "ArrayProperty(StructProperty(Str_Passport_Weapon1(/Game/Blueprints/Structure/Passports/Str_Passport_Weapon1),4ffe2bb5-464d-c6a5-8138-3cadd2c7eb0d))"},
    {"Available Weapons 2H", "ArrayProperty(StructProperty(Str_Passport_Weapon1(/Game/Blueprints/Structure/Passports/Str_Passport_Weapon1),4ffe2bb5-464d-c6a5-8138-3cadd2c7eb0d))"},
    {"Available Shields", "ArrayProperty(StructProperty(Str_Passport_Weapon1(/Game/Blueprints/Structure/Passports/Str_Passport_Weapon1),4ffe2bb5-464d-c6a5-8138-3cadd2c7eb0d))"},
    {"Available Custom Armor", "ArrayProperty(StructProperty(Str_Passport_Armor1(/Game/Blueprints/Structure/Passports/Str_Passport_Armor1),ad7b47e0-4889-02da-185c-209d93b75b21))"},
    {"Free Mode Player Character 1", "StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),186c699a-4d6a-f0e8-1522-929b3295615b)"},
    {"Fencer Present", "BoolProperty"},
    {"Merchant Present", "BoolProperty"},
    {"Armorsmith Present", "BoolProperty"},
    {"Jester Present", "BoolProperty"},
    {"Available Combat Events", "ArrayProperty(StructProperty(Str_Passport_CombatEvent1(/Game/Blueprints/Structure/Passports/Str_Passport_CombatEvent1),57a75596-462b-60d7-4021-35b763311a77))"},
    {"Player Tier", "ByteProperty(Enum_Ranks(/Game/Blueprints/Enumerator/Enum_Ranks))"},
    {"Player Body Condition", "StructProperty(Str_Character_Body_Condition(/Game/Blueprints/Structure/Character/Str_Character_Body_Condition),19b60ed0-4002-95c8-2e02-9c9821ade11e)"},
    {"Dead Opponents", "ArrayProperty(StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),186c699a-4d6a-f0e8-1522-929b3295615b))"},
    {"Mercenaries for Hire", "MapProperty(IntProperty,StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),186c699a-4d6a-f0e8-1522-929b3295615b))"},
    {"Last Open Level", "SoftObjectProperty"},
    {"Last Open Level Path", "NameProperty"},
    {"Rounds To WIn", "IntProperty"},
    {"Rounds Won", "IntProperty"},
    {"Spawned Combatants", "ArrayProperty(StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),186c699a-4d6a-f0e8-1522-929b3295615b))"},
    {"Current Combat Mode", "ByteProperty(Enum_CombatEvent_Modes(/Game/Blueprints/Enumerator/Enum_CombatEvent_Modes))"},
    {"Player Character In Hell", "StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),186c699a-4d6a-f0e8-1522-929b3295615b)"},
    {"Free Mode Player Character 2", "StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),186c699a-4d6a-f0e8-1522-929b3295615b)"},
    {"Nemesis Opponents", "ArrayProperty(StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),186c699a-4d6a-f0e8-1522-929b3295615b))"},
    {"First Start Intro Played", "BoolProperty"},
    {"First Start Tutorial Played", "BoolProperty"},
    {"First Start Innkeeper Greeted", "BoolProperty"},
    {"Armors Refreshed", "BoolProperty"},
};
constexpr size_t kGameProgressPropertyCount = sizeof kGameProgressProperties / sizeof kGameProgressProperties[0];

namespace gvas_detail {
struct Cursor {
    const uint8_t* d; size_t n; size_t p = 0; bool ok = true;
    bool need(size_t k) { if (!ok || n - p < k) ok = false; return ok; }
    void skip(size_t k) { if (need(k)) p += k; }
    uint8_t u8() { if (!need(1)) return 0; return d[p++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = uint16_t(d[p] | d[p + 1] << 8); p += 2; return v; }
    int32_t i32() { if (!need(4)) return 0; uint32_t v = uint32_t(d[p]) | uint32_t(d[p + 1]) << 8 | uint32_t(d[p + 2]) << 16 | uint32_t(d[p + 3]) << 24; p += 4; return int32_t(v); }
    // An FString of at most `max` characters: ANSI (positive length) printable ASCII only, or UTF-16 (negative) with
    // every unit below 0x80 so it reads back as the same ASCII. Lengths count the terminating NUL, which must be there.
    std::string str(size_t max) {
        int32_t len = i32();
        if (!ok) return {};
        if (len == 0) return {};
        bool wide = len < 0;
        if (len == INT32_MIN || size_t(wide ? -int64_t(len) : len) > max + 1) { ok = false; return {}; }
        size_t count = size_t(wide ? -int64_t(len) : len);
        if (!need(count * (wide ? 2 : 1))) return {};
        std::string s;
        for (size_t i = 0; i < count; ++i) {
            unsigned c = wide ? unsigned(d[p + 2 * i] | d[p + 2 * i + 1] << 8) : d[p + i];
            if (i + 1 == count) { if (c != 0) ok = false; break; }
            if (c < 0x20 || c >= 0x7F) { ok = false; break; }
            s.push_back(char(c));
        }
        p += count * (wide ? 2 : 1);
        return ok ? s : std::string();
    }
    // A complete type name: "Name" or "Name(Inner,Inner)", at most 4 levels and 8 parameters each.
    std::string typeName(int depth = 0) {
        std::string name = str(256);
        int32_t inner = i32();
        if (!ok || name.empty() || inner < 0 || inner > 8 || (inner > 0 && depth >= 4)) { ok = false; return {}; }
        if (inner == 0) return name;
        name += '(';
        for (int32_t i = 0; i < inner && ok; ++i) { if (i) name += ','; name += typeName(depth + 1); }
        return ok ? name + ")" : std::string();
    }
};

// The GVAS header of an SG_GameProgress_C save from this engine, up to its first property tag (the caller checked that
// at least 64 bytes are there). nullptr when it is one, else the first problem.
inline const char* ReadSaveHeader(Cursor& c) {
    if (std::memcmp(c.d, "GVAS", 4) != 0) return "not a save file";
    c.skip(4);
    int32_t saveVersion = c.i32();
    if (saveVersion != 3) return "save version";
    int32_t ue4 = c.i32(), ue5 = c.i32();
    if (ue4 < 500 || ue4 > 1000 || ue5 < 1000 || ue5 > 2000) return "package version";
    uint16_t major = c.u16(), minor = c.u16();
    c.u16(); c.i32();
    if (major != 5 || minor != 4) return "engine version";
    c.str(128);                                                   // branch
    if (c.i32() != 3) return "custom version format";
    int32_t customs = c.i32();
    if (!c.ok || customs < 0 || customs > 256) return "custom versions";
    c.skip(size_t(customs) * 20);                                 // FGuid + int32 each
    if (c.str(256) != kPassportSaveClass || !c.ok) return "save class";
    if (c.u8() != 0) return "serialization control";              // UE 5.4 class serialization control extension: none
    return nullptr;
}
}  // namespace gvas_detail

// true: `d` is a passport save this build would write. `why` names the first problem otherwise.
inline bool CheckPassportSave(const uint8_t* d, size_t n, std::string* why = nullptr, size_t maxBytes = 256 * 1024) {
    auto fail = [why](const char* w) { if (why) *why = w; return false; };
    if (!d || n < 64 || n > maxBytes) return fail("size");
    gvas_detail::Cursor c{d, n};
    if (const char* bad = gvas_detail::ReadSaveHeader(c)) return fail(bad);
    bool seen[kGameProgressPropertyCount] = {};
    for (int props = 0;; ++props) {
        std::string name = c.str(256);
        if (!c.ok) return fail("truncated");
        if (name == "None") break;
        if (props >= 64) return fail("too many properties");
        std::string type = c.typeName();
        int32_t size = c.i32();
        uint8_t flags = c.u8();
        if (!c.ok || size < 0) return fail("property tag");
        if (flags & ~uint8_t(0x01 | 0x02 | 0x10)) return fail("property flags");   // array index, guid, bool value only
        if (flags & 0x01) c.i32();
        if (flags & 0x02) c.skip(16);
        size_t k = 0;
        for (; k < kGameProgressPropertyCount; ++k)
            if (name == kGameProgressProperties[k].name) break;
        if (k == kGameProgressPropertyCount || type != kGameProgressProperties[k].type) return fail("unknown property");
        if (seen[k]) return fail("repeated property");
        seen[k] = true;
        if (type == "BoolProperty" ? size != 0 : (flags & 0x10) != 0) return fail("bool property");
        if (type == "IntProperty" && size != 4) return fail("int property");
        c.skip(size_t(size));
        if (!c.ok) return fail("truncated");
    }
    if (n - c.p > 8) return fail("trailing data");
    for (size_t i = c.p; i < n; ++i) if (d[i] != 0) return fail("trailing data");
    return true;
}

// The multiplayer career's file (session/hsm_mp_career.h): a whole SG_GameProgress_C save from this engine. This
// machine's own game wrote it (Save Game into the slot, copied out), so only its shape is checked: the header and save
// class, and that it ends as a save does, "None" with at most 8 zero bytes after it. That is no proof the file is whole
// (structs inside end with "None" too); the atomic write that made it sees to that. Its properties are not walked: a
// career carries far more than a passport (CheckPassportSave is the one for peers' bytes).
constexpr size_t kMaxCareerSaveBytes = size_t(32) << 20;
inline bool CheckCareerSave(const uint8_t* d, size_t n, std::string* why = nullptr, size_t maxBytes = kMaxCareerSaveBytes) {
    auto fail = [why](const char* w) { if (why) *why = w; return false; };
    if (!d || n < 64 || n > maxBytes) return fail("size");
    gvas_detail::Cursor c{d, n};
    if (const char* bad = gvas_detail::ReadSaveHeader(c)) return fail(bad);
    static constexpr uint8_t kNone[] = {5, 0, 0, 0, 'N', 'o', 'n', 'e', 0};   // FString "None", the end of the properties
    for (size_t zeros = 0; zeros <= 8; ++zeros) {                // the tail tried: the last `zeros` bytes, all zero
        if (zeros > 0 && d[n - zeros] != 0) break;               // the byte this try adds is not zero: nothing further back
        if (n - c.p < zeros + sizeof kNone) break;               // "None" would reach into the header
        if (std::memcmp(d + n - zeros - sizeof kNone, kNone, sizeof kNone) == 0) return true;
    }
    return fail("no end of properties");
}

}  // namespace hsm
