// Deterministic fuzz harness for every hsm_protocol decoder (run under ASan/UBSan by run_fuzz.sh).
// Properties: no decoder reads out of bounds or invokes UB on arbitrary bytes; whatever decodes successfully
// re-encodes to bytes that decode to an identical encoding (canonical round trip).
// -DHSM_FUZZ_NEGATIVE_CONTROL adds a deliberately broken decoder so the gate can prove it detects bugs.
#include "combat/hsm_equip.h"
#include "combat/hsm_fx.h"
#include "combat/hsm_sever.h"
#include "hsm_owner.h"
#include "hsm_protocol.h"
#include "match/hsm_ctf.h"
#include "session/hsm_career.h"
#include "session/hsm_dice.h"
#include "session/hsm_lease.h"
#include "session/hsm_loadout_change.h"
#include "session/hsm_ground_clear.h"
#include "session/hsm_map_reset.h"
#include "session/hsm_mod_challenge.h"
#include "session/hsm_mod_rules.h"
#include "session/hsm_modmaps.h"
#include "session/hsm_object_event.h"
#include "session/hsm_password.h"
#include "session/hsm_progvote.h"
#include "session/hsm_prop_census.h"
#include "session/hsm_report.h"
#include "sync/hsm_grab.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace hsm;

namespace {
uint64_t g_state = 0x9E3779B97F4A7C15ull;
uint64_t Rand() { g_state ^= g_state << 13; g_state ^= g_state >> 7; g_state ^= g_state << 17; return g_state; }
size_t Below(size_t n) { return n ? size_t(Rand() % n) : 0; }

int g_roundTripFailures = 0;

template <class M>
void Try(const uint8_t* d, size_t n) {
    M m;
    if (!Decode(d, n, m)) return;
    std::vector<uint8_t> again = Encode(m);
    M m2;
    if (!Decode(again.data(), again.size(), m2) || Encode(m2) != again) {
        if (++g_roundTripFailures <= 5) std::printf("round-trip mismatch for message type %u (size %zu)\n", unsigned(d[3]), n);
    }
}

// The pak index reader on arbitrary bytes: no over-read, and whatever it returns stays inside its caps.
void FuzzPak(const uint8_t* d, size_t n) {
    PakFooter f; PakPrimary pr; std::vector<std::string> maps;
    ParsePakFooter(d, n, uint64_t(n) * (1 + (n & 3)), f);
    ParsePakPrimaryIndex(d, n, uint64_t(n) + 4096, pr);
    if (ParsePakDirectoryIndex(d, n, "../../../", maps) == PakParse::Ok && maps.size() > kMaxPakMaps) { std::printf("pak maps over the cap\n"); ++g_roundTripFailures; }
    HashCache c; c.Parse(std::string_view(reinterpret_cast<const char*>(d), n));
    HashCache c2; c2.Parse(c.Serialize());
    if (c2.Serialize() != c.Serialize()) { std::printf("hash cache round trip\n"); ++g_roundTripFailures; }
}

void TryAll(const uint8_t* d, size_t n) {
    Try<Hello>(d, n); Try<Welcome>(d, n); Try<Reject>(d, n); Try<InputFrame>(d, n); Try<Snapshot>(d, n);
    Try<Event>(d, n); Try<Travel>(d, n); Try<Ready>(d, n); Try<Spawn>(d, n); Try<Despawn>(d, n); Try<BlobChunk>(d, n);
    Try<MatchInfo>(d, n); Try<Kill>(d, n);
    Try<PoseMsg>(d, n); Try<VitalsMsg>(d, n); Try<HitClaimMsg>(d, n); Try<HitFxMsg>(d, n);
    PoseMsg pose; sync::PoseFrame frame;
    const bool admitted = DecodePoseEnvelope(d, n, pose);
    const bool decoded = DecodePoseFrame(d, n, pose, frame);
    if ((decoded && !admitted) || !pose.pose.empty()) {
        if (++g_roundTripFailures <= 5) std::printf("pose admission/frame contract failed (size %zu)\n", n);
    }
    Try<combat::SeverClaimMsg>(d, n); Try<combat::SeverNoticeMsg>(d, n); Try<combat::StuckBladeMsg>(d, n);
    Try<GrabStateMsg>(d, n); Try<BodyStateMsg>(d, n); Try<PropLeaseMsg>(d, n);
    Try<combat::PaintFxMsg>(d, n); Try<combat::EquipStateMsg>(d, n); Try<PropCensusMsg>(d, n); Try<GiveUpMsg>(d, n); Try<ReportShareMsg>(d, n); Try<PeerMapMsg>(d, n); Try<MeshHeardMsg>(d, n); Try<PingTableMsg>(d, n); Try<VoiceMsg>(d, n); Try<VoiceModeMsg>(d, n); Try<RouteTableMsg>(d, n); Try<ForwardMsg>(d, n); Try<VoteStateMsg>(d, n); Try<DeathReport>(d, n);
    Try<ObjectAnnounceMsg>(d, n); Try<ObjectEventMsg>(d, n); Try<ModMapMsg>(d, n); Try<dice::LevelSeedMsg>(d, n); Try<MapResetMsg>(d, n); Try<GroundClearMsg>(d, n); Try<LobbySecretMsg>(d, n); Try<LoadoutChangeMsg>(d, n); Try<LoadoutChangeResult>(d, n); Try<CtfEventMsg>(d, n); Try<ModeEventMsg>(d, n); Try<ProgVoteMsg>(d, n);
    Try<ModListMsg>(d, n); Try<SessionModsMsg>(d, n); Try<ModChallengeMsg>(d, n); Try<ModAnswerMsg>(d, n); Try<ProgResultMsg>(d, n);
    FuzzPak(d, n);
    Ping p; bool pong; DecodePing(d, n, p, pong);
    MsgType t; PeekType(d, n, t);
#ifdef HSM_FUZZ_NEGATIVE_CONTROL
    // Known-bad decoder: trusts a length byte from the packet and reads past the end of the buffer.
    if (n >= 5) { volatile uint8_t sink = 0; size_t len = d[4]; for (size_t i = 0; i < len + 8; ++i) sink ^= d[5 + i]; (void)sink; }
#endif
}

std::vector<std::vector<uint8_t>> Seeds() {
    std::vector<std::vector<uint8_t>> s;
    Hello h; h.buildKey = 0x8EFA122909712000ull; h.name = "seed"; h.modBuild = "0.6.4 (v0.6.3-12-gdb82570)"; s.push_back(Encode(h));
    h.password = "hunter2 hunter2"; s.push_back(Encode(h));                    // a locked lobby's Hello
    s.push_back(Encode(LobbySecretMsg{"hunter2 hunter2"})); s.push_back(Encode(LobbySecretMsg{}));   // the host's password hand-out, and its clear
    s.push_back(Encode(LobbySecretMsg{std::string(kMaxLobbyPassword, 'p')}));
    { LoadoutChangeMsg lc; lc.chunk.blobId = 0x01000001u; lc.chunk.total = 5; lc.chunk.count = 1; lc.chunk.crc = 7; lc.chunk.data = {1, 2, 3, 4, 5}; s.push_back(Encode(lc)); s.push_back(Encode(LoadoutChangeResult{LoadoutVerdict::AtNextRespawn})); }   // the Esc menu's CHANGE LOADOUT
    { ProgVoteMsg pv; pv.seq = 3; pv.secondsLeft = 20; pv.mine = 1; pv.voters = 3; pv.voted = 2; pv.options = {{"The Pit", 1}, {"Old Yard", 1}, {"Tourney", 0}}; s.push_back(Encode(pv));   // the Tavern's next-fight vote: state, a pick, the result
      ProgVoteMsg pk; pk.kind = ProgVoteKind::Pick; pk.seq = 3; pk.pick = 2; s.push_back(Encode(pk));
      ProgVoteMsg pr; pr.kind = ProgVoteKind::Result; pr.seq = 3; pr.winner = 1; pr.how = ProgWin::Seed; pr.name = "Old Yard"; s.push_back(Encode(pr)); }
    { ProgResultMsg rw; rw.fight = 41; rw.flags = kResultWon | kResultTourney; rw.purse = 240; s.push_back(Encode(rw));   // rev 41: a co-op fight's end
      ProgResultMsg rl; rl.fight = 42; s.push_back(Encode(rl));
      ProgResultMsg rn; rn.fight = 43; rn.flags = kResultWon | kResultNextRound | kResultTimeStep; s.push_back(Encode(rn)); }   // a round won, more to come
    { Spawn xs; xs.entity = 2; xs.flags = kSpawnCrossbow; s.push_back(Encode(xs)); }
    s.push_back(Encode(ModeEventMsg{ModeEventKind::TagPass, 1, 2})); s.push_back(Encode(ModeEventMsg{ModeEventKind::TagBurn, 3, 0})); s.push_back(Encode(ModeEventMsg{ModeEventKind::Infected, 0, 4}));
    { MatchInfo mt; mt.format = uint8_t(Format::Tag); mt.tagIt = 2; mt.fuseLeftMs = 12000; mt.fighters.resize(2); mt.fighters[0].entity = 1; mt.fighters[1].entity = 2; s.push_back(Encode(mt)); }                                                  // a body given a crossbow
    Welcome w; w.map = "/Game/Maps/Arenas/Map_Arena_Yard"; w.modBuild = "0.6.4"; w.flags = kWelcomeMesh; s.push_back(Encode(w));
    Reject r; r.reason = RejectReason::Full; s.push_back(Encode(r));
    r.reason = RejectReason::Banned; s.push_back(Encode(r));
    r.reason = RejectReason::ModsNotAllowed; r.detail = "This lobby blocks cheats: remove HSEnhancer."; s.push_back(Encode(r));
    {   // rev 40: mod rules and challenge-response
        ModListMsg ml; ml.official = Official::Yes; ml.report.ue4ss = true; ml.report.complete = true;
        ModItem a; a.kind = ModKind::MapPak; a.name = "Tavern.pak"; a.hashed = true; a.sha[0] = 1; ml.report.items.push_back(a);
        ModItem b; b.kind = ModKind::Module; b.name = "HSEnhancer"; ml.report.items.push_back(b);
        s.push_back(Encode(ml));
        ModReport other = ml.report;
        SessionModsMsg sm = BuildSessionMods({{0, &ml.report}, {2, &other}}); s.push_back(Encode(sm));
        ModChallengeMsg mc; mc.id = 7; mc.nonce[0] = 9; mc.ranges = {{16, 4096}, {9000, 100}}; s.push_back(Encode(mc));
        ModAnswerMsg ma; ma.id = 7; ma.digest[3] = 4; s.push_back(Encode(ma));
        ma.status = AnswerStatus::Unable; s.push_back(Encode(ma));
    }
    InputFrame f; f.seq = 7; f.axes[0] = 1; f.buttons = 5; s.push_back(Encode(f));
    Snapshot sn; WillieState ws; ws.entity = 1; ws.bodies.resize(23); sn.willies.push_back(ws); s.push_back(Encode(sn));
    Event e; e.kind = EventKind::Died; e.a = 2; s.push_back(Encode(e));
    Travel t; t.level = "/Game/Maps/Map_Hub_Tavern_Frank"; t.settings.push_back({"Free Mode Activated", {1}}); s.push_back(Encode(t));
    Ready rd; rd.level = "x"; s.push_back(Encode(rd));
    s.push_back(EncodePing(5, false));
    Spawn sp; sp.entity = 2; sp.blobId = 9; sp.x = 10; sp.life = 3; s.push_back(Encode(sp));
    PeerMapMsg pmm; pmm.peers = {{1, 76561198000000001ull}, {2, 76561198000000002ull}}; s.push_back(Encode(pmm));
    MeshHeardMsg mh; mh.mask = 0x06; s.push_back(Encode(mh));
    PingTableMsg pt; pt.entries = {{0, 40, 38, 0}, {2, 95, 0xFFFF, 12}}; s.push_back(Encode(pt));
    VoiceMsg vo; vo.entity = 4; vo.seq = 77; vo.flags = 1; vo.data = {9, 8, 7, 6, 5, 4}; s.push_back(Encode(vo));
    vo.flags = kVoiceSpurt | kVoiceAdpcm; s.push_back(Encode(vo));
    VoiceModeMsg vmm; vmm.reach = 2; s.push_back(Encode(vmm));
    VoteStateMsg vs; vs.target = 3; vs.yes = 2; vs.no = 1; vs.needed = 3; vs.voters = 4; vs.secondsLeft = 20; vs.mine = 1; s.push_back(Encode(vs));
    s.push_back(Encode(VoteStateMsg{}));
    RouteTableMsg rt; rt.epoch = 3; rt.keys.resize(2); rt.keys[0].peer = 2; rt.keys[0].via = 3; rt.keys[0].key[5] = 0x55;
    rt.keys[1].peer = 4; rt.keys[1].via = kRouteViaDirect; rt.relays = {{2, 5}}; s.push_back(Encode(rt));
    ForwardMsg fw; fw.from = 1; fw.to = 2; fw.epoch = 3; fw.seq = 9; fw.tag = 0xFEEDFACEull; fw.payload = Encode(vo);
    fw.payload[3] = uint8_t(MsgType::Pose); s.push_back(Encode(fw));
    DeathReport dr; dr.victim = 2; dr.killer = 1; dr.victimLife = 3; s.push_back(Encode(dr));
    Despawn dp; dp.entity = 2; s.push_back(Encode(dp));
    BlobChunk bc; bc.blobId = 9; bc.total = 300; bc.count = 1; bc.crc = 1; bc.data.assign(300, 7); s.push_back(Encode(bc));
    MatchInfo mi; mi.format = 1; mi.teams = 4; mi.scoreLimit = 10; mi.phase = 2; mi.scores = {{0, 3}, {1, 4}}; mi.fighters = {{1, 0, 2, 1, "a"}, {2, 1, 0, 3, "bb"}}; s.push_back(Encode(mi));
    mi.crossbows = 3; s.push_back(Encode(mi));                                  // the Crossbows row: Half
    { MatchInfo tn = mi; tn.format = uint8_t(Format::Tournament); tn.crossbows = 0; Bracket br; br.Start({1, 2, 3, 4, 5}, 3);   // a Tournament: the bracket
      Bracket::Game g; if (br.Current(g)) br.Report(g.a);
      tn.queueOrder = br.seeds(); tn.bracketWinners = br.GameWinners(); tn.fightWinsA = 1; s.push_back(Encode(tn)); }
    { MatchInfo pb = mi; pb.format = uint8_t(Format::PitBridgeDuel); pb.crossbows = 0; pb.queueOrder = {2, 1}; pb.streak = 2; s.push_back(Encode(pb)); }   // Pit Bridge Duels: the queue
    { MatchInfo rt = mi; rt.format = uint8_t(Format::Riot); rt.teamKit = uint8_t(TeamKit::Tabard); rt.freeCam = 0; rt.crossbows = 4;       // a Riot: roles on the wire, the kit, camera and crossbow bytes
      rt.fighters = {{1, 0, 0, 0, "k", 10, true, false, 7, kRoleKnight}, {2, 1, 0, 0, "r", 20, true, false, 8, uint8_t(kRoleRioterLast)}}; s.push_back(Encode(rt)); }
    { MatchInfo ct = mi; ct.format = uint8_t(Format::CaptureTheFlag); ct.teams = 2;                                                  // a CTF match: both flags on the wire
      Objective carried; carried.owner = 0; carried.state = ObjectiveState::Carried; carried.carrier = 2; carried.x = 100; carried.y = -50; carried.z = 30;
      Objective lying; lying.owner = 1; lying.state = ObjectiveState::Dropped; lying.timerMs = 14000; lying.x = 5; lying.y = 6; lying.z = 7;
      ct.objectives = {carried, lying}; s.push_back(Encode(ct)); }
    s.push_back(Encode(CtfEventMsg{{CtfEventKind::Captured, 1, 2}})); s.push_back(Encode(CtfEventMsg{{CtfEventKind::Expired, 0, 0}}));
    MatchInfo md; md.format = 4; md.scoreLimit = 3; md.phase = 2; md.gear = 3; md.tier = 4; md.round = 2; md.roundGapLeftMs = 3000; md.roundWinner = 1;
    md.scores = {{1, 1}}; md.fighters = {{1, -1, 1, 0, "a", 40, true}, {2, -1, 0, 1, "bb", 120, false}}; s.push_back(Encode(md));
    Kill kl; kl.killer = 1; kl.victim = 2; s.push_back(Encode(kl));
    sync::PoseFrame pf; pf.tsMs = 5000; pf.stepUs = 16667;
    for (int b = 0; b < sync::kPoseBones; ++b) { pf.bones[size_t(b)].pos = sync::V3{ 10.0 * b, 5, 90 }; pf.bones[size_t(b)].rot = sync::Q4{ 0, 0, 0, 1 }; }
    pf.weapons[0].present = true; pf.weapons[0].classHash = 7; pf.weapons[0].rot = sync::Q4{ 0, 0, std::sin(0.2), std::cos(0.2) };
    pf.weapons[0].hasHead = true; pf.weapons[0].head.pos = sync::V3{ 60, 5, 40 }; pf.weapons[0].head.vel = sync::V3{ 300, 0, -200 };   // a flail
    PoseMsg pm; pm.entity = 2; pm.rootX = 1; pm.rootZ = 90; pm.yawDeg = 30; pm.pose = sync::EncodePose(pf); s.push_back(Encode(pm));
    VitalsMsg vm; vm.entity = 2; vm.flags = 2; vm.health = 50; vm.partHealth[3] = 9; vm.severed = 4; s.push_back(Encode(vm));
    combat::HitClaim hc; hc.claimId = 3; hc.victimLife = 2; hc.attacker = 1; hc.victim = 2; hc.hitBone = "head"; hc.strikeComponent = "Blade";
    hc.source = combat::StrikeSource::WeaponRight; hc.hasHitBox = true; hc.hitBoxExtent = sync::V3{ 5, 5, 5 };
    s.push_back(Encode(HitClaimMsg{ hc })); s.push_back(Encode(HitFxMsg{ hc }));
    combat::HitClaim rock = hc;
    combat::StruckByProp(rock, MakeLevelPropId(14), sync::V3{ 300, 0, -50 }, combat::Xform{ {}, sync::Q4{ 0, 0, 0, 1 }, { 1, 1, 1 } });
    s.push_back(Encode(HitClaimMsg{ rock })); s.push_back(Encode(HitFxMsg{ rock }));
    combat::HitClaim stab = hc; stab.striker = combat::StrikerKind::Stuck; stab.rawDamage = 3500; stab.hitBone = "spine_03";
    s.push_back(Encode(HitClaimMsg{ stab })); s.push_back(Encode(HitFxMsg{ stab }));
    combat::SeverClaim sc; sc.claimId = 4; sc.victimLife = 2; sc.attacker = 1; sc.victim = 2; sc.part = 9; sc.masterMesh = "CharacterMesh0";
    sc.attachMarker = "DM Sphere Thigh_R 2"; sc.markers = {"DM Sphere Thigh_R 2", "DM Sphere Thigh_R 3"}; sc.frameBone = "thigh_r";
    sc.box1.local.rot = sync::Q4{ 0, 0, 0, 1 }; sc.box1.extent = sync::V3{ 100, 500, 100 }; sc.box1.local.pos = sync::V3{ 0, 500, 0 };
    sc.box2 = sc.box1; sc.box2.local.pos = sync::V3{ 0, -500, 0 }; sc.source = combat::StrikeSource::WeaponRight; sc.weaponComponent = "Blade";
    s.push_back(Encode(combat::SeverClaimMsg{ sc }));
    combat::SeverClaim own = sc; own.attacker = own.victim; s.push_back(Encode(combat::SeverNoticeMsg{ own }));
    combat::StuckBladeMsg sb; sb.attacker = 1; sb.victim = 2; sb.stuck = true; sb.seq = 3; sb.bone = "spine_03"; sb.weaponComponent = "Blade";
    sb.weaponInBone.rot = sync::Q4{ 0, 0, 0, 1 }; s.push_back(Encode(sb));
    sb.stuck = false; sb.bone = ""; s.push_back(Encode(sb));
    GrabStateMsg gm; gm.grabber = 1; gm.seq = 4; gm.hands[0].held = true; gm.hands[0].target = 2; gm.hands[0].grabId = 9;
    gm.hands[0].bone = "lowerarm_l"; gm.hands[0].grip = sync::V3{ 1.5, -2.25, 10 }; s.push_back(Encode(gm));
    BodyStateMsg ps; ps.objId = MakeDropPropId(3, 1, 5); ps.lessee = 2; ps.senderTimeMs = 77; ps.px = 120; ps.pz = 40;
    ps.rot = PackQuat(0, 0, 0.3f, 0.95f); ps.vx = 300; ps.wz = 720; s.push_back(Encode(ps));
    PropLeaseMsg pl; pl.propId = ps.objId; pl.action = LeaseAction::Grant; pl.level = LeaseLevel::Touch; pl.slot = 2; s.push_back(Encode(pl));
    PropLeaseMsg lv; lv.propId = MakeLevelPropId(3); lv.action = LeaseAction::Request; lv.level = LeaseLevel::Hold; lv.slot = 1; s.push_back(Encode(lv));
    BodyStateMsg pe = ps; pe.objId = MakeDropPropId(uint8_t(kMaxWillies), 0, 127, 0xFFFFFFFFu); s.push_back(Encode(pe));   // the top drop epoch
    PropLeaseMsg le = pl; le.propId = pe.objId; le.action = LeaseAction::Request; le.level = LeaseLevel::Hold; s.push_back(Encode(le));
    ObjectAnnounceMsg oa; oa.objId = MakeSpawnedId(2, 1); oa.classHash = 0xDEADBEEFu; oa.px = 10; oa.py = -4; oa.pz = 55; oa.bodyCount = 3; oa.parentId = MakeLevelPropId(2);
    s.push_back(Encode(oa));
    oa.flags = kAnnounceCanonical; oa.objId = MakeSpawnedId(0, 9); oa.proposalId = MakeSpawnedId(2, 1); s.push_back(Encode(oa));
    BodyStateMsg bs = pe; bs.objId = MakeSpawnedId(uint8_t(kMaxFighters - 1), 0xFFFFFFFFu); bs.body = kMaxObjectBodies - 1; s.push_back(Encode(bs));
    ObjectEventMsg ev; ev.objId = MakeSpawnedId(2, 1); ev.body = 2; ev.kind = ObjectEventKind::JointBroke; ev.constraint = 5; s.push_back(Encode(ev));
    ev.kind = ObjectEventKind::Detached; ev.otherId = MakeSpawnedId(2, 2); s.push_back(Encode(ev));
    ev.kind = ObjectEventKind::Attached; ev.otherId = MakeLevelPropId(7); s.push_back(Encode(ev));
    ev.kind = ObjectEventKind::Destroyed; s.push_back(Encode(ev));
    ev.kind = ObjectEventKind::Custom; ev.modId = 77; ev.data = {1, 2, 3}; s.push_back(Encode(ev));
    ev.data.assign(kMaxCustomEventBytes, 0x5A); s.push_back(Encode(ev));   // the longest custom payload
    ev.kind = ObjectEventKind::Fracture; ev.data.clear(); ev.modId = 0; ev.item = 41; ev.x = 120.5f; ev.y = -3400.f; ev.z = 88.f;
    ev.radius = 75.f; ev.strain = 250000.f; s.push_back(Encode(ev));
    ev.item = kMaxFractureItem; ev.x = kMaxPropCoord; ev.radius = kMaxFractureRadius; ev.strain = kMaxFractureStrain; s.push_back(Encode(ev));
    ModMapMsg mm; mm.path = "/Game/Maps/Custom/Arena_Bog"; mm.display = "Arena Bog"; mm.pakName = "BogMap_P.pak"; mm.pakSize = 123456789;
    for (size_t i = 0; i < mm.sha.size(); ++i) mm.sha[i] = uint8_t(i + 1);
    s.push_back(Encode(mm));
    mm.pakName.clear(); mm.pakSize = 0; mm.sha = {}; s.push_back(Encode(mm));      // a map no pak is known for
    s.push_back(Encode(ModMapMsg{}));                                              // the stock-pick clear
    s.push_back(Encode(dice::LevelSeedMsg{7, 0x0123456789ABCDEFull}));            // a level's shared-dice seed
    { MapResetMsg mr; mr.seq = 0xFFFF; mr.round = 12; mr.flags = kMapResetRematch; s.push_back(Encode(mr)); s.push_back(Encode(MapResetMsg{})); }
    for (uint8_t mask : {uint8_t(kClearAll), uint8_t(kClearRocks), uint8_t(0)}) s.push_back(Encode(GroundClearMsg{200, mask}));   // /clear (mask 0 is a bad one)
    {   // a pak full directory index, for the pak reader (byte-level mutation reaches its length and count checks)
        std::vector<uint8_t> fdi;
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) fdi.push_back(uint8_t(v >> (8 * i))); };
        auto fstr = [&](const char* t) { const size_t n = std::strlen(t); u32(uint32_t(n + 1)); fdi.insert(fdi.end(), t, t + n); fdi.push_back(0); };
        u32(2); fstr("HalfswordUE5/Content/Maps/Custom/"); u32(2); fstr("Arena_Bog.umap"); u32(0); fstr("Arena_Bog.uexp"); u32(0);
        fstr("/"); u32(1); fstr("Top.umap"); u32(0);
        s.push_back(fdi);
    }
    combat::PaintFxMsg bl; bl.entity = 3; bl.kind = combat::FxKind::Blood; bl.seq = 7; bl.bone = "lowerarm_r"; bl.bodyPart = 2;
    bl.local.pos = sync::V3{ 4, -2, 7.5 }; bl.local.rot = sync::Q4{ 0, 0, std::sin(0.3), std::cos(0.3) }; bl.rate = 12.5; bl.damageRate = 1;
    bl.loopCount = 4; bl.extraVelocity = true; bl.velocity = sync::V3{ -30, 12, 400 }; bl.bleeding = 37; s.push_back(Encode(bl));
    combat::PaintFxMsg wd; wd.entity = uint8_t(kMaxWillies); wd.kind = combat::FxKind::Wound; wd.bone = "head"; wd.extent = sync::V3{ 3, 3, 3 };
    wd.damageRate = 1.5; s.push_back(Encode(wd));
    combat::EquipStateMsg eq; eq.entity = 2; eq.seq = 9; eq.hand = {1, 4}; eq.sheathed = 0x04; s.push_back(Encode(eq));
    PropCensusMsg pc; pc.seq = 3; pc.part = 1; pc.parts = 2;
    pc.entries = {CensusEntry{0, 0x1234u, 10, -20, 30}, CensusEntry{9, 0xBEEFu, -500, 0.5f, 120}}; s.push_back(Encode(pc));
    GiveUpMsg gu; gu.entity = 2; s.push_back(Encode(gu));
    ReportShareMsg rs; rs.fromSlot = 1; rs.id = "r7Kq2"; rs.token = "Ab3_x-9QwertyUiop12"; s.push_back(Encode(rs));
    return s;
}
}  // namespace

int main(int argc, char** argv) {
    long iterations = argc > 1 ? std::atol(argv[1]) : 300000;
    auto seeds = Seeds();
    std::vector<uint8_t> buf;
    for (long i = 0; i < iterations; ++i) {
        if (Rand() % 4 == 0) {                       // pure random bytes, sometimes with a valid header
            buf.resize(Below(600));
            for (uint8_t& b : buf) b = uint8_t(Rand());
            if (buf.size() >= 4 && Rand() % 2) { buf[0] = 0x48; buf[1] = 0x4D; buf[2] = kProtocolRevision; buf[3] = uint8_t(1 + Below(kLastReservedMsgType)); }
        } else {                                     // mutate a valid message
            buf = seeds[Below(seeds.size())];
            int edits = 1 + int(Below(4));
            for (int k = 0; k < edits; ++k) {
                switch (Below(4)) {
                    case 0: if (!buf.empty()) buf[Below(buf.size())] ^= uint8_t(1u << Below(8)); break;
                    case 1: if (!buf.empty()) buf.resize(Below(buf.size())); break;
                    case 2: buf.insert(buf.begin() + long(Below(buf.size() + 1)), uint8_t(Rand())); break;
                    case 3: if (!buf.empty()) buf[Below(buf.size())] = uint8_t(Rand()); break;
                }
            }
        }
        // Copy into an exact-size heap block so ASan sees any read past the end.
        uint8_t* exact = static_cast<uint8_t*>(std::malloc(buf.size() ? buf.size() : 1));
        if (!buf.empty()) std::memcpy(exact, buf.data(), buf.size());
        TryAll(exact, buf.size());
        std::free(exact);
    }
    std::printf("fuzz: %ld inputs, %d round-trip failures\n", iterations, g_roundTripFailures);
    return g_roundTripFailures ? 1 : 0;
}
