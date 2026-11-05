// AC-1: roster assigns at most kMaxPlayers (8) unique slots/entities by join order, reuses freed slots, rejects a 9th
// member; with the host slot reserved (role=server) the 8 players get slots 1..8.
#include "session/hsm_roster.h"
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestJoinOrderAndLimit() {
    Roster r;
    for (uint64_t k = 100; k < 100 + kMaxPlayers; ++k) {
        auto slot = r.Add(k, "p" + std::to_string(k));
        CHECK(slot.has_value() && *slot == k - 100);
    }
    CHECK(r.size() == kMaxPlayers && kMaxPlayers == 8 && kMaxFighters == 9);
    CHECK(!r.Add(999, "ninth").has_value());            // 9th member rejected: slot 8 stays empty with a host fighter
    CHECK(!r.BySlot(8));
    std::set<uint8_t> entities;
    for (const RosterMember& m : r.members()) { entities.insert(m.entity); CHECK(m.entity == m.slot + 1); }
    CHECK(entities.size() == kMaxPlayers);                 // unique entities 1..8
    CHECK(r.Remove(103) && *r.Add(998, "late") == 3);      // a freed slot is reused, never slot 8
}

static void TestServerSeatsEight() {
    Roster r; r.ReserveHostSlot(true);
    for (uint64_t k = 1; k <= uint64_t(kMaxPlayers); ++k) CHECK(r.Add(k, "c") == std::optional<uint8_t>(uint8_t(k)));
    CHECK(r.size() == 8 && r.BySlot(8) && r.ByEntity(9) && !r.BySlot(0));
    CHECK(!r.Add(99, "ninth").has_value());
    CHECK(r.Remove(8) && *r.Add(99, "ninth") == 8);
}

static void TestIdempotentAndReuse() {
    Roster r;
    CHECK(*r.Add(1, "host") == 0);
    CHECK(*r.Add(2, "a") == 1);
    CHECK(*r.Add(3, "b") == 2);
    CHECK(*r.Add(2, "a-again") == 1);                      // same key keeps its slot
    CHECK(r.size() == 3);
    CHECK(r.Rename(2, "renamed"));
    CHECK(r.Find(2)->name == "renamed" && r.Find(2)->slot == 1 && r.Find(2)->entity == 2);
    CHECK(r.Find(1)->name == "host" && !r.Rename(99, "missing"));
    CHECK(!r.Rename(2, "renamed"));
    CHECK(r.Remove(2) && !r.Remove(2));
    CHECK(*r.Add(4, "c") == 1);                            // lowest free slot reused
    CHECK(r.ByEntity(2) && r.ByEntity(2)->key == 4);
    CHECK(!r.ByEntity(7) && !r.Find(2) && r.BySlot(2)->key == 3);
}

int main() {
    TestJoinOrderAndLimit(); TestIdempotentAndReuse(); TestServerSeatsEight();
    std::printf(g_fail ? "%d FAILED\n" : "all roster tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
