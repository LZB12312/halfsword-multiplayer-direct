// Unit tests for hsm_gvas.h. Run via implementation/shared/run_tests.sh test_hsm_gvas.
// HSM_GVAS_SAMPLE=<path to a real GameProgress.sav> also checks that file (never committed: it is a player's save).
#include "session/hsm_gvas.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using Bytes = std::vector<uint8_t>;

namespace {
struct Writer {
    Bytes b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void i32(int32_t v) { for (int i = 0; i < 4; ++i) u8(uint8_t(uint32_t(v) >> (8 * i))); }
    void str(const std::string& s) { i32(int32_t(s.size() + 1)); for (char c : s) u8(uint8_t(c)); u8(0); }
    void wstr(const std::string& s) { i32(-int32_t(s.size() + 1)); for (char c : s) u16(uint8_t(c)); u16(0); }
    void raw(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
    // "A(B,C(D))" as a complete type name tree
    void type(const std::string& t, size_t& i) {
        size_t j = i;
        while (j < t.size() && t[j] != '(' && t[j] != ',' && t[j] != ')') ++j;
        str(t.substr(i, j - i));
        std::vector<size_t> starts;
        if (j < t.size() && t[j] == '(') {
            // count the direct parameters first
            int depth = 0; int32_t n = 1;
            for (size_t k = j + 1; k < t.size(); ++k) {
                if (t[k] == '(') ++depth;
                else if (t[k] == ')') { if (depth-- == 0) break; }
                else if (t[k] == ',' && depth == 0) ++n;
            }
            i32(n);
            i = j + 1;
            for (int32_t k = 0; k < n; ++k) { type(t, i); ++i; }   // skip ',' or ')'
        } else {
            i32(0);
            i = j;
        }
    }
    void type(const std::string& t) { size_t i = 0; type(t, i); }
};

struct Options {
    int32_t saveVersion = 3;
    uint16_t major = 5, minor = 4;
    int32_t customFormat = 3, customs = 79;
    std::string cls = kPassportSaveClass;
    uint8_t control = 0;
};

Writer Header(const Options& o = {}) {
    Writer w;
    w.raw({'G', 'V', 'A', 'S'});
    w.i32(o.saveVersion); w.i32(522); w.i32(1012);
    w.u16(o.major); w.u16(o.minor); w.u16(4); w.i32(int32_t(2147486353u));
    w.str("++Halfsword+Main");
    w.i32(o.customFormat); w.i32(o.customs);
    for (int32_t i = 0; i < o.customs; ++i) { for (int k = 0; k < 16; ++k) w.u8(uint8_t(i + k)); w.i32(i); }
    w.str(o.cls);
    w.u8(o.control);
    return w;
}

void Prop(Writer& w, const std::string& name, const std::string& type, const Bytes& value, uint8_t flags = 0) {
    w.str(name); w.type(type); w.i32(int32_t(value.size())); w.u8(flags);
    if (flags & 0x01) w.i32(0);
    if (flags & 0x02) for (int k = 0; k < 16; ++k) w.u8(0);
    w.raw(value);
}

const std::string kPassportType = kGameProgressProperties[9].type;

void End(Writer& w) { w.str("None"); w.i32(0); }

Bytes Good() {
    Writer w = Header();
    Prop(w, "Day Count", "IntProperty", {7, 0, 0, 0});
    Prop(w, kPassportSaveProperty, kPassportType, Bytes(300, 0x11));
    Prop(w, "Fencer Present", "BoolProperty", {}, 0x10);
    End(w);
    return w.b;
}

bool Ok(const Bytes& b, std::string* why = nullptr) { return CheckPassportSave(b.data(), b.size(), why); }
std::string Why(const Bytes& b) { std::string w; CheckPassportSave(b.data(), b.size(), &w); return w; }
}  // namespace

static void TestAccepts() {
    CHECK(std::string(kGameProgressProperties[9].name) == kPassportSaveProperty);
    std::string why;
    CHECK(Ok(Good(), &why));
    if (!why.empty()) std::printf("  why: %s\n", why.c_str());
    // just the passport, or every listed property once (a fresh save object writes only non-default values)
    { Writer w = Header(); Prop(w, kPassportSaveProperty, kPassportType, Bytes(10, 1)); End(w); CHECK(Ok(w.b)); }
    {
        Writer w = Header();
        for (const GvasProperty& p : kGameProgressProperties) {
            std::string t = p.type;
            if (t == "BoolProperty") Prop(w, p.name, t, {}, 0x10);
            else if (t == "IntProperty") Prop(w, p.name, t, {1, 2, 3, 4});
            else Prop(w, p.name, t, Bytes(17, 3));
        }
        End(w);
        CHECK(Ok(w.b));
    }
    // no tail, UTF-16 names, a property guid
    { Bytes b = Good(); b.resize(b.size() - 4); CHECK(Ok(b)); }
    { Writer w = Header(); w.wstr(kPassportSaveProperty); w.type(kPassportType); w.i32(2); w.u8(0x02); for (int k = 0; k < 16; ++k) w.u8(9); w.raw({1, 2}); End(w); CHECK(Ok(w.b)); }
    { Writer w = Header(); Options o; o.customs = 0; w = Header(o); Prop(w, "Day Count", "IntProperty", {1, 0, 0, 0}); End(w); CHECK(Ok(w.b)); }
}

static void TestHeaderRejects() {
    Bytes b = Good();
    { Bytes x = b; x[0] = 'X'; CHECK(!Ok(x)); }
    auto with = [](auto set) { Options o; set(o); Writer w = Header(o); Prop(w, kPassportSaveProperty, kPassportType, Bytes(8, 1)); End(w); return w.b; };
    CHECK(Why(with([](Options& o) { o.saveVersion = 2; })) == "save version");
    CHECK(Why(with([](Options& o) { o.major = 4; })) == "engine version");
    CHECK(Why(with([](Options& o) { o.minor = 5; })) == "engine version");
    CHECK(Why(with([](Options& o) { o.customFormat = 2; })) == "custom version format");
    CHECK(Why(with([](Options& o) { o.customs = 257; })) == "custom versions");
    CHECK(Why(with([](Options& o) { o.control = 1; })) == "serialization control");
    // the class decides what LoadGameFromSlot instantiates: only SG_GameProgress_C, spelled exactly
    for (const char* cls : {"/Script/Engine.SaveGame", "/Game/Blueprints/SaveGame/SG_GameProgress.SG_GameProgress_C2",
                            "/game/blueprints/savegame/SG_GameProgress.SG_GameProgress_C", "/Script/Engine.Actor", ""})
        CHECK(Why(with([cls](Options& o) { o.cls = cls; })) == "save class");
    // sizes
    CHECK(!CheckPassportSave(nullptr, 0));
    CHECK(Why(Bytes(10, 0)) == "size");
    { Bytes big = Good(); big.resize(256 * 1024 + 1); CHECK(Why(big) == "size"); }
}

static void TestPropertyRejects() {
    auto one = [](const std::string& name, const std::string& type, const Bytes& v, uint8_t flags = 0) {
        Writer w = Header(); Prop(w, name, type, v, flags); End(w); return Why(w.b);
    };
    CHECK(one("Totally New", "IntProperty", {0, 0, 0, 0}) == "unknown property");
    CHECK(one("Day Count", "Int64Property", Bytes(8, 0)) == "unknown property");             // listed name, wrong type
    CHECK(one(kPassportSaveProperty, "ObjectProperty(/Script/Engine.Actor)", Bytes(8, 0)) == "unknown property");
    CHECK(one(kPassportSaveProperty, "StructProperty(Str_Passport_Character1(/Game/Blueprints/Structure/Passports/Str_Passport_Character1),00000000-0000-0000-0000-000000000000)", Bytes(8, 0)) == "unknown property");
    CHECK(one("Day Count", "IntProperty", {0, 0, 0, 0}, 0x08) == "property flags");           // native serialization
    CHECK(one("Day Count", "IntProperty", {0, 0, 0, 0}, 0x04) == "property flags");           // extensions
    CHECK(one("Day Count", "IntProperty", {0, 0}) == "int property");
    CHECK(one("Fencer Present", "BoolProperty", {1}) == "bool property");
    CHECK(one("Day Count", "IntProperty", {0, 0, 0, 0}, 0x10) == "bool property");
    { Writer w = Header(); Prop(w, "Day Count", "IntProperty", {1, 0, 0, 0}); Prop(w, "Day Count", "IntProperty", {2, 0, 0, 0}); End(w); CHECK(Why(w.b) == "repeated property"); }
    // control characters and non-ASCII in names
    { Writer w = Header(); Prop(w, std::string("Day\nCount"), "IntProperty", {0, 0, 0, 0}); End(w); CHECK(!Ok(w.b)); }
    { Writer w = Header(); w.i32(-3); w.u16('D'); w.u16(0x4E00); w.u16(0); End(w); CHECK(!Ok(w.b)); }
    // missing NUL, absurd lengths, deep or wide type trees
    { Writer w = Header(); w.i32(4); w.raw({'N', 'o', 'n', 'e'}); CHECK(!Ok(w.b)); }
    { Writer w = Header(); w.i32(INT32_MIN); End(w); CHECK(!Ok(w.b)); }
    { Writer w = Header(); w.i32(0x7FFFFFFF); End(w); CHECK(!Ok(w.b)); }
    { Writer w = Header(); w.str("Day Count"); for (int i = 0; i < 6; ++i) { w.str("ArrayProperty"); w.i32(1); } w.str("IntProperty"); w.i32(0); w.i32(0); w.u8(0); End(w); CHECK(Why(w.b) == "property tag"); }
    { Writer w = Header(); w.str("Day Count"); w.str("MapProperty"); w.i32(9); End(w); CHECK(Why(w.b) == "property tag"); }
    { Writer w = Header(); w.str("Day Count"); w.str("IntProperty"); w.i32(-1); End(w); CHECK(Why(w.b) == "property tag"); }
    { Writer w = Header(); w.str("Day Count"); w.str("IntProperty"); w.i32(0); w.i32(-4); w.u8(0); End(w); CHECK(Why(w.b) == "property tag"); }
    // a value running past the end, no "None", trailing bytes
    { Writer w = Header(); w.str(kPassportSaveProperty); w.type(kPassportType); w.i32(100000); w.u8(0); w.raw(Bytes(50, 0)); CHECK(Why(w.b) == "truncated"); }
    { Writer w = Header(); Prop(w, "Day Count", "IntProperty", {1, 0, 0, 0}); CHECK(Why(w.b) == "truncated"); }
    { Bytes b = Good(); b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0); CHECK(Why(b) == "trailing data"); }
    { Bytes b = Good(); b.back() = 1; CHECK(Why(b) == "trailing data"); }
    { Writer w = Header(); for (int i = 0; i < 65; ++i) Prop(w, "Day Count", "IntProperty", {0, 0, 0, 0}); End(w); CHECK(!Ok(w.b)); }
}

static void TestEveryTruncation() {
    Bytes b = Good();
    for (size_t n = 0; n + 4 < b.size(); ++n) CHECK(!CheckPassportSave(b.data(), n));   // bounds: never reads past n
    for (size_t i = 0; i < b.size(); ++i) {                                              // and never trusts one byte
        Bytes x = b; x[i] ^= 0xFF; CheckPassportSave(x.data(), x.size());
    }
}

// The multiplayer career's file (session/hsm_mp_career.h): a whole SG_GameProgress_C save of this engine, with properties
// the passport check would refuse.
static void TestCareerSave() {
    auto ok = [](const Bytes& b) { return CheckCareerSave(b.data(), b.size()); };
    auto why = [](const Bytes& b) { std::string w; CheckCareerSave(b.data(), b.size(), &w); return w; };
    CHECK(ok(Good()));
    {   // a career's own fields, and more of them than a passport may carry
        Writer w = Header();
        Prop(w, "Player Inventory", "StructProperty(Str_Inventory(/Game/Blueprints/Structure/Inventory/Str_Inventory),56ede05e-4678-b389-4f60-08b3af6d676b)", Bytes(5000, 7));
        Prop(w, "Reputation on Rank", "IntProperty", {9, 0, 0, 0});
        Prop(w, "Available Combatants", "ArrayProperty(IntProperty)", Bytes(300 * 1024, 2));
        End(w);
        CHECK(ok(w.b));
        CHECK(!CheckPassportSave(w.b.data(), w.b.size()));
    }
    { Writer w = Header(); End(w); CHECK(ok(w.b)); }                                     // a save of defaults only
    // the tail: "None", then at most 8 zero bytes
    { Bytes b = Good(); b.resize(b.size() - 4); CHECK(ok(b)); }
    { Bytes b = Good(); for (int i = 0; i < 4; ++i) b.push_back(0); CHECK(ok(b)); }
    { Bytes b = Good(); for (int i = 0; i < 5; ++i) b.push_back(0); CHECK(why(b) == "no end of properties"); }
    { Bytes b = Good(); b.back() = 1; CHECK(why(b) == "no end of properties"); }
    { Bytes b = Good(); b.push_back(7); CHECK(why(b) == "no end of properties"); }
    { Writer w = Header(); Prop(w, "Day Count", "IntProperty", {1, 0, 0, 0}); CHECK(why(w.b) == "no end of properties"); }
    // the header and the class, as for a passport: the class decides what LoadGameFromSlot makes
    { Bytes b = Good(); b[0] = 'X'; CHECK(why(b) == "not a save file"); }
    { Options o; o.cls = "/Script/Engine.SaveGame"; Writer w = Header(o); End(w); CHECK(why(w.b) == "save class"); }
    { Options o; o.minor = 5; Writer w = Header(o); End(w); CHECK(why(w.b) == "engine version"); }
    { Options o; o.control = 1; Writer w = Header(o); End(w); CHECK(why(w.b) == "serialization control"); }
    CHECK(!CheckCareerSave(nullptr, 0));
    CHECK(why(Bytes(10, 0)) == "size");
    { Bytes b = Good(); CHECK(!CheckCareerSave(b.data(), b.size(), nullptr, b.size() - 1)); }
    // cut short (only the zeros after "None" may go), and every flipped byte read in bounds
    const Bytes b = Good();
    for (size_t n = 0; n + 4 < b.size(); ++n) CHECK(!CheckCareerSave(b.data(), n));
    for (size_t i = 0; i < b.size(); ++i) { Bytes x = b; x[i] ^= 0xFF; CheckCareerSave(x.data(), x.size()); }
}

static void TestSample() {
    const char* path = std::getenv("HSM_GVAS_SAMPLE");
    if (!path) return;
    Bytes b;
    if (FILE* f = std::fopen(path, "rb")) {
        uint8_t buf[65536]; size_t k;
        while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + k);
        std::fclose(f);
    }
    std::string why;
    bool ok = CheckPassportSave(b.data(), b.size(), &why, 4 * 1024 * 1024);
    std::printf("sample %s (%zu bytes): %s %s\n", path, b.size(), ok ? "accepted" : "REJECTED", why.c_str());
    CHECK(ok);
    const bool career = CheckCareerSave(b.data(), b.size(), &why);
    std::printf("sample as a career save: %s %s\n", career ? "accepted" : "REJECTED", career ? "" : why.c_str());
    CHECK(career);
}

int main() {
    TestAccepts();
    TestHeaderRejects();
    TestPropertyRejects();
    TestEveryTruncation();
    TestCareerSave();
    TestSample();
    if (g_fail) { std::printf("test_hsm_gvas: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_gvas: ok\n");
    return 0;
}
