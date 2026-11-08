// Map reset: the MapReset wire, which objects are restored / destroyed / respawned / left, the per-frame pacing, and the gate
// that holds a queued reset until the respawn and the dropped-weapon clear are done.
#include "session/hsm_map_reset.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestCodec() {
    MapResetMsg m; m.seq = 0xBEEF; m.round = 7; m.flags = kMapResetRematch;
    const auto b = Encode(m);
    CHECK(b.size() == 4 + 2 + 2 + 1);
    CHECK(b[3] == 54 && b[2] == kProtocolRevision && kProtocolRevision == 41);
    MapResetMsg back;
    CHECK(Decode(b.data(), b.size(), back) && back.seq == 0xBEEF && back.round == 7 && back.flags == kMapResetRematch);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, back));           // every truncation
    auto longer = b; longer.push_back(0); CHECK(!Decode(longer.data(), longer.size(), back));  // trailing byte
    auto bad = b; bad.back() = 2; CHECK(!Decode(bad.data(), bad.size(), back));                // an unknown flag
    bad = b; bad[3] = 53; CHECK(!Decode(bad.data(), bad.size(), back));                        // another type
    bad = b; bad[2] = 27; CHECK(!Decode(bad.data(), bad.size(), back));                        // another revision
    CHECK(uint8_t(MsgType::MapReset) == 54 && kLastDecodedMsgType >= 54);
    MapResetMsg zero; const auto z = Encode(zero); CHECK(Decode(z.data(), z.size(), back) && back.seq == 0 && back.round == 0 && back.flags == 0);
}

static ResetCandidate Prop() { ResetCandidate c; c.kind = ResetKind::LevelProp; c.haveStart = true; return c; }
static ResetCandidate Spawned() { ResetCandidate c; c.kind = ResetKind::Spawned; c.bornInRound = true; return c; }
static ResetCandidate Weapon() { ResetCandidate c; c.kind = ResetKind::LevelWeapon; c.haveStart = true; c.classKnown = true; return c; }

static void TestPlanLevelProp() {
    CHECK(PlanReset(Prop()) == ResetAction::Restore);
    auto c = Prop(); c.alive = false; CHECK(PlanReset(c) == ResetAction::Leave);      // gone: the census drops it
    c = Prop(); c.held = true; CHECK(PlanReset(c) == ResetAction::Leave);
    c = Prop(); c.haveStart = false; CHECK(PlanReset(c) == ResetAction::Skip);        // never recorded
    c = Prop(); c.broken = true; CHECK(PlanReset(c) == ResetAction::Skip);            // a shattered geometry collection
    c = Prop(); c.detached = true; CHECK(PlanReset(c) == ResetAction::Skip);
    c = Prop(); c.bornInRound = true; CHECK(PlanReset(c) == ResetAction::Restore);    // the round flag is for spawned objects only
}

static void TestPlanSpawned() {
    CHECK(PlanReset(Spawned()) == ResetAction::Destroy);
    auto c = Spawned(); c.bornInRound = false; CHECK(PlanReset(c) == ResetAction::Leave);   // the level had it at the match start
    c = Spawned(); c.alive = false; CHECK(PlanReset(c) == ResetAction::Leave);
    c = Spawned(); c.held = true; CHECK(PlanReset(c) == ResetAction::Leave);
    c = Spawned(); c.detached = true; CHECK(PlanReset(c) == ResetAction::Skip);
    c = Spawned(); c.haveStart = true; c.classKnown = true; CHECK(PlanReset(c) == ResetAction::Destroy);   // starts and classes do not matter
}

static void TestPlanWeapon() {
    CHECK(PlanReset(Weapon()) == ResetAction::Restore);                               // lying about: back on its spot
    auto c = Weapon(); c.held = true; CHECK(PlanReset(c) == ResetAction::Leave);
    c = Weapon(); c.alive = false; CHECK(PlanReset(c) == ResetAction::Respawn);       // destroyed (dropped weapons are cleared): a new one
    c = Weapon(); c.alive = false; c.classKnown = false; CHECK(PlanReset(c) == ResetAction::Skip);   // a procedural weapon: skipped
    c = Weapon(); c.haveStart = false; CHECK(PlanReset(c) == ResetAction::Skip);
    c = Weapon(); c.haveStart = false; c.alive = false; CHECK(PlanReset(c) == ResetAction::Skip);
}

static void TestTally() {
    ResetTally t;
    for (ResetAction a : {ResetAction::Restore, ResetAction::Restore, ResetAction::Destroy, ResetAction::Respawn, ResetAction::Skip, ResetAction::Leave, ResetAction::Leave})
        t.Count(a);
    CHECK(t.restored == 2 && t.destroyed == 1 && t.respawned == 1 && t.skipped == 1 && t.left == 2);
}

static void TestPacing() {
    CHECK(ResetPerFrame(0) == kResetMinPerFrame);
    CHECK(ResetPerFrame(5) == kResetMinPerFrame);
    CHECK(ResetPerFrame(48) == 16);                                                   // exactly three frames of the minimum
    CHECK(ResetPerFrame(49) == 17);
    for (size_t total : {1u, 15u, 16u, 17u, 47u, 48u, 49u, 100u, 333u, 1000u, 4096u}) {
        const size_t per = ResetPerFrame(total);
        CHECK(per >= kResetMinPerFrame);
        CHECK(per * kResetMaxFrames >= total);                                        // done within kResetMaxFrames frames
        CHECK(total <= kResetMinPerFrame || (per - 1) * kResetMaxFrames < total || per == kResetMinPerFrame);   // and no more than needed
    }
}

static void TestGate() {
    MapResetGate g; MapResetMsg m, out;
    m.seq = 5; m.round = 2;
    CHECK(!g.pending());
    CHECK(!g.Poll(1000, false, out));                                                 // nothing queued
    g.Queue(m, 1000);
    CHECK(g.pending());
    CHECK(!g.Poll(1000 + kMapResetDelayMs - 1, false, out));                          // too early
    CHECK(g.Poll(1000 + kMapResetDelayMs, false, out) && out.seq == 5 && out.round == 2);
    CHECK(!g.pending() && !g.Poll(1000 + kMapResetDelayMs + 10, false, out));         // once
    g.Queue(m, 9000);                                                                 // the same seq again: a repeat
    CHECK(!g.pending());
    m.seq = 6; g.Queue(m, 9000);
    CHECK(g.pending());
    CHECK(!g.Poll(9000 + kMapResetDelayMs, true, out));                               // busy: waits ...
    CHECK(!g.Poll(9000 + kMapResetGiveUpMs - 1, true, out));
    CHECK(g.Poll(9000 + kMapResetGiveUpMs, true, out) && out.seq == 6);               // ... until the give-up
    m.seq = 7; g.Queue(m, 20000);
    CHECK(!g.Poll(20000 + 2000, true, out));
    CHECK(g.Poll(20000 + 2100, false, out) && out.seq == 7);                          // the busy part clears: it runs
    m.seq = 8; g.Queue(m, 30000); g.Cancel(); CHECK(!g.pending() && !g.Poll(40000, false, out));
    g.Reset(); m.seq = 7; g.Queue(m, 50000); CHECK(g.pending());                     // a new session forgets the old seq
}

static void TestGateClockWrap() {
    MapResetGate g; MapResetMsg m, out; m.seq = 1;
    const uint32_t t0 = 0xFFFFFFFFu - 500;                                            // the 32-bit millisecond clock wraps inside the wait
    g.Queue(m, t0);
    CHECK(!g.Poll(t0 + 100, false, out));
    CHECK(g.Poll(t0 + kMapResetDelayMs, false, out));
}

static void TestSupersede() {
    MapResetGate g; MapResetMsg a, b, out; a.seq = 1; a.round = 2; b.seq = 2; b.round = 3;
    g.Queue(a, 1000); g.Queue(b, 1200);                                               // a newer reset replaces one still waiting
    CHECK(g.Poll(1200 + kMapResetDelayMs, false, out) && out.seq == 2 && out.round == 3);
    CHECK(!g.pending());
}

static void TestMidMatchReload() {
    CHECK(MidMatchReload(true, true, false, false) == ReloadVerdict::Reload);
    CHECK(MidMatchReload(false, true, false, false) == ReloadVerdict::NoMatch);     // the menu, the hub
    CHECK(MidMatchReload(true, false, false, false) == ReloadVerdict::NoMatch);     // no match running
    CHECK(MidMatchReload(true, true, true, false) == ReloadVerdict::Unsupported);     // a wave cannot be rebuilt
    CHECK(MidMatchReload(true, true, false, true) == ReloadVerdict::Travelling);    // one reload at a time
    CHECK(ReloadForRound(true, true) && !ReloadForRound(true, false) && !ReloadForRound(false, true));
}

int main() {
    TestMidMatchReload();
    TestCodec(); TestPlanLevelProp(); TestPlanSpawned(); TestPlanWeapon(); TestTally(); TestPacing(); TestGate(); TestGateClockWrap(); TestSupersede();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("map reset: ok\n");
    return 0;
}
