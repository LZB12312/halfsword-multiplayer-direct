// Unit tests for hsm_protocol.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_protocol.cpp
#include "hsm_protocol.h"
#include "match/hsm_match.h"
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static void TestHello() {
    Hello h; h.buildKey = 0x1122334455667788ull; h.name = "player"; h.modBuild = "0.6.4 (v0.6.3-12-gdb82570)";
    auto b = Encode(h); Hello o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.buildKey == h.buildKey && o.name == "player" && o.revision == kProtocolRevision && o.modBuild == h.modBuild);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));      // truncation
    auto extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), o));  // trailing bytes
    auto bad = b; bad[2] = kProtocolRevision + 1; CHECK(!Decode(bad.data(), bad.size(), o));
    Hello longName; longName.name = std::string(100, 'x');
    auto lb = Encode(longName); CHECK(Decode(lb.data(), lb.size(), o) && o.name.size() == kMaxPlayerName);
    Hello longBuild; longBuild.modBuild = std::string(200, 'b');   // rev 17: the mod build, cut to kMaxModBuild
    auto bb = Encode(longBuild); CHECK(Decode(bb.data(), bb.size(), o) && o.modBuild == std::string(kMaxModBuild, 'b') && o.name.empty());
    Hello noBuild; noBuild.name = "x";
    auto nb = Encode(noBuild); CHECK(Decode(nb.data(), nb.size(), o) && o.modBuild.empty() && o.name == "x");
    CHECK(nb.size() == Encode(Hello{}).size() + noBuild.name.size());   // an empty build costs its length byte only
    // rev 35: the password ends the Hello (str, 32).
    Hello pw = h; pw.password = "s3cret pass";
    auto pb = Encode(pw); Hello po;
    CHECK(Decode(pb.data(), pb.size(), po) && po.password == "s3cret pass" && po.name == "player" && po.modBuild == h.modBuild);
    CHECK(pb.size() == b.size() + pw.password.size());                                  // an empty password costs its length byte only
    CHECK(pb[pb.size() - 1 - pw.password.size()] == uint8_t(pw.password.size()));        // length byte, then the bytes, at the very end
    for (size_t cut = 0; cut < pb.size(); ++cut) CHECK(!Decode(pb.data(), cut, po));
    Hello longPw = h; longPw.password = std::string(100, 'p');
    auto lpb = Encode(longPw); CHECK(Decode(lpb.data(), lpb.size(), po) && po.password == std::string(kMaxLobbyPassword, 'p'));
    auto over = pb; over[over.size() - 1 - pw.password.size()] = uint8_t(kMaxLobbyPassword + 1); over.resize(over.size() + 40, 'x');   // a length past the cap
    CHECK(!Decode(over.data(), over.size(), po));
    auto old = b; old.pop_back(); CHECK(!Decode(old.data(), old.size(), po));         // the pre-35 layout (no password byte) is not a Hello
}

static void TestInput() {
    InputFrame f; f.seq = 42; f.clientTimeMs = 1000; f.axes[size_t(Axis::MoveForward)] = 1.f; f.axes[size_t(Axis::TurnMouse)] = -3.5f;
    f.buttons = 1u << size_t(Action::GrabLeft) | 1u << size_t(Action::Thrust); f.controlYaw = 90; f.controlPitch = -10;
    auto b = Encode(f); InputFrame o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.seq == 42 && o.axes[size_t(Axis::TurnMouse)] == -3.5f && o.buttons == f.buttons && o.controlYaw == 90);
    InputFrame big = f; big.axes[0] = kAxisLimit * 2; auto bb = Encode(big); CHECK(!Decode(bb.data(), bb.size(), o));
    InputFrame nan = f; nan.axes[1] = std::numeric_limits<float>::quiet_NaN(); auto nb = Encode(nan); CHECK(!Decode(nb.data(), nb.size(), o));
    InputFrame inf = f; inf.controlYaw = std::numeric_limits<float>::infinity(); auto ib = Encode(inf); CHECK(!Decode(ib.data(), ib.size(), o));
    InputFrame btn = f; btn.buttons = 1u << kActionCount; auto bt = Encode(btn); CHECK(!Decode(bt.data(), bt.size(), o));
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
}

static void TestSnapshot() {
    Snapshot s; s.tick = 7; s.ackInputSeq = 41;
    for (int i = 0; i < 2; ++i) {
        WillieState w; w.entity = uint8_t(i + 1); w.flags = 2; w.health = 80; w.partHealth[3] = 12.5f; w.dismembered = 1u << 4;
        for (int k = 0; k < 20; ++k) { BodyState bs; bs.px = float(k); bs.rot = PackQuat(0, 0, 0.7071f, 0.7071f); w.bodies.push_back(bs); }
        s.willies.push_back(w);
    }
    auto b = Encode(s); Snapshot o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.willies.size() == 2 && o.willies[1].entity == 2 && o.willies[1].bodies.size() == 20 && o.willies[0].bodies[19].px == 19.f);
    CHECK(o.willies[0].partHealth[3] == 12.5f && o.willies[0].dismembered == 16u);
    for (size_t cut = 0; cut < b.size(); cut += 7) CHECK(!Decode(b.data(), cut, o));
    auto bad = b; bad[12] = uint8_t(kMaxWillies + 1); CHECK(!Decode(bad.data(), bad.size(), o));   // willie count byte
}

static void TestEventAndReject() {
    Event e; e.seq = 3; e.kind = EventKind::Dismembered; e.a = 1; e.b = 5; e.text = "x";
    auto b = Encode(e); Event o; CHECK(Decode(b.data(), b.size(), o) && o.kind == EventKind::Dismembered && o.b == 5);
    auto bad = b; bad[8] = 99; CHECK(!Decode(bad.data(), bad.size(), o));
    Reject r; r.reason = RejectReason::BuildMismatch; auto rb = Encode(r); Reject ro;
    CHECK(Decode(rb.data(), rb.size(), ro) && ro.reason == RejectReason::BuildMismatch);
    for (RejectReason k : {RejectReason::Kicked, RejectReason::Banned, RejectReason::WrongPassword, RejectReason::TooManyTries}) {   // rev 20; rev 35
        Reject rk; rk.reason = k; auto kb = Encode(rk); Reject ko;
        CHECK(Decode(kb.data(), kb.size(), ko) && ko.reason == k);
    }
    CHECK(kMaxRejectReason == 11 && uint8_t(RejectReason::WrongPassword) == 7 && uint8_t(RejectReason::TooManyTries) == 8);
    CHECK(uint8_t(RejectReason::ModBuild) == 9 && uint8_t(RejectReason::ModsNotAllowed) == 10 && uint8_t(RejectReason::ModifiedClient) == 11);   // rev 40
    {   // rev 40: a detail line, printable ASCII, at most kMaxRejectDetail bytes (a longer one is cut by the encoder).
        Reject rd; rd.reason = RejectReason::ModsNotAllowed; rd.detail = "This lobby blocks cheats: remove HSEnhancer.";
        auto db = Encode(rd); Reject dout;
        CHECK(Decode(db.data(), db.size(), dout) && dout.reason == RejectReason::ModsNotAllowed && dout.detail == rd.detail);
        for (size_t cut = 0; cut < db.size(); ++cut) CHECK(!Decode(db.data(), cut, dout));
        auto ctl = db; ctl[7] = 0x07; CHECK(!Decode(ctl.data(), ctl.size(), dout));
        rd.detail = std::string(300, 'x'); db = Encode(rd); CHECK(Decode(db.data(), db.size(), dout) && dout.detail.size() == kMaxRejectDetail);
        CHECK(ro.detail.empty());
    }
    { auto bad = rb; bad[4] = 0; CHECK(!Decode(bad.data(), bad.size(), ro)); bad[4] = kMaxRejectReason + 1; CHECK(!Decode(bad.data(), bad.size(), ro)); }
    Welcome w; w.snapshotHz = 0; auto wb = Encode(w); Welcome wo; CHECK(!Decode(wb.data(), wb.size(), wo));
    w.snapshotHz = 30; w.map = "/Game/Maps/Arenas/Map_Arena_Yard"; wb = Encode(w); CHECK(Decode(wb.data(), wb.size(), wo) && wo.map == w.map);
    MsgType t; CHECK(PeekType(rb.data(), rb.size(), t) && t == MsgType::Reject);
    uint8_t junk[3] = {1, 2, 3}; CHECK(!PeekType(junk, 3, t));
}

static void TestQuatAndSeq() {
    Quat16 q = PackQuat(0, 0, -0.7071f, -0.7071f);   // negative w is canonicalized
    float x, y, z, w; UnpackQuat(q, x, y, z, w);
    CHECK(w > 0 && std::fabs(z - 0.7071f) < 1e-3f);
    Quat16 zero = PackQuat(0, 0, 0, 0); UnpackQuat(zero, x, y, z, w); CHECK(w == 1.f);
    Quat16 nanq = PackQuat(std::numeric_limits<float>::quiet_NaN(), 0, 0, 1); UnpackQuat(nanq, x, y, z, w); CHECK(w == 1.f);
    CHECK(SeqNewer(5, 4) && !SeqNewer(4, 5) && SeqNewer(1, 0xFFFFFFF0u) && !SeqNewer(7, 7));
}

static void TestTravel() {
    Travel t; t.seq = 4; t.level = "/Game/Maps/Arenas/Map_Arena_Yard.Map_Arena_Yard"; t.absolute = true; t.options = "";
    t.settings.push_back({"Free Mode Activated", {1}});
    t.settings.push_back({"Free Mode Foes Amount", {3, 0, 0, 0}});
    auto b = Encode(t); Travel o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.seq == 4 && o.level == t.level && o.absolute && o.settings.size() == 2 && o.settings[1].value[0] == 3 && o.settings[0].name == "Free Mode Activated");
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    Travel empty = t; empty.level.clear(); auto eb = Encode(empty); CHECK(!Decode(eb.data(), eb.size(), o));      // level required
    Travel noname = t; noname.settings[0].name.clear(); auto nb = Encode(noname); CHECK(!Decode(nb.data(), nb.size(), o));
    Travel big = t; big.settings[0].value.assign(40, 7); auto bb = Encode(big);                                         // value truncated to limit on encode
    CHECK(Decode(bb.data(), bb.size(), o) && o.settings[0].value.size() == kMaxSettingBytes);
    auto bad = b; bad[4 + 4 + 1 + t.level.size()] = 2; CHECK(!Decode(bad.data(), bad.size(), o));                      // absolute flag must be 0/1
    Ready r; r.travelSeq = 4; r.level = t.level; auto rb = Encode(r); Ready ro;
    CHECK(Decode(rb.data(), rb.size(), ro) && ro.travelSeq == 4 && ro.level == t.level);
    MsgType mt; CHECK(PeekType(b.data(), b.size(), mt) && mt == MsgType::Travel);
}

static void TestSpawnDespawn() {
    Spawn s; s.entity = 3; s.kind = EntityKind::Fighter; s.ownerSlot = 2; s.team = 1; s.blobId = 77; s.x = 120.5f; s.y = -40; s.z = 90; s.yaw = 180;
    auto b = Encode(s); Spawn o;
    CHECK(Decode(b.data(), b.size(), o) && o.entity == 3 && o.kind == EntityKind::Fighter && o.ownerSlot == 2 && o.team == 1 && o.blobId == 77 && o.x == 120.5f && o.yaw == 180);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    Spawn zero = s; zero.entity = 0; auto zb = Encode(zero); CHECK(!Decode(zb.data(), zb.size(), o));
    auto badKind = b; badKind[5] = 9; CHECK(!Decode(badKind.data(), badKind.size(), o));
    Spawn team = s; team.team = 8; auto tb = Encode(team); CHECK(!Decode(tb.data(), tb.size(), o));
    Spawn far = s; far.x = 1e9f; auto fb = Encode(far); CHECK(!Decode(fb.data(), fb.size(), o));
    Spawn ffa = s; ffa.team = -1; auto ab = Encode(ffa); CHECK(Decode(ab.data(), ab.size(), o) && o.team == -1);
    CHECK(o.flags == 0 && Spawn{}.flags == 0);                                  // rev 36: no crossbow unless the host says so
    { Spawn xs = s; xs.flags = kSpawnCrossbow; auto xb = Encode(xs); Spawn y; CHECK(Decode(xb.data(), xb.size(), y) && y.flags == kSpawnCrossbow && y.entity == 3 && y.life == xs.life);
      xs.flags = kSpawnZombie; xb = Encode(xs); CHECK(Decode(xb.data(), xb.size(), y) && y.flags == kSpawnZombie);   // rev 38: a zombie body
      xs.flags = kSpawnCrossbow | kSpawnZombie; xb = Encode(xs); CHECK(Decode(xb.data(), xb.size(), y) && y.flags == 3);
      xs.flags = 4; xb = Encode(xs); CHECK(!Decode(xb.data(), xb.size(), y));    // an unknown flag bit
      xs.flags = 0xFF; xb = Encode(xs); CHECK(!Decode(xb.data(), xb.size(), y)); }
    Despawn d; d.entity = 4; auto db = Encode(d); Despawn od; CHECK(Decode(db.data(), db.size(), od) && od.entity == 4);
    Despawn d0; auto d0b = Encode(d0); CHECK(!Decode(d0b.data(), d0b.size(), od));
}

// Tournament and Pit Bridge Duels: the bracket / queue rides the MatchInfo and Decode checks it holds together.
static void TestQueueModesOnTheWire() {
    MatchInfo base; base.format = uint8_t(Format::Tournament); base.scoreLimit = 2; base.phase = 2; base.round = 3;
    base.fighters = {{1, -1, 0, 0, "ann"}, {2, -1, 0, 0, "bo"}, {3, -1, 0, 0, "cy"}, {4, -1, 0, 0, "di"}};
    // Tournament: a real bracket round-trips, with a game played and a player who left
    Bracket b; CHECK(b.Start({1, 2, 3, 4}, 9));
    Bracket::Game g; CHECK(b.Current(g)); b.Report(g.a);
    MatchInfo t = base; t.queueOrder = b.seeds(); t.bracketWinners = b.GameWinners(); t.fightWinsA = 1; t.fightWinsB = 0;
    auto e = Encode(t); MatchInfo y;
    CHECK(Decode(e.data(), e.size(), y) && y.queueOrder == t.queueOrder && y.bracketWinners == t.bracketWinners && y.fightWinsA == 1 && y.fightWinsB == 0 && y.streak == 0);
    for (size_t cut = 0; cut < e.size(); ++cut) CHECK(!Decode(e.data(), cut, y));
    { MatchInfo l = t; b.Remove(b.seeds()[2]); l.queueOrder = b.seeds(); l.bracketWinners = b.GameWinners(); auto el = Encode(l); MatchInfo yl;
      CHECK(Decode(el.data(), el.size(), yl) && yl.bracketWinners == l.bracketWinners); }
    // an empty bracket (before the first MatchInfo of a match) is fine
    { MatchInfo n = base; auto en = Encode(n); MatchInfo yn; CHECK(Decode(en.data(), en.size(), yn) && yn.queueOrder.empty() && yn.bracketWinners.empty()); }
    auto bad = [&](const MatchInfo& x) { auto ex = Encode(x); MatchInfo yx; return !Decode(ex.data(), ex.size(), yx); };
    { MatchInfo x = t; x.bracketWinners.pop_back(); CHECK(bad(x)); }                          // the wrong number of games
    { MatchInfo x = t; x.bracketWinners[0] = 77; CHECK(bad(x)); }                              // a winner who is not in the game
    { MatchInfo x = t; x.queueOrder[1] = x.queueOrder[0]; CHECK(bad(x)); }                     // a seed twice
    { MatchInfo x = t; x.queueOrder = {1}; x.bracketWinners = {}; CHECK(bad(x)); }             // one seed is no bracket
    { MatchInfo x = t; x.queueOrder.resize(kMaxBracketPlayers + 1, 5); CHECK(bad(x)); }        // too many (Encode cuts at 16: a count of 17 never goes out)
    { MatchInfo x = t; x.streak = 2; CHECK(bad(x)); }                                          // a streak is the Pit's
    { MatchInfo x = t; x.fightWinsA = 4; CHECK(bad(x)); }                                      // past the most rounds a fight has
    // Pit Bridge Duels: the queue and the streak
    MatchInfo p = base; p.format = uint8_t(Format::PitBridgeDuel); p.scoreLimit = 5; p.queueOrder = {3, 1, 4, 2}; p.streak = 3;
    auto ep = Encode(p); MatchInfo yp;
    CHECK(Decode(ep.data(), ep.size(), yp) && yp.queueOrder == p.queueOrder && yp.streak == 3 && yp.bracketWinners.empty());
    for (size_t cut = 0; cut < ep.size(); ++cut) CHECK(!Decode(ep.data(), cut, yp));
    { MatchInfo x = p; x.queueOrder = {3, 1, 3}; CHECK(bad(x)); }                              // a player twice
    { MatchInfo x = p; x.queueOrder = {3, 0}; CHECK(bad(x)); }
    { MatchInfo x = p; x.queueOrder = {}; CHECK(bad(x)); }                                     // a streak with nobody
    { MatchInfo x = p; x.queueOrder = {3, 99}; CHECK(bad(x)); }                                // not an entity
    { MatchInfo x = p; x.bracketWinners = {1}; CHECK(bad(x)); }                                // a bracket's field
    { MatchInfo x = p; x.fightWinsA = 1; CHECK(bad(x)); }
    { MatchInfo x = p; x.streak = 0; auto ex = Encode(x); MatchInfo yx; CHECK(Decode(ex.data(), ex.size(), yx) && yx.streak == 0); }
    // every other format carries none of it
    { MatchInfo x = base; x.format = uint8_t(Format::Duel); x.queueOrder = {1, 2}; CHECK(bad(x)); }
    { MatchInfo x = base; x.format = uint8_t(Format::LastManStanding); x.streak = 1; CHECK(bad(x)); }
    { MatchInfo x = base; x.format = uint8_t(Format::FreeForAll); x.fightWinsB = 1; CHECK(bad(x)); }
}

static void TestMatchInfoAndKill() {
    MatchInfo m; m.format = 2; m.scoreLimit = 12; m.timeLimitS = 300; m.respawnDelayS = 4; m.friendlyFire = true;
    m.phase = 2; m.timeLeftMs = 123456; m.winner = -1000;
    m.scores = {{0, 3}, {1, -2}, {2, 7}};
    m.fighters = {{1, 0, 3, 1, "host"}, {2, 1, 0, 2, "ann"}, {9, 2, 7, 0, "Bot"}};
    CHECK(m.gear == 0 && m.tier == 2 && m.round == 0 && m.roundGapLeftMs == 0 && m.roundWinner == 0 && m.damagePct == 0xFFFF);
    CHECK(m.fighters[0].pingMs == 0 && m.fighters[0].alive);                   // defaults: no ping known, alive
    m.gear = 2; m.tier = 5; m.round = 3; m.roundGapLeftMs = 4200; m.roundWinner = 9;   // rev 13: the rounds plan's fields
    m.fighters[0].pingMs = 87; m.fighters[1].alive = false; m.fighters[2].pingMs = 65535;
    CHECK(!m.fighters[0].down);
    m.fighters[2].down = true;                                                  // rev 17: the draw check has it down
    auto b = Encode(m); MatchInfo o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.gear == 2 && o.tier == 5 && o.round == 3 && o.roundGapLeftMs == 4200 && o.roundWinner == 9);
    CHECK(o.damagePct == 0xFFFF);   // rev 24: stock
    { MatchInfo cw = m; cw.waveBreakS = 120; cw.clearGroundMin = 5; cw.clearCount = 7; auto e = Encode(cw); MatchInfo y;   // rev 28
      CHECK(Decode(e.data(), e.size(), y) && y.waveBreakS == 120 && y.clearGroundMin == 5 && y.clearCount == 7); }
    CHECK(o.waveBreakS == 0 && o.clearGroundMin == 0 && o.clearCount == 0);
    { MatchInfo sm = m; sm.syncMode = 0; auto e = Encode(sm); MatchInfo y; CHECK(Decode(e.data(), e.size(), y) && y.syncMode == 0);   // rev 29
      sm.syncMode = 2; e = Encode(sm); CHECK(!Decode(e.data(), e.size(), y)); }
    CHECK(o.syncMode == 1);                                                   // per player unless the host says so
    CHECK(o.forceStrength == 1 && MatchInfo{}.forceStrength == 1);            // rev 34: builds forced unless the host says so
    { MatchInfo fm = m; fm.forceStrength = 0; auto e = Encode(fm); MatchInfo y; CHECK(Decode(e.data(), e.size(), y) && y.forceStrength == 0);
      fm.forceStrength = 1; e = Encode(fm); CHECK(Decode(e.data(), e.size(), y) && y.forceStrength == 1 && y.syncMode == o.syncMode && y.physics == o.physics);
      fm.forceStrength = 2; e = Encode(fm); CHECK(!Decode(e.data(), e.size(), y));
      fm.forceStrength = 0xFF; e = Encode(fm); CHECK(!Decode(e.data(), e.size(), y)); }
    CHECK(o.teamKit == uint8_t(TeamKit::Full) && MatchInfo{}.teamKit == 2);   // the Team colours row: Full kit unless the host says so
    for (uint8_t k = 0; k < kTeamKitCount; ++k) { MatchInfo km = m; km.teamKit = k; auto e = Encode(km); MatchInfo y;
        CHECK(Decode(e.data(), e.size(), y) && y.teamKit == k && y.forceStrength == m.forceStrength && y.physics == o.physics && y.fighters.size() == m.fighters.size()); }
    CHECK(o.freeCam == 1 && MatchInfo{}.freeCam == 1);                       // rev 36: the dead may fly a free camera unless the host says so
    { MatchInfo cm = m; cm.freeCam = 0; auto e = Encode(cm); MatchInfo y; CHECK(Decode(e.data(), e.size(), y) && y.freeCam == 0 && y.forceStrength == o.forceStrength && y.teamKit == o.teamKit);
      cm.freeCam = 1; e = Encode(cm); CHECK(Decode(e.data(), e.size(), y) && y.freeCam == 1 && y.syncMode == o.syncMode);
      cm.freeCam = 2; e = Encode(cm); CHECK(!Decode(e.data(), e.size(), y));
      cm.freeCam = 0xFF; e = Encode(cm); CHECK(!Decode(e.data(), e.size(), y)); }
    CHECK(MatchInfo{}.crossbows == 0 && o.crossbows == 0);                      // rev 36: the Crossbows row, Off by default
    { MatchInfo cm = m; MatchInfo y;
      for (uint8_t v = 0; v < kCrossbowModeCount; ++v) { cm.crossbows = v; auto e = Encode(cm); CHECK(Decode(e.data(), e.size(), y) && y.crossbows == v && y.forceStrength == o.forceStrength && y.teamKit == o.teamKit && y.freeCam == o.freeCam && y.physics == o.physics); }
      for (uint8_t bad : {uint8_t(kCrossbowModeCount), uint8_t(9), uint8_t(0xFF)}) { cm.crossbows = bad; auto e = Encode(cm); CHECK(!Decode(e.data(), e.size(), y)); } }
    CHECK(o.physics[0] == -1.f && o.physics[1] == -1.f && o.physics[2] == -1.f);   // rev 31: each its own
    { MatchInfo pm = m; pm.physics = { 0.5f, 1.f, 0.f }; auto e = Encode(pm); MatchInfo y;
      CHECK(Decode(e.data(), e.size(), y) && y.physics[0] == 0.5f && y.physics[1] == 1.f && y.physics[2] == 0.f);
      pm.physics[1] = -0.5f; e = Encode(pm); CHECK(!Decode(e.data(), e.size(), y));
      pm.physics[1] = std::numeric_limits<float>::quiet_NaN(); e = Encode(pm); CHECK(!Decode(e.data(), e.size(), y));
      pm.physics[1] = 11.f; e = Encode(pm); CHECK(!Decode(e.data(), e.size(), y)); }
    { MatchInfo dm = m; dm.damagePct = 150; auto e = Encode(dm); MatchInfo y;
      CHECK(Decode(e.data(), e.size(), y) && y.damagePct == 150); }
    { MatchInfo dm = m; dm.damagePct = 1000; auto e = Encode(dm); MatchInfo y;
      CHECK(Decode(e.data(), e.size(), y) && y.damagePct == 1000); }
    CHECK(o.fighters[0].pingMs == 87 && o.fighters[0].alive && !o.fighters[1].alive && o.fighters[1].pingMs == 0);
    CHECK(o.fighters[2].alive && o.fighters[2].pingMs == 65535);
    CHECK(o.fighters[2].down && !o.fighters[0].down && !o.fighters[1].down);
    CHECK(o.fighters[0].steamId == 0 && o.fighters[2].steamId == 0);              // rev 24: bots and non-Steam players carry none
    { MatchInfo sid = m; sid.fighters[0].steamId = 76561198000000001ull; sid.fighters[1].steamId = 0xFFFFFFFFFFFFFFFFull;
      auto e = Encode(sid); MatchInfo y;
      CHECK(Decode(e.data(), e.size(), y) && y.fighters[0].steamId == 76561198000000001ull && y.fighters[1].steamId == 0xFFFFFFFFFFFFFFFFull && y.fighters[2].steamId == 0);
      CHECK(y.fighters[1].name == "ann" && y.fighters[2].entity == 9);
      for (size_t cut = 0; cut < e.size(); ++cut) CHECK(!Decode(e.data(), cut, y)); }
    { MatchInfo dd = m; dd.fighters[2].alive = false; auto e = Encode(dd); MatchInfo y;   // each bit on its own
      CHECK(Decode(e.data(), e.size(), y) && !y.fighters[2].alive && y.fighters[2].down); }
    for (uint8_t g = 0; g < kGearCount; ++g)
        for (uint8_t tier = 0; tier <= 7; ++tier) {
            MatchInfo x = m; x.gear = g; x.tier = tier; x.round = uint16_t(1000 + tier); x.roundGapLeftMs = 0xFFFFFFFFu;
            auto e = Encode(x); MatchInfo y;
            CHECK(Decode(e.data(), e.size(), y) && y.gear == g && y.tier == tier && y.round == 1000 + tier && y.roundGapLeftMs == 0xFFFFFFFFu);
        }
    CHECK(o.format == 2 && o.scoreLimit == 12 && o.timeLimitS == 300 && o.respawnDelayS == 4 && o.friendlyFire && o.phase == 2);
    CHECK(o.timeLeftMs == 123456 && o.winner == -1000 && o.scores.size() == 3 && o.scores[1].score == -2 && o.scores[2].key == 2);
    CHECK(o.fighters.size() == 3 && o.fighters[2].entity == 9 && o.fighters[2].team == 2 && o.fighters[2].kills == 7 && o.fighters[1].deaths == 2 && o.fighters[1].name == "ann");
    auto bad = [&](auto mutate) { MatchInfo x = m; mutate(x); auto e = Encode(x); MatchInfo y; return !Decode(e.data(), e.size(), y); };
    // Every match format the host can run must reach the client (Duel and Zombies were dropped, test 2).
    for (uint8_t f = 0; f < kFormatCount; ++f) {
        MatchInfo x = m; x.format = f; auto e = Encode(x); MatchInfo y;
        CHECK(Decode(e.data(), e.size(), y) && y.format == f);
    }
    CHECK(bad([](MatchInfo& x) { x.format = kFormatCount; }));
    CHECK(bad([](MatchInfo& x) { x.format = 255; }));
    CHECK(bad([](MatchInfo& x) { x.phase = 5; }));
    CHECK(bad([](MatchInfo& x) { x.scoreLimit = 0; }));
    CHECK(bad([](MatchInfo& x) { x.scoreLimit = 101; }));
    CHECK(bad([](MatchInfo& x) { x.timeLimitS = 3601; }));
    CHECK(bad([](MatchInfo& x) { x.respawnDelayS = 61; }));
    // rev 35: the Teams format's team count rides every MatchInfo (2..4; 2 by default, other formats ignore it).
    CHECK(MatchInfo{}.teams == 2 && o.teams == 2);
    for (uint8_t t = 2; t <= 4; ++t) {
        MatchInfo x = m; x.format = uint8_t(Format::TwoTeams); x.teams = t; x.scores = {{0, 1}, {3, 2}}; x.fighters[2].team = int8_t(t - 1);
        auto e = Encode(x); MatchInfo y;
        CHECK(Decode(e.data(), e.size(), y) && y.teams == t && y.format == uint8_t(Format::TwoTeams) && y.fighters[2].team == int8_t(t - 1) && y.scores.size() == 2);
        for (size_t cut = 0; cut < e.size(); ++cut) CHECK(!Decode(e.data(), cut, y));
    }
    { MatchInfo x = m; x.format = uint8_t(Format::LastManStanding); x.teams = 4; auto e = Encode(x); MatchInfo y;   // not a teams format: carried, ignored
      CHECK(Decode(e.data(), e.size(), y) && y.format == uint8_t(Format::LastManStanding) && y.teams == 4); }
    CHECK(bad([](MatchInfo& x) { x.teams = 0; }) && bad([](MatchInfo& x) { x.teams = 1; }) && bad([](MatchInfo& x) { x.teams = 5; }) && bad([](MatchInfo& x) { x.teams = 255; }));
    CHECK(bad([](MatchInfo& x) { x.format = uint8_t(Format::TwoTeams); x.teams = 5; }));
    CHECK(bad([](MatchInfo& x) { x.teamKit = 3; }) && bad([](MatchInfo& x) { x.teamKit = 255; }));   // Team colours: Off, Tabard, Full only
    CHECK(bad([](MatchInfo& x) { x.waveBreakS = 45; }));      // rev 28
    CHECK(bad([](MatchInfo& x) { x.clearGroundMin = 4; }));
    CHECK(bad([](MatchInfo& x) { x.damagePct = 1001; }));   // rev 24: past the most, not the stock marker
    CHECK(bad([](MatchInfo& x) { x.fighters[0].entity = 0; }));
    CHECK(bad([](MatchInfo& x) { x.fighters[0].team = 8; }));
    CHECK(bad([](MatchInfo& x) { x.fighters.resize(kMaxWillies + 1, {1, 0, 0, 0, ""}); }));
    CHECK(bad([](MatchInfo& x) { x.scores.resize(kMaxWillies + 1); }));
    { MatchInfo x = m; x.format = uint8_t(Format::FreeForAll); x.gear = uint8_t(Gear::Fists); x.tier = 0;   // rev 21: Brawl
      auto e = Encode(x); MatchInfo y; CHECK(Decode(e.data(), e.size(), y) && y.gear == 3 && y.format == 0 && y.tier == 0); }
    CHECK(kGearCount == 4 && uint8_t(Gear::Fists) == kGearCount - 1);          // the last accepted gear is Fists; an older build's range ended at 2
    CHECK(bad([](MatchInfo& x) { x.gear = kGearCount; }));
    CHECK(bad([](MatchInfo& x) { x.tier = 8; }));
    { auto e = Encode(m); uint8_t& al = e[e.size() - 16]; CHECK(al == 3 && e[e.size() - 7] == 0 && e.back() == 0); al = 4; MatchInfo y; CHECK(!Decode(e.data(), e.size(), y)); }   // alive: bits 0-1 only (the last fighter's alive byte, before its steamId and role, the objective count and the queue fields: 2 counts and 3 bytes)
    { auto e = Encode(m); e.pop_back(); MatchInfo y; CHECK(!Decode(e.data(), e.size(), y)); }                        // truncated
    b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));                  // trailing byte
    Kill k; k.killer = 3; k.victim = 4; k.flags = 1;
    auto kb = Encode(k); Kill ko; CHECK(Decode(kb.data(), kb.size(), ko) && ko.killer == 3 && ko.victim == 4 && ko.flags == 1);
    k.victim = 0; kb = Encode(k); CHECK(!Decode(kb.data(), kb.size(), ko));   // victim required
    // Spec revision (rev 17): flag 8 is "gave up" (Duel, held G); 16 and 32 a drawn round; 64 is the first unknown bit.
    k.victim = 4; k.flags = 8; kb = Encode(k); CHECK(Decode(kb.data(), kb.size(), ko) && ko.flags == 8);
    k.flags = 16; kb = Encode(k); CHECK(Decode(kb.data(), kb.size(), ko) && ko.flags == 16);
    k.flags = 63; kb = Encode(k); CHECK(Decode(kb.data(), kb.size(), ko) && ko.flags == 63);
    k.flags = 64; kb = Encode(k); CHECK(!Decode(kb.data(), kb.size(), ko));
    CHECK(kMaxWillies >= 18 && kProtocolRevision == 41);   // rev 41: ProgressionResult 69 (next-round and time-step flags), Day Count in travels; rev 40: ModList, SessionMods, ModChallenge, ModAnswer, Reject.detail, RejectReason 9-11; rev 39: Tag bare-fisted, one-round Tournament fights; rev 38: Spawn kSpawnZombie, HitClaim source Ammo; rev 37: formats 10-15, CtfEvent 62, ModeEvent 63, ProgressionVote 66, MatchInfo.tagIt/fuseLeftMs/objectives/queueOrder/bracketWinners/fightWinsA/fightWinsB/streak; rev 36: FighterEntry.role, Format TeamDuel/Riot/Juggernaut, MatchInfo.teamKit/freeCam/crossbows, Spawn.flags, LoadoutChange; rev 35: GroundClear, LobbySecret, Hello.password, MatchInfo.teams, Format::LastManStanding; rev 34: MatchInfo.forceStrength; rev 33: Input/RelayInput toggle counts and lock-on target; rev 32: HandOff, HandOffAsk, Pose/Vitals epoch; rev 31: MatchInfo.physics; rev 25: HitClaim Stuck + rawDamage; rev 24: Voice flags NoCut, Everyone (voice per player); rev 23: Voice ADPCM flag, VoiceMode, kWelcomeVoiceAll, RouteTable, Forward; rev 22: 8 players, PingTable, kWelcomePeerPing; rev 21: MatchInfo.gear Fists (Brawl); rev 20: Reject Kicked/Banned, server chat lines; rev 19: Spawn.life, victimLife, PeerMap, MeshHeard, Welcome.flags; rev 18: ReportShare; rev 17: drawn rounds, flail head, mod build in Hello and Welcome, fighters down in MatchInfo; rev 16: GiveUp; rev 15: PaintFx Voice, HitSound, Armor, ArmorOn; rev 14: pose codec v2 (grip state); rev 13: prop HitClaims, MatchInfo rounds/gear/ping/alive; rev 12: SeverNotice, PropCensus
    // rev 9: Welcome says who simulates the fighters (0 host, 1 owner); nothing else is a mode
    Welcome sm; sm.snapshotHz = 60; sm.map = "m"; Welcome smo;
    CHECK(sm.syncMode == 0);                                                   // host authority unless the host says so
    auto smb = Encode(sm); CHECK(Decode(smb.data(), smb.size(), smo) && smo.syncMode == 0);
    sm.syncMode = 1; smb = Encode(sm); CHECK(Decode(smb.data(), smb.size(), smo) && smo.syncMode == 1 && smo.map == "m" && smo.snapshotHz == 60);
    sm.modBuild = "0.6.4"; smb = Encode(sm);   // rev 17: Welcome carries the host's mod build
    CHECK(Decode(smb.data(), smb.size(), smo) && smo.modBuild == "0.6.4" && smo.syncMode == 1);
    for (size_t cut = 0; cut < smb.size(); ++cut) CHECK(!Decode(smb.data(), cut, smo));
    sm.modBuild.clear();
    sm.syncMode = 2; smb = Encode(sm); CHECK(!Decode(smb.data(), smb.size(), smo));
    sm.syncMode = 0xFF; smb = Encode(sm); CHECK(!Decode(smb.data(), smb.size(), smo));
    sm.syncMode = 1; smb = Encode(sm); smb.pop_back(); CHECK(!Decode(smb.data(), smb.size(), smo));   // the mode byte is required
    // Reserved for M5-M6: numbers fixed, no decoder (ModList and SessionMods have one from rev 40, session/hsm_mod_rules.h). A Welcome with the reserved flag 32 is rejected until M4 lands.
    CHECK(uint8_t(MsgType::ModList) == 48 && uint8_t(MsgType::SessionMods) == 49 && uint8_t(MsgType::ModMsg) == 50
          && uint8_t(MsgType::PackList) == 51 && uint8_t(MsgType::PackStatus) == 52 && uint8_t(MsgType::ModMap) == 53 && uint8_t(MsgType::MapReset) == 54 && uint8_t(MsgType::LevelSeed) == 55 && uint8_t(MsgType::RemoteWrite) == 56 && uint8_t(MsgType::HandOff) == 57 && uint8_t(MsgType::HandOffAsk) == 58 && uint8_t(MsgType::GroundClear) == 59 && uint8_t(MsgType::LobbySecret) == 60 && uint8_t(MsgType::LoadoutChange) == 61 && uint8_t(MsgType::CtfEvent) == 62 && uint8_t(MsgType::ModeEvent) == 63 && uint8_t(MsgType::ProgressionVote) == 66 && uint8_t(MsgType::ModChallenge) == 67 && uint8_t(MsgType::ModAnswer) == 68 && uint8_t(MsgType::ProgressionResult) == 69 && kLastDecodedMsgType == 69 && kLastReservedMsgType == 69);
    {
        Welcome w; w.snapshotHz = 30; w.map = "m"; w.flags = 32;
        auto wb = Encode(w); Welcome back; CHECK(!Decode(wb.data(), wb.size(), back));
    }
    CHECK(uint8_t(MsgType::Pose) == 22 && uint8_t(MsgType::Vitals) == 23 && uint8_t(MsgType::HitClaim) == 24 && uint8_t(MsgType::HitFx) == 25);
}

static void TestRelayInput() {
    RelayInput r; r.entity = 3; r.frame.seq = 77; r.frame.clientTimeMs = 9; r.frame.axes[0] = 1.f; r.frame.buttons = 5; r.frame.controlYaw = 90.f;
    std::vector<uint8_t> b = Encode(r);
    RelayInput back;
    CHECK(Decode(b.data(), b.size(), back) && back.entity == 3 && back.frame.seq == 77 && back.frame.buttons == 5 && back.frame.controlYaw == 90.f);
    for (size_t n = 0; n < b.size(); ++n) CHECK(!Decode(b.data(), n, back));
    RelayInput z = r; z.entity = 0; std::vector<uint8_t> bz = Encode(z); CHECK(!Decode(bz.data(), bz.size(), back));
    RelayInput big = r; big.entity = uint8_t(kMaxWillies + 1); std::vector<uint8_t> bb = Encode(big); CHECK(!Decode(bb.data(), bb.size(), back));
    RelayInput nan = r; nan.frame.axes[1] = NAN; std::vector<uint8_t> bn = Encode(nan); CHECK(!Decode(bn.data(), bn.size(), back));
    InputFrame f; CHECK(!Decode(b.data(), b.size(), f));                   // not an Input message
}

static void TestOwnerStateAndDeath() {
    OwnerState o; o.state.entity = 2; o.state.health = 80; o.state.flags = 1; o.state.bodies.resize(23);
    o.state.bodies[5].px = 12.5f;
    std::vector<uint8_t> b = Encode(o);
    OwnerState back;
    CHECK(Decode(b.data(), b.size(), back) && back.state.entity == 2 && back.state.bodies.size() == 23 && back.state.bodies[5].px == 12.5f);
    for (size_t n = 0; n < b.size(); ++n) CHECK(!Decode(b.data(), n, back));
    OwnerState z = o; z.state.entity = 0; std::vector<uint8_t> bz = Encode(z); CHECK(!Decode(bz.data(), bz.size(), back));
    OwnerState nan = o; nan.state.health = NAN; std::vector<uint8_t> bn = Encode(nan); CHECK(!Decode(bn.data(), bn.size(), back));
    DeathReport d; d.victim = 2; d.killer = 1;
    std::vector<uint8_t> db = Encode(d);
    DeathReport dback;
    CHECK(Decode(db.data(), db.size(), dback) && dback.victim == 2 && dback.killer == 1);
    for (size_t n = 0; n < db.size(); ++n) CHECK(!Decode(db.data(), n, dback));
    DeathReport env = d; env.killer = 0; std::vector<uint8_t> eb = Encode(env); CHECK(Decode(eb.data(), eb.size(), dback) && dback.killer == 0);
    DeathReport bad = d; bad.victim = 0; std::vector<uint8_t> bb = Encode(bad); CHECK(!Decode(bb.data(), bb.size(), dback));
    DeathReport big = d; big.killer = uint8_t(kMaxWillies + 1); std::vector<uint8_t> gb = Encode(big); CHECK(!Decode(gb.data(), gb.size(), dback));
    GiveUpMsg g; g.entity = 3;
    std::vector<uint8_t> ub = Encode(g);
    GiveUpMsg gback; MsgType gt;
    CHECK(Decode(ub.data(), ub.size(), gback) && gback.entity == 3 && PeekType(ub.data(), ub.size(), gt) && gt == MsgType::GiveUp);
    for (size_t n = 0; n < ub.size(); ++n) CHECK(!Decode(ub.data(), n, gback));
    std::vector<uint8_t> ul = ub; ul.push_back(0); CHECK(!Decode(ul.data(), ul.size(), gback));   // trailing byte
    for (uint8_t e : {uint8_t(0), uint8_t(kMaxWillies + 1)}) { GiveUpMsg x; x.entity = e; std::vector<uint8_t> xb = Encode(x); CHECK(!Decode(xb.data(), xb.size(), gback)); }
    CHECK(!Decode(db.data(), db.size(), gback));                                // another message's bytes
}

int main() {
    TestOwnerStateAndDeath();
    TestRelayInput();
    TestMatchInfoAndKill();
    TestQueueModesOnTheWire();
    TestTravel();
    TestSpawnDespawn();
    TestHello(); TestInput(); TestSnapshot(); TestEventAndReject(); TestQuatAndSeq();
    std::printf(g_fail ? "%d FAILED\n" : "all protocol tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
