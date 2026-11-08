// AC-3: spawn selection returns distinct, maximally spread points for 1..6 fighters and degrades
// deterministically (reuse with ring offsets) when there are fewer candidates.
#include "session/hsm_spawn.h"
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestLine() {
    std::vector<Point3> line; for (int i = 0; i <= 10; ++i) line.push_back({double(i) * 100, 0, 0});
    auto p = SelectSpread(line, 3);
    CHECK(p.size() == 3 && p[0].index == 0 && p[1].index == 10 && p[2].index == 5);   // ends first, then middle
    for (const SpawnPick& s : p) CHECK(s.ring == 0);
}

static void TestDistinctAndSpread() {
    // 4 corners of a square plus 4 points clustered near the center: 4 fighters must take the 4 corners.
    std::vector<Point3> c = {{0, 0, 0}, {1000, 1000, 0}, {1000, 0, 0}, {0, 1000, 0}, {490, 500, 0}, {510, 500, 0}, {500, 490, 0}, {500, 510, 0}};
    for (size_t n = 1; n <= 6; ++n) {
        auto p = SelectSpread(c, n);
        std::set<size_t> idx; for (auto& s : p) idx.insert(s.index);
        CHECK(p.size() == n && idx.size() == n);                                         // distinct when enough candidates
    }
    auto four = SelectSpread(c, 4);
    std::set<size_t> corners; for (auto& s : four) corners.insert(s.index);
    CHECK(corners == std::set<size_t>({0, 1, 2, 3}));
}

static void TestFewerCandidatesAndDeterminism() {
    std::vector<Point3> two = {{0, 0, 0}, {500, 0, 0}};
    auto p = SelectSpread(two, 5);
    CHECK(p.size() == 5);
    CHECK(p[0].index == 0 && p[0].ring == 0 && p[1].index == 1 && p[1].ring == 0);
    CHECK(p[2].index == 0 && p[2].ring == 1 && p[3].index == 1 && p[3].ring == 1 && p[4].index == 0 && p[4].ring == 2);
    auto again = SelectSpread(two, 5);
    for (size_t i = 0; i < p.size(); ++i) CHECK(p[i].index == again[i].index && p[i].ring == again[i].ring);
    CHECK(SelectSpread({}, 3).empty() && SelectSpread(two, 0).empty());
}

static double Flat(const Point3& a, const Point3& b) { return std::hypot(a.x - b.x, a.y - b.y); }
static bool PairwiseApart(const std::vector<Point3>& p, double gap) {
    for (size_t i = 0; i < p.size(); ++i)
        for (size_t j = i + 1; j < p.size(); ++j) if (Flat(p[i], p[j]) < gap - 1e-6) return false;
    return true;
}

// Test 5: one candidate (the Abyss has no spawners) put fighters 120 cm apart, inside each other's ragdoll. A spread
// slot is never closer than the gap to any other slot; slot 0 is the point itself, ring 1 is six slots at the gap.
static void TestSpreadSlotsKeepTheGap() {
    const Point3 c{100, -50, 30};
    std::vector<Point3> slots;
    for (size_t k = 0; k < kSpreadSlots; ++k) slots.push_back(SpreadSlot(c, k, kMinFighterGapCm));
    CHECK(kMinFighterGapCm >= 250.0);                                   // a fallen Willie is about 200 cm long
    CHECK(kSpreadSlots >= 37);                                          // rings 0..3
    CHECK(PairwiseApart(slots, kMinFighterGapCm));
    CHECK(Flat(slots[0], c) == 0 && slots[0].z == c.z);
    for (size_t k = 1; k <= 6; ++k) CHECK(std::fabs(Flat(slots[k], c) - kMinFighterGapCm) < 1e-6 && slots[k].z == c.z);
    for (size_t k = 7; k <= 18; ++k) CHECK(std::fabs(Flat(slots[k], c) - 2 * kMinFighterGapCm) < 1e-6);
}

static void TestSpreadPositions() {
    const Point3 host{10, 20, -96};
    for (size_t n = 1; n <= 8; ++n) {                                   // one candidate: never closer than the gap
        auto p = SpreadPositions({host}, n);
        CHECK(p.size() == n && PairwiseApart(p, kMinFighterGapCm));
        CHECK(Flat(p[0], host) == 0);                                   // the first stays where the candidate is
    }
    std::vector<Point3> eight;                                          // eight spawners far apart: everyone on one
    for (int i = 0; i < 8; ++i) eight.push_back({std::cos(i * 0.785398) * 1500, std::sin(i * 0.785398) * 1500, double(i)});
    auto picks = SelectSpread(eight, 6);
    auto p = SpreadPositions(eight, 6);
    CHECK(p.size() == 6);
    for (size_t i = 0; i < p.size() && i < picks.size(); ++i)
        CHECK(p[i].x == eight[picks[i].index].x && p[i].y == eight[picks[i].index].y && p[i].z == eight[picks[i].index].z);
    auto close = SpreadPositions({{0, 0, 0}, {50, 0, 0}}, 2);           // a spawner next to the host
    CHECK(close.size() == 2 && PairwiseApart(close, kMinFighterGapCm) && Flat(close[0], {0, 0, 0}) == 0);
    auto twelve = SpreadPositions({{0, 0, 0}, {50, 0, 0}, {5000, 0, 0}}, 12);
    CHECK(twelve.size() == 12 && PairwiseApart(twelve, kMinFighterGapCm));
    CHECK(SpreadPositions({}, 3).empty() && SpreadPositions({host}, 0).empty());
}

// A (re)spawn next to bodies already standing (test 5: a stand-in respawned on top of the host): the first slot from
// the fighter's own on that no body is within the gap of, horizontally (a body on top counts).
static void TestFreeSpot() {
    const Point3 c{0, 0, 0};
    CHECK(Flat(FreeSpot(c, {}, 3, kMinFighterGapCm), SpreadSlot(c, 3, kMinFighterGapCm)) == 0);   // nothing near: its own slot
    std::vector<Point3> taken = {SpreadSlot(c, 3, kMinFighterGapCm)};
    taken[0].z += 160;                                                  // standing on top still counts
    Point3 f = FreeSpot(c, taken, 3, kMinFighterGapCm);
    CHECK(Flat(f, SpreadSlot(c, 4, kMinFighterGapCm)) == 0 && f.z == c.z);   // the next slot
    CHECK(Flat(FreeSpot(c, {{0, 0, 0}}, 0, kMinFighterGapCm), SpreadSlot(c, 1, kMinFighterGapCm)) == 0);
    std::vector<Point3> all;
    for (size_t k = 0; k < kSpreadSlots + 2; ++k) all.push_back(SpreadSlot(c, k, kMinFighterGapCm));
    CHECK(Flat(FreeSpot(c, all, 2, kMinFighterGapCm), SpreadSlot(c, 2, kMinFighterGapCm)) == 0);  // none free: its own slot
    all.pop_back();                                                     // the last slot searched is free
    CHECK(Flat(FreeSpot(c, all, 2, kMinFighterGapCm), SpreadSlot(c, kSpreadSlots + 1, kMinFighterGapCm)) == 0);
    std::vector<Point3> far = {{4000, 0, 0}};
    CHECK(Flat(FreeSpot(c, far, 0, kMinFighterGapCm), c) == 0);
}

// Test 5: "fighters stand -96 cm above the floor" in the Abyss (the trace hit something above); the Yard read 110-114.
static void TestSaneStandOffset() {
    CHECK(SaneStandOffset(114) == 114 && SaneStandOffset(0) == 0 && SaneStandOffset(kStandMaxCm) == kStandMaxCm);
    CHECK(SaneStandOffset(-96) == kStandDefaultCm && SaneStandOffset(-0.5) == kStandDefaultCm);
    CHECK(SaneStandOffset(250) == kStandMaxCm && SaneStandOffset(NAN) == kStandDefaultCm && SaneStandOffset(INFINITY) == kStandDefaultCm);
    CHECK(kStandDefaultCm > 0 && kStandDefaultCm < kStandMaxCm && kStandMaxCm == 200);
}

// Spawner-less maps: points on a ring around the host where a fighter can stand.
static void TestStandableRing() {
    const Point3 host{0, 0, 100};
    auto floorAt = [](const Point3& p) -> std::optional<double> {
        if (p.x > 390) return std::nullopt;                             // angle 0: over the edge, no floor
        if (p.y > 390) return 0.0 + 300;                                // angle 90: a ledge 300 cm up
        if (p.y < -390) return -10.0;                                   // angle 270: a small step down
        return 0.0;
    };
    std::vector<Point3> fighters = {{-400, 100, 100}};                  // someone stands near angle 180
    auto ring = StandableRing(host, 0.0, floorAt, fighters, 400);
    CHECK(ring.size() == 5);                                            // 8 minus no floor, the ledge and the occupied one
    for (const Point3& p : ring) {
        CHECK(std::fabs(Flat(p, host) - 400) < 1e-6);
        CHECK(Flat(p, fighters[0]) >= 150);
        CHECK(p.z == 0 || p.z == -10);                                  // the traced floor
    }
    CHECK(PairwiseApart(ring, kMinFighterGapCm));                       // 8 on 400 cm: 306 cm apart
    auto none = StandableRing(host, 0.0, [](const Point3&) -> std::optional<double> { return std::nullopt; }, {}, 400);
    CHECK(none.empty());
}

// Spawner-less maps with a navmesh (the Abyss): the level's own placement before Spawn_NPC, a random navigable point
// near an aim point, asked for each ring point; an answer is kept on the centre's floor, clear of the fighters and of
// the points kept before it.
static void TestNavigableRing() {
    const Point3 host{0, 0, 100};
    std::vector<Point3> asked;
    std::vector<double> tols;
    auto nav = [&](const Point3& aim, double tol) -> std::optional<Point3> {
        asked.push_back(aim); tols.push_back(tol);
        return Point3{aim.x + 20, aim.y, 7};                            // the navmesh sits a little above the floor
    };
    auto flat = [](const Point3&) -> std::optional<double> { return 5.0; };
    auto ring = NavigableRing(host, 0.0, nav, flat, {}, 400);
    CHECK(ring.size() == 8 && asked.size() == 8);
    for (size_t i = 0; i < asked.size(); ++i) {
        CHECK(std::fabs(Flat(asked[i], host) - 400) < 1e-6 && asked[i].z == host.z);
        CHECK(tols[i] == kNavToleranceCm);
    }
    if (asked.size() == 8) CHECK(std::fabs(asked[0].x - 400) < 1e-6 && std::fabs(asked[2].y - 400) < 1e-6);
    for (size_t i = 0; i < ring.size() && i < asked.size(); ++i)
        CHECK(std::fabs(ring[i].x - (asked[i].x + 20)) < 1e-6 && std::fabs(ring[i].y - asked[i].y) < 1e-6 && ring[i].z == 5.0);   // at the traced floor
    CHECK(PairwiseApart(ring, kMinFighterGapCm));
    asked.clear(); tols.clear();
    CHECK(NavigableRing(host, 0.0, nav, flat, {}, 600, 90).size() == 8 && !tols.empty() && tols[0] == 90);

    auto noNav = [](const Point3&, double) -> std::optional<Point3> { return std::nullopt; };
    CHECK(NavigableRing(host, 0.0, noNav, flat, {}, 400).empty());
    auto far = [](const Point3& aim, double tol) -> std::optional<Point3> { return Point3{aim.x + tol + 1, aim.y, 0}; };
    CHECK(NavigableRing(host, 0.0, far, flat, {}, 400).empty());                       // farther than asked: not an answer
    auto edge = [](const Point3& aim, double tol) -> std::optional<Point3> { return Point3{aim.x, aim.y + tol - 1, 0}; };
    CHECK(NavigableRing(host, 0.0, edge, flat, {}, 400).size() == 8);
    CHECK(kNavToleranceCm == 150.0);
    auto bad = [](const Point3& aim, double) -> std::optional<Point3> { return Point3{aim.x, std::nan(""), 0}; };
    CHECK(NavigableRing(host, 0.0, bad, flat, {}, 400).empty());
    auto badX = [](const Point3& aim, double) -> std::optional<Point3> { return Point3{std::nan(""), aim.y, 0}; };
    CHECK(NavigableRing(host, 0.0, badX, flat, {}, 400).empty());
    auto badZ = [](const Point3& aim, double) -> std::optional<Point3> { return Point3{aim.x, aim.y, std::nan("")}; };
    CHECK(NavigableRing(host, 0.0, badZ, flat, {}, 400).empty());
    auto shifted = [](const Point3& aim, double) -> std::optional<Point3> { return Point3{aim.x + 120, aim.y, 0}; };
    auto edgeFloor = [](const Point3& p) -> std::optional<double> { return p.x > 450 ? std::nullopt : std::optional<double>(0.0); };
    CHECK(NavigableRing(host, 0.0, shifted, edgeFloor, {}, 400).size() == 7);           // the floor is traced under the answer
    auto same = [](const Point3&, double) -> std::optional<Point3> { return Point3{300, 0, 0}; };
    auto one = NavigableRing(host, 0.0, same, flat, {}, 400, 1000);
    CHECK(one.size() == 1 && one[0].x == 300);                                         // one spot is one candidate
    auto close = [](const Point3& aim, double) -> std::optional<Point3> { return Point3{aim.x * 0.5, aim.y * 0.5, 0}; };
    auto tight = NavigableRing(host, 0.0, close, flat, {}, 400, 1000);                 // 8 on 200 cm: 153 cm apart
    CHECK(tight.size() >= 2 && tight.size() < 8 && PairwiseApart(tight, kMinFighterGapCm));

    auto floors = [](const Point3& p) -> std::optional<double> {
        if (p.x > 390) return std::nullopt;                             // angle 0: no floor under it
        if (p.y > 390) return kMaxStepCm + 1;                           // angle 90: a ledge
        if (p.y < -390) return -kMaxStepCm + 1;                         // angle 270: a step down
        return 0.0;
    };
    auto exact = [](const Point3& aim, double) -> std::optional<Point3> { return Point3{aim.x, aim.y, 0}; };
    std::vector<Point3> fighters = {{-400, 100, 100}};                  // someone stands near angle 180
    auto kept = NavigableRing(host, 0.0, exact, floors, fighters, 400);
    CHECK(kept.size() == 5);
    for (const Point3& p : kept) CHECK(Flat(p, fighters[0]) >= 150 && (p.z == 0 || p.z == -kMaxStepCm + 1));
    auto raised = NavigableRing(host, kMaxStepCm, exact, floors, {}, 400);             // the centre's own floor counts
    CHECK(raised.size() == 6);                                         // the ledge now level, the step down too far
    int ledge = 0;
    for (const Point3& p : raised) { CHECK(p.z != -kMaxStepCm + 1); if (p.z == kMaxStepCm + 1) ++ledge; }
    CHECK(ledge == 1);
}

static void TestFloorFits() {
    CHECK(!FloorFits(0.0, std::nullopt));                               // no floor (the pit) or a trap
    CHECK(!FloorFits(0.0, NAN));
    CHECK(FloorFits(std::nullopt, 500.0));                              // nothing to compare with: any floor
    CHECK(FloorFits(NAN, -40.0));
    CHECK(!FloorFits(std::nullopt, NAN) && !FloorFits(std::nullopt, INFINITY));   // nothing to compare with still needs a floor
    CHECK(FloorFits(10.0, 10.0 + kSpotStepCm) && FloorFits(10.0, 10.0 - kSpotStepCm));
    CHECK(!FloorFits(10.0, 10.0 + kSpotStepCm + 1) && !FloorFits(10.0, 10.0 - kSpotStepCm - 1));
    CHECK(!FloorFits(0.0, 150.0));                                      // Yard: a body's head under the trace
    CHECK(kSpotStepCm == 50);
    std::vector<Point3> traced;
    auto flat = [&](const Point3& q) { traced.push_back(q); return std::optional<double>(0.0); };
    CHECK(FootprintFits(0.0, {100, 100, 7}, flat) && traced.size() == 5);
    for (const Point3& q : traced) CHECK(q.z == 7 && (Flat(q, {100, 100, 0}) == 0 || std::fabs(Flat(q, {100, 100, 0}) - kFootprintCm) < 1e-9));
    for (int hole = 0; hole < 5; ++hole) {                              // any one point without a floor fails it
        int n = 0;
        auto oneHole = [&](const Point3&) { return n++ == hole ? std::nullopt : std::optional<double>(0.0); };
        CHECK(!FootprintFits(0.0, {0, 0, 0}, oneHole));
    }
    CHECK(!FootprintFits(0.0, {0, 0, 0}, [](const Point3& q) { return std::optional<double>(q.x > 50 ? 80.0 : 0.0); }));
}

// The Pit's spawners the level manager keeps in Free Mode (Map_Arena_Pit: _0 _1 _2 _4 _5 _6 _7 _10; the bridge's _8/_9
// and the six-combatant ring _11.._14 are destroyed by BP_LevelManager::Spawn_Combatants).
static std::vector<Point3> PitSpawners() {
    return {{2, 509, 1}, {-391, -394, 1}, {2, -584, 1}, {346, 334, 1}, {-338, 334, -1}, {527, 9, 1}, {-524, -7, 1}, {356, -359, 2}};
}
static bool InPit(const Point3& p) { return std::fabs(p.x) < 270 && p.y > -300 && p.y < 230; }
static bool BehindFence(const Point3& p) { return std::hypot(p.x, p.y) > 700; }

static void TestAreaOf() {
    const SpawnArea none = AreaOf({});
    CHECK(none.Contains({1e7, -1e7, 0}));                              // no candidates: everywhere
    const SpawnArea pit = AreaOf(PitSpawners());
    double far = 0;
    for (const Point3& c : PitSpawners()) far = std::fmax(far, Flat(c, pit.centre));
    CHECK(std::fabs(pit.radius - (far + kAreaMarginCm)) < 1e-9);
    for (const Point3& c : PitSpawners()) CHECK(pit.Contains(c));
    CHECK(!pit.Contains({127, 726, 105}));                              // test 2026-10-25: the puppet behind the fence
    const SpawnArea two = AreaOf({{0, 0, 0}, {100, 0, 0}});
    CHECK(two.radius == kAreaMinRadiusCm && two.Contains({650, 0, 0}) && !two.Contains({700, 0, 0}));
}

// Test 2026-10-25, the Pit: respawns off a corpse at (-524,-7) and (2,509) went to (-274,-7), the lip of the pit, and
// (127,726), behind the fence. With the floor, wall and area checks each goes to a slot a fighter can stand on.
static void TestStandableSpotPit() {
    const std::vector<Point3> cands = PitSpawners();
    const SpawnArea area = AreaOf(cands);
    std::vector<std::pair<Point3, Point3>> asked;
    auto stands = [&](const Point3& c, const Point3& p) {
        asked.push_back({c, p});
        auto floorAt = [](const Point3& q) { return InPit(q) ? std::nullopt : std::optional<double>(0.0); };
        return FootprintFits(0.0, p, floorAt) && !BehindFence(p);       // the wall: the fence ring
    };
    CHECK(!InPit({-274, -7, 0}));                                       // the lip itself has a floor under it...
    CHECK(!stands({-524, -7, 1}, {-274, -7, 1}));                       // ...but not under the whole fighter
    asked.clear();
    const Point3 west{-524, -7, 1}, north{2, 509, 1};
    CHECK(Flat(FreeSpot(west, {west}, 0, kMinFighterGapCm), {-274, -7, 0}) < 1);    // what 0.6.1 did
    size_t skipped = 0;
    const Point3 w = StandableSpot(west, {west}, 0, kMinFighterGapCm, area, stands, &skipped);
    CHECK(!InPit(w) && !BehindFence(w) && Flat(w, west) >= kMinFighterGapCm - 1e-6);
    CHECK(skipped == 1 && asked.front().first.x == west.x && asked.front().first.y == west.y);
    const std::vector<Point3> bodies = {north, SpreadSlot(north, 1, kMinFighterGapCm)};
    CHECK(Flat(FreeSpot(north, bodies, 0, kMinFighterGapCm), {127, 726, 0}) < 1);   // what 0.6.1 did
    const Point3 n = StandableSpot(north, bodies, 0, kMinFighterGapCm, area, stands);
    CHECK(!InPit(n) && !BehindFence(n) && area.Contains(n));
    for (const Point3& b : bodies) CHECK(Flat(n, b) >= kMinFighterGapCm - 1e-6);
}

static void TestStandableSpotFallback() {
    const Point3 c{100, 200, 5};
    int calls = 0;
    auto never = [&](const Point3&, const Point3&) { ++calls; return false; };
    size_t skipped = 0;
    Point3 s = StandableSpot(c, {c}, 0, kMinFighterGapCm, SpawnArea{}, never, &skipped);
    CHECK(Flat(s, c) == 0 && s.z == c.z);                               // nothing passes: the spawner itself
    CHECK(calls == int(kSpreadSlots) - 1 && skipped == kSpreadSlots - 1);
    calls = 0;
    s = StandableSpot(c, {}, 0, kMinFighterGapCm, SpawnArea{}, never);
    CHECK(Flat(s, c) == 0 && calls == 0);                               // slot 0 is the spawner: never asked
    const SpawnArea tiny{c, 10};
    int always = 0;
    s = StandableSpot(c, {c}, 0, kMinFighterGapCm, tiny, [&](const Point3&, const Point3&) { ++always; return true; });
    CHECK(Flat(s, c) == 0 && always == 0);                              // outside the area: not even traced
}

static void TestStandableSpread() {
    std::vector<Point3> cands = {{0, 0, 0}, {300, 0, 0}};
    auto yes = [](const Point3&, const Point3&) { return true; };
    auto a = StandableSpread(cands, 5, SpawnArea{}, yes);
    auto b = SpreadPositions(cands, 5);
    CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) CHECK(Flat(a[i], b[i]) == 0);
    auto onlyEast = [](const Point3& c, const Point3& p) { return p.x >= c.x; };
    auto e = StandableSpread(cands, 5, SpawnArea{}, onlyEast);
    CHECK(e.size() == 5 && PairwiseApart(e, kMinFighterGapCm));
    for (size_t i = 2; i < e.size(); ++i) CHECK(e[i].x >= 0);           // ring picks stay on the standable side
}

// The report: NPCs spawned right on the players at a Doubles start. The Cellar: four spawners; the player stood at one, the
// mercenary and the two opponents at the other three. A client's spread slot near the player is never one by another spawner.
static void TestCoopStart() {
    const Point3 player{722, 442, 120};
    const std::vector<Point3> spawners = {{700, 400, 100}, {722, 742, 100}, {-600, 0, 100}, {0, -600, 100}};
    std::vector<Point3> taken = CoopStartTaken(player, {player}, spawners);
    CHECK(taken.size() == 1 + 3);                                         // the fighter, and every spawner but the player's own
    for (const Point3& t : taken) CHECK(!(t.x == 700 && t.y == 400));    // the player's own spawner is no hazard
    bool mercenarySpawner = false;
    for (const Point3& t : taken) mercenarySpawner = mercenarySpawner || (t.x == 722 && t.y == 742);
    CHECK(mercenarySpawner);
    auto stands = [](const Point3&, const Point3&) { return true; };
    SpawnArea area;                                                       // no arena bound
    for (size_t e = 2; e <= 4; ++e) {                                     // three clients
        const Point3 p = StandableSpot(player, taken, e, kMinFighterGapCm, area, stands);
        for (const Point3& t : taken) CHECK(FlatDist2(p, t) >= kMinFighterGapCm * kMinFighterGapCm - 1e-6);
        taken.push_back(p);
    }
    CHECK(CoopStartTaken(player, {}, {}).empty());
    CHECK(CoopStartTaken(player, {}, {{1, 1, 0}}).empty());               // a lone spawner is the player's own
}

int main() {
    TestCoopStart();
    TestLine(); TestDistinctAndSpread(); TestFewerCandidatesAndDeterminism();
    TestSpreadSlotsKeepTheGap(); TestSpreadPositions(); TestFreeSpot(); TestSaneStandOffset(); TestStandableRing();
    TestNavigableRing(); TestFloorFits(); TestAreaOf(); TestStandableSpotPit(); TestStandableSpotFallback();
    TestStandableSpread();
    std::printf(g_fail ? "%d FAILED\n" : "all spawn tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
