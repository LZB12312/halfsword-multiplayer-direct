// Unit tests for hsm_contact_probe.h. Run via implementation/shared/run_tests.sh test_hsm_contact_probe.
//
// A contact away from the drawn body must be named: what was struck and how far off, for a body lying face down as well
// as a standing one.
#include "combat/hsm_contact_probe.h"
#include <cmath>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static void TestPlace() {
    CHECK(PlaceOfContact(true, true, true, 0.0) == ContactPlace::Body);
    CHECK(PlaceOfContact(true, true, true, kContactOffBodyCm) == ContactPlace::Body);
    CHECK(PlaceOfContact(true, true, true, kContactOffBodyCm + 0.1) == ContactPlace::OffBody);
    CHECK(PlaceOfContact(true, true, false, 50.0) == ContactPlace::Unread);
    // A part is a part wherever it sits: a blocker inside the body is still not the Mesh.
    CHECK(PlaceOfContact(false, true, true, 0.0) == ContactPlace::Part);
    CHECK(PlaceOfContact(false, true, false, 0.0) == ContactPlace::Part);
    CHECK(PlaceOfContact(false, false, true, 0.0) == ContactPlace::Gear);
    CHECK(PlaceOfContact(false, false, false, 0.0) == ContactPlace::Gear);
}

static void TestPlaceNames() {
    CHECK(std::string(ContactPlaceName(ContactPlace::Body)) == "body");
    CHECK(std::string(ContactPlaceName(ContactPlace::OffBody)) == "off-body");
    CHECK(std::string(ContactPlaceName(ContactPlace::Part)) == "part");
    CHECK(std::string(ContactPlaceName(ContactPlace::Gear)) == "gear");
    CHECK(std::string(ContactPlaceName(ContactPlace::Unread)) == "unread");
}

static void TestBudget() {
    ProbeBudget b;
    for (int i = 0; i < kProbeEventsPerSec; ++i) CHECK(b.Take(5000.0));
    CHECK(!b.Take(5000.0));
    CHECK(!b.Take(5999.0));
    CHECK(b.Take(6000.0));          // a new second
    for (int i = 1; i < kProbeEventsPerSec; ++i) CHECK(b.Take(6000.0));
    CHECK(!b.Take(6000.0));
    CHECK(b.Take(100.0));           // a clock that went back starts over
}

// Standing, facing +x: Unreal's right is +y.
static void TestFrameStanding() {
    const ProbeVec pelvis{0, 0, 100}, head{0, 0, 170}, l{0, -20, 150}, r{0, 20, 150};
    BodyOffset o;
    CHECK(BodyFrameOffset(pelvis, head, l, r, {-25, 5, 140}, o));
    CHECK(Near(o.forward, -25));    // behind
    CHECK(Near(o.right, 5));
    CHECK(Near(o.up, 40));
    CHECK(BodyFrameOffset(pelvis, head, l, r, {25, -5, 60}, o));
    CHECK(Near(o.forward, 25));
    CHECK(Near(o.right, -5));
    CHECK(Near(o.up, -40));
}

// Face down, head towards +x: the world's up is behind the body.
static void TestFrameFaceDown() {
    const ProbeVec pelvis{0, 0, 10}, head{70, 0, 10}, l{50, -20, 10}, r{50, 20, 10};
    BodyOffset o;
    CHECK(BodyFrameOffset(pelvis, head, l, r, {30, 0, 40}, o));
    CHECK(Near(o.forward, -30));
    CHECK(Near(o.up, 30));
    CHECK(Near(o.right, 0));
}

// A head off to one side of the pelvis (a bent body) does not tilt the shoulders' axis.
static void TestFrameBent() {
    BodyOffset o;
    CHECK(BodyFrameOffset({0, 0, 100}, {0, 30, 170}, {0, -20, 150}, {0, 20, 150}, {0, 10, 100}, o));
    CHECK(Near(o.right, 10));
    CHECK(Near(o.forward, 0));
    CHECK(Near(o.up, 0));
}

static void TestFrameDegenerate() {
    BodyOffset o;
    const ProbeVec p{1, 2, 3};
    CHECK(!BodyFrameOffset(p, p, p, p, p, o));                                       // nothing read yet
    CHECK(!BodyFrameOffset(p, {1, 2, 80}, p, p, p, o));                              // shoulders together
    CHECK(!BodyFrameOffset({0, 0, 0}, {0, 50, 0}, {0, -20, 0}, {0, 20, 0}, p, o));   // spine along the shoulders
    CHECK(!BodyFrameOffset({0, 0, 0}, {0, 0, 50}, {0, -20, 0}, {0, 20, 0}, {NAN, 0, 0}, o));
}

static void TestTallyEmpty() {
    ContactTally t;
    CHECK(t.DrainLine().empty());
}

static void TestTallyLine() {
    ContactTally t;
    CHECK(t.Add(ContactPlace::Body, "Willie_BP_C/CharacterMesh0 bone spine_03", true, 1.0));
    CHECK(!t.Add(ContactPlace::Body, "Willie_BP_C/CharacterMesh0 bone spine_03", true, 2.0));   // a second one: no line
    CHECK(t.Add(ContactPlace::OffBody, "Willie_BP_C/CharacterMesh0 bone pelvis", true, 23.25));
    CHECK(t.Add(ContactPlace::Part, "Willie_BP_C/Box1 bone spine_03", true, 41.0));
    CHECK(!t.Add(ContactPlace::Part, "Willie_BP_C/Box1 bone spine_03", true, 3.0));
    CHECK(!t.Add(ContactPlace::Part, "Willie_BP_C/Box1 bone spine_03", false, 900.0));   // not measured: not a distance
    CHECK(t.Add(ContactPlace::Gear, "BP_Shield_C/BaseMesh bone None", false, 0.0));
    CHECK(!t.Add(ContactPlace::Unread, "Willie_BP_C/CharacterMesh0 bone head", false, 0.0));
    t.Dropped();
    t.Dropped();
    const std::string line = t.DrainLine();
    CHECK(Has(line, "probe: contacts on stand-ins: body 2 off-body 1 part 3 gear 1 unread 1 dropped 2, farthest 41.0 cm"));
    CHECK(Has(line, "; top: Willie_BP_C/Box1 bone spine_03 x3 part, up to 41.0 cm"));   // the most contacts first
    CHECK(Has(line, "; Willie_BP_C/CharacterMesh0 bone pelvis x1 off-body, up to 23.2 cm"));
    CHECK(Has(line, "BP_Shield_C/BaseMesh bone None x1 gear, up to 0.0 cm"));
    CHECK(!Has(line, "spine_03 x2"));                  // body rows are not named
    CHECK(t.DrainLine().empty());                      // a fresh window
    CHECK(t.Add(ContactPlace::Part, "Willie_BP_C/Box1 bone spine_03", true, 1.0));   // lines come back with it
}

// A normal contact first must not hide a far one on the same bone.
static void TestTallySameBoneTwoPlaces() {
    ContactTally t;
    const char* bone = "Willie_BP_C/CharacterMesh0 bone spine_03";
    CHECK(t.Add(ContactPlace::Body, bone, true, 1.0));
    CHECK(t.Add(ContactPlace::OffBody, bone, true, 40.0));
    CHECK(!t.Add(ContactPlace::OffBody, bone, true, 2.0));
    const std::string line = t.DrainLine();
    CHECK(Has(line, "body 1 off-body 2"));
    CHECK(Has(line, "; top: Willie_BP_C/CharacterMesh0 bone spine_03 x2 off-body, up to 40.0 cm"));
}

static void TestTallyDroppedOnly() {
    ContactTally t;
    t.Dropped();
    CHECK(Has(t.DrainLine(), "dropped 1,"));
}

static void TestTallyTopLimit() {
    ContactTally t;
    for (size_t i = 0; i < kProbeTopRows + 2; ++i)
        for (size_t n = 0; n <= i; ++n) t.Add(ContactPlace::Part, "p" + std::to_string(i), true, 1.0);
    const std::string line = t.DrainLine();
    CHECK(Has(line, "top: p6 x7 part"));
    CHECK(Has(line, "; p2 x3 part"));
    CHECK(!Has(line, "; p1 "));
    CHECK(!Has(line, "; p0 "));
}

static void TestDetailLimits() {
    ContactTally t;
    int lines = 0;
    for (int i = 0; i < kProbeDetailsPerWindow + 5; ++i) lines += t.Add(ContactPlace::Gear, "g" + std::to_string(i), true, 1.0);
    CHECK(lines == kProbeDetailsPerWindow);
    lines = 0;
    for (int i = 0; i < kProbeBodyDetailsPerWindow + 5; ++i) lines += t.Add(ContactPlace::Body, "b" + std::to_string(i), true, 1.0);
    CHECK(lines == kProbeBodyDetailsPerWindow);    // body contacts have their own limit: they cannot use up the others'
}

static void TestRowLimit() {
    ContactTally t;
    for (size_t i = 0; i < kProbeTallyRows; ++i) t.Add(ContactPlace::Body, "b" + std::to_string(i), true, 1.0);
    CHECK(!t.Add(ContactPlace::Part, "late", true, 77.0));
    CHECK(!t.Add(ContactPlace::Part, "late2", true, 1.0));
    t.Add(ContactPlace::Body, "b0", true, 1.0);     // a row it already has still counts
    const std::string line = t.DrainLine();
    CHECK(Has(line, "body 65 off-body 0 part 2"));
    CHECK(Has(line, "farthest 77.0 cm"));           // the totals keep what the rows could not
    CHECK(Has(line, "2 more on other parts"));
}

int main() {
    TestPlace();
    TestPlaceNames();
    TestBudget();
    TestFrameStanding();
    TestFrameFaceDown();
    TestFrameBent();
    TestFrameDegenerate();
    TestTallyEmpty();
    TestTallyLine();
    TestTallySameBoneTwoPlaces();
    TestTallyDroppedOnly();
    TestTallyTopLimit();
    TestDetailLimits();
    TestRowLimit();
    if (g_fail) { std::printf("test_hsm_contact_probe: %d failed\n", g_fail); return 1; }
    std::printf("test_hsm_contact_probe: PASS\n");
    return 0;
}
