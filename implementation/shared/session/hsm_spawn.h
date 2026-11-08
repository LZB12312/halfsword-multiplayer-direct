// Spawn-point selection. Game-independent, unit-tested.
// Picks spread-out points with farthest-point sampling (deterministic: starts at candidate 0). When there are
// more fighters than candidates, candidates are reused with an increasing ring number so the caller can offset
// them (ring * radius around the point) instead of stacking fighters.
// SpreadPositions / FreeSpot turn picks into places no two fighters share: a hexagonal spread of slots around a point,
// never closer than kMinFighterGapCm. SaneStandOffset and StandableRing check the floor traces of spawner-less maps;
// NavigableRing finds their points on the navmesh first, the way the Abyss places its own enemies.
#pragma once
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace hsm {

struct Point3 { double x = 0, y = 0, z = 0; };
struct SpawnPick { size_t index = 0; int ring = 0; };

inline double Dist2(const Point3& a, const Point3& b) { double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return dx * dx + dy * dy + dz * dz; }

inline std::vector<SpawnPick> SelectSpread(const std::vector<Point3>& candidates, size_t count) {
    std::vector<SpawnPick> picks;
    if (candidates.empty() || count == 0) return picks;
    std::vector<bool> used(candidates.size(), false);
    std::vector<size_t> chosen;
    size_t distinct = count < candidates.size() ? count : candidates.size();
    chosen.push_back(0); used[0] = true;
    while (chosen.size() < distinct) {
        size_t best = 0; double bestScore = -1;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (used[i]) continue;
            double nearest = INFINITY;
            for (size_t c : chosen) nearest = std::fmin(nearest, Dist2(candidates[i], candidates[c]));
            if (nearest > bestScore) { bestScore = nearest; best = i; }   // ties keep the lowest index
        }
        chosen.push_back(best); used[best] = true;
    }
    for (size_t k = 0; k < count; ++k) picks.push_back({chosen[k % chosen.size()], int(k / chosen.size())});
    return picks;
}

constexpr double kMinFighterGapCm = 250.0;   // a fallen Willie is about 200 cm long (test 5: 120 cm left bodies inside each other)
constexpr size_t kSpreadSlots = 37;          // rings 0..3 of SpreadSlot: what FreeSpot searches
constexpr double kStandDefaultCm = 110.0;    // actor height above its floor where the stock spawner put the host (Yard: 110-114)
constexpr double kStandMaxCm = 200.0;
constexpr double kMaxStepCm = 100.0;         // StandableRing: a floor this far above or below the centre's is a ledge or a pit
constexpr double kNavToleranceCm = 150.0;    // NavigableRing: how far from its ring point a navmesh answer may be

// Slot k of a hexagonal spread around c: slot 0 is c, ring n >= 1 holds 6n slots on a circle of n * gap, so any two
// slots are at least `gap` apart (neighbours on ring n: 2 n gap sin(pi / 6n) >= gap). Same height as c.
inline Point3 SpreadSlot(const Point3& c, size_t k, double gap) {
    if (k == 0) return c;
    size_t n = 1;
    while (1 + 3 * n * (n + 1) <= k) ++n;
    const size_t j = k - (1 + 3 * n * (n - 1));
    const double angle = double(j) * (6.283185307179586 / double(6 * n));
    return {c.x + std::cos(angle) * gap * double(n), c.y + std::sin(angle) * gap * double(n), c.z};
}

inline double FlatDist2(const Point3& a, const Point3& b) { double dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy; }

// The first spread slot around c, from `first` on, that no point of `taken` is within `gap` of, horizontally (a body
// lying on top of the spot counts); slot `first` itself when none of the kSpreadSlots slots is free.
inline Point3 FreeSpot(const Point3& c, const std::vector<Point3>& taken, size_t first, double gap) {
    const double g2 = gap * gap - 1e-6;
    for (size_t i = 0; i < kSpreadSlots; ++i) {
        const Point3 p = SpreadSlot(c, first + i, gap);
        bool clear = true;
        for (const Point3& t : taken) if (FlatDist2(p, t) < g2) { clear = false; break; }
        if (clear) return p;
    }
    return SpreadSlot(c, first, gap);
}

// Places for `count` fighters: SelectSpread's picks in order, each on the first slot around its candidate (from its
// ring on) that keeps it `gap` from the places before it. Candidates far enough apart are used as they are.
inline std::vector<Point3> SpreadPositions(const std::vector<Point3>& candidates, size_t count, double gap = kMinFighterGapCm) {
    std::vector<Point3> out;
    for (const SpawnPick& pk : SelectSpread(candidates, count)) out.push_back(FreeSpot(candidates[pk.index], out, size_t(pk.ring), gap));
    return out;
}

// A measured stand height (the host's actor z minus the floor traced under it) fit for every spawn. Negative (the
// trace hit something above the floor: test 5's Abyss read -96 cm) or not finite: kStandDefaultCm, so a spawn is never
// lowered into the floor; above kStandMaxCm: kStandMaxCm.
inline double SaneStandOffset(double raw) {
    if (!std::isfinite(raw) || raw < 0) return kStandDefaultCm;
    return raw > kStandMaxCm ? kStandMaxCm : raw;
}

// Spawner-less maps: `n` points on a circle of `radius` around `center` where a fighter can stand: floorAt (a trace
// down at the point, nullopt for no hit) finds a floor within kMaxStepCm of the centre's own, and no fighter in
// `fighters` is within `clear` of the point, horizontally. Each point is returned at its traced floor height.
template <class FloorAt>
std::vector<Point3> StandableRing(const Point3& center, double centerFloorZ, FloorAt floorAt, const std::vector<Point3>& fighters,
                                  double radius, int n = 8, double clear = 150.0) {
    std::vector<Point3> out;
    for (int i = 0; i < n; ++i) {
        const double angle = double(i) * (6.283185307179586 / double(n));
        Point3 p{center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius, center.z};
        const std::optional<double> z = floorAt(p);
        if (!z || !std::isfinite(*z) || std::fabs(*z - centerFloorZ) > kMaxStepCm) continue;
        bool occupied = false;
        for (const Point3& f : fighters) if (FlatDist2(p, f) < clear * clear) { occupied = true; break; }
        if (occupied) continue;
        p.z = *z;
        out.push_back(p);
    }
    return out;
}

// Spawner-less maps with a navmesh (the Abyss). The level script places each enemy at a random navigable point near
// an aim point (K2_GetRandomLocationInNavigableRadius, then Spawn_NPC: Abyss_Map_Open_EA ReceiveTick); here the aim
// points are the ring's `n` points around `center`. navPoint(aim, tolerance) is that query (nullopt: no navmesh
// there). An answer is kept when it is within `tolerance` of its aim, floorAt finds a floor under it within
// kMaxStepCm of the centre's own, no fighter is within `clear` of it and no point kept before it within
// kMinFighterGapCm (horizontally). Each point is returned at its traced floor height.
template <class NavPoint, class FloorAt>
std::vector<Point3> NavigableRing(const Point3& center, double centerFloorZ, NavPoint navPoint, FloorAt floorAt,
                                  const std::vector<Point3>& fighters, double radius, double tolerance = kNavToleranceCm,
                                  int n = 8, double clear = 150.0) {
    std::vector<Point3> out;
    for (int i = 0; i < n; ++i) {
        const double angle = double(i) * (6.283185307179586 / double(n));
        const Point3 aim{center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius, center.z};
        const std::optional<Point3> at = navPoint(aim, tolerance);
        if (!at || !std::isfinite(at->x) || !std::isfinite(at->y) || !std::isfinite(at->z)) continue;
        if (FlatDist2(*at, aim) > tolerance * tolerance) continue;
        const std::optional<double> z = floorAt(*at);
        if (!z || !std::isfinite(*z) || std::fabs(*z - centerFloorZ) > kMaxStepCm) continue;
        bool taken = false;
        for (const Point3& f : fighters) if (FlatDist2(*at, f) < clear * clear) { taken = true; break; }
        for (const Point3& k : out) if (FlatDist2(*at, k) < kMinFighterGapCm * kMinFighterGapCm) { taken = true; break; }
        if (taken) continue;
        out.push_back({at->x, at->y, *z});
    }
    return out;
}

// Spots around a spawner on a floor with holes and walls (two-player test 2026-10-25, the Pit: FreeSpot moved respawns
// off a corpse onto ring-1 slots 250 cm from good spawners, one at the lip of the pit in the middle, one behind the
// fence). A slot is used only where a fighter can stand: the engine checks a floor under it and nothing between it and
// its centre (a fence, a wall), and it stays inside the area the level's spawners span.
constexpr double kSpotStepCm = 50.0;       // a slot's floor this far from its centre's: a hole, a ledge or something lying there
constexpr double kWallProbeCm = 60.0;      // the wall check between a centre and its slot runs this high above the centre's floor
constexpr double kAreaMarginCm = 100.0;    // SpawnArea: how far past the outermost spawner a slot may be
constexpr double kAreaMinRadiusCm = 600.0; // SpawnArea: never smaller than this (one or two spawners span nothing)

// The floor test of a slot: a floor under it (nullopt: no floor, or a trap), within kSpotStepCm of its centre's floor.
// Without a centre floor to compare with, any floor.
inline bool FloorFits(std::optional<double> centreFloor, std::optional<double> slotFloor, double step = kSpotStepCm) {
    if (!slotFloor || !std::isfinite(*slotFloor)) return false;
    if (!centreFloor || !std::isfinite(*centreFloor)) return true;
    return std::fabs(*slotFloor - *centreFloor) <= step;
}

// A fighter is about a metre wide: a slot at the pit's lip (test 2026-10-25: (-274,-7), 4 cm past the mouth) has floor
// under its centre and falls in. Its floor is traced at the slot and kFootprintCm from it in four directions;
// floorAt(point) is that trace (nullopt: no floor, or a trap), and every one must fit the centre's floor.
constexpr double kFootprintCm = 75.0;
template <class FloorAt>
bool FootprintFits(std::optional<double> centreFloor, const Point3& p, FloorAt floorAt) {
    static constexpr double kDir[5][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    for (const auto& d : kDir)
        if (!FloorFits(centreFloor, floorAt(Point3{p.x + d[0] * kFootprintCm, p.y + d[1] * kFootprintCm, p.z}))) return false;
    return true;
}

// The area a level's spawn candidates span: a circle around their mean through the outermost one plus kAreaMarginCm,
// at least kAreaMinRadiusCm. Horizontal. No candidates: everywhere.
struct SpawnArea {
    Point3 centre;
    double radius = INFINITY;
    bool Contains(const Point3& p) const { return FlatDist2(p, centre) <= radius * radius; }
};
inline SpawnArea AreaOf(const std::vector<Point3>& candidates) {
    SpawnArea a;
    if (candidates.empty()) return a;
    for (const Point3& c : candidates) { a.centre.x += c.x; a.centre.y += c.y; a.centre.z += c.z; }
    const double n = double(candidates.size());
    a.centre.x /= n; a.centre.y /= n; a.centre.z /= n;
    double far2 = 0;
    for (const Point3& c : candidates) far2 = std::fmax(far2, FlatDist2(c, a.centre));
    a.radius = std::fmax(std::sqrt(far2) + kAreaMarginCm, kAreaMinRadiusCm);
    return a;
}

// FreeSpot where a fighter can stand: the first spread slot around c, from `first` on, that no point of `taken` is
// within `gap` of (horizontally), that lies in `area` and that stands(c, slot) accepts (the engine's floor and wall
// checks; never asked for c itself). None: c itself, a known spawner, even with a body there (better than the pit).
// `skipped`, when given, counts the slots clear of bodies that were turned down.
template <class Stands>
Point3 StandableSpot(const Point3& c, const std::vector<Point3>& taken, size_t first, double gap, const SpawnArea& area,
                     Stands stands, size_t* skipped = nullptr) {
    const double g2 = gap * gap - 1e-6;
    for (size_t k = first; k < first + kSpreadSlots; ++k) {
        const Point3 p = SpreadSlot(c, k, gap);
        bool clear = true;
        for (const Point3& body : taken) if (FlatDist2(p, body) < g2) { clear = false; break; }
        if (!clear) continue;
        if (k == 0 || (area.Contains(p) && stands(c, p))) return p;
        if (skipped) ++*skipped;
    }
    return c;
}

// SpreadPositions with StandableSpot: each pick on the first standable slot around its candidate, from its ring on,
// that keeps it `gap` from the places before it.
template <class Stands>
std::vector<Point3> StandableSpread(const std::vector<Point3>& candidates, size_t count, const SpawnArea& area, Stands stands,
                                    double gap = kMinFighterGapCm) {
    std::vector<Point3> out;
    for (const SpawnPick& pk : SelectSpread(candidates, count))
        out.push_back(StandableSpot(candidates[pk.index], out, size_t(pk.ring), gap, area, stands));
    return out;
}

// A Progression co-op fight's start (the report: NPCs spawned right on the players in Doubles). The host keeps the place the
// stock spawner gave the player: its Player Spawner holds nobody else (BP_LevelManager.cpp:403-413). The others spread round it
// (StandableSpot) clear of these points: every fighter in the level, and every spawner but the player's own (the nearest one),
// where an opponent or a hired mercenary stands, or is about to (the stock spawners can run after the mod's respawn).
inline std::vector<Point3> CoopStartTaken(const Point3& start, const std::vector<Point3>& fighters, const std::vector<Point3>& spawners) {
    std::vector<Point3> out = fighters;
    size_t own = spawners.size();
    for (size_t i = 0; i < spawners.size(); ++i)
        if (own == spawners.size() || FlatDist2(spawners[i], start) < FlatDist2(spawners[own], start)) own = i;
    for (size_t i = 0; i < spawners.size(); ++i) if (i != own) out.push_back(spawners[i]);
    return out;
}

}  // namespace hsm
