// Prop leases (FREE/TOUCH/HOLD, host-arbitrated, 3 s lapse): wire format for PropState and PropLease (round trip,
// truncation, non-finite, out-of-range, trailing bytes, propId bounds), the host's LeaseTable (grant/conflict/renew/
// lapse/release/entity-left, host-owned bots, who may stream, grants for a late joiner), the host's check of a peer's
// PropLease, the BodyStateView (only the granted streamer applies; stale/duplicate rejection per streamer), each
// machine's LeaseClaim (request/renew/downgrade/release from its own holds and touches), the rest test and the host's
// FREE stream schedule, and the propId identity scheme (weapon owner+slot vs. level-prop name hash never collide, id 0
// reserved; a dropped weapon's id carries a 32-bit epoch above the owner byte, advanced on every sequence wrap).
#include "session/hsm_lease.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

// ---------------------------------------------------------------- wire format: PropState
static BodyStateMsg SomeState() {
    BodyStateMsg m;
    m.objId = MakeWeaponPropId(3, 5);
    m.lessee = 2;
    m.senderTimeMs = 123456;
    m.px = 10.5f; m.py = -20.25f; m.pz = 5.f;
    m.rot = PackQuat(0, 0, 0.7071f, 0.7071f);
    m.vx = 100.f; m.vy = -50.f; m.vz = 0.f;
    m.wx = 0.f; m.wy = 30.f; m.wz = -10.f;
    return m;
}

static void TestPropStateRoundTrip() {
    const BodyStateMsg m = SomeState();
    const auto bytes = Encode(m);
    BodyStateMsg o;
    CHECK(Decode(bytes.data(), bytes.size(), o));
    CHECK(o.objId == m.objId && o.lessee == 2 && o.senderTimeMs == 123456);
    CHECK(o.px == 10.5f && o.py == -20.25f && o.pz == 5.f);
    CHECK(o.rot.x == m.rot.x && o.rot.w == m.rot.w);
    CHECK(o.vx == 100.f && o.vy == -50.f && o.vz == 0.f);
    CHECK(o.wx == 0.f && o.wy == 30.f && o.wz == -10.f);
}

static void TestPropStateRejects() {
    auto bad = [](BodyStateMsg m) { const auto b = Encode(m); BodyStateMsg o; return !Decode(b.data(), b.size(), o); };
    BodyStateMsg m = SomeState();
    m.objId = 0; CHECK(bad(m));                                             // id 0 reserved
    m = SomeState(); m.lessee = uint8_t(kMaxFighters); CHECK(bad(m));         // slot out of range
    m = SomeState(); m.lessee = uint8_t(kMaxFighters - 1); CHECK(!bad(m));    // last valid slot
    m = SomeState(); m.px = 1.0e7f; CHECK(!bad(m));                          // bound itself allowed
    m = SomeState(); m.px = 1.1e7f; CHECK(bad(m));
    m = SomeState(); m.py = std::numeric_limits<float>::quiet_NaN(); CHECK(bad(m));
    m = SomeState(); m.vz = std::numeric_limits<float>::infinity(); CHECK(bad(m));

    const auto good = Encode(SomeState());
    BodyStateMsg o;
    auto b = good; b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));     // trailing byte
    b = good; b.pop_back(); CHECK(!Decode(b.data(), b.size(), o));            // missing byte
    for (size_t n = 0; n < good.size(); ++n) CHECK(!Decode(good.data(), n, o) || n == good.size());
    b = good; b[3] = uint8_t(MsgType::PropLease); CHECK(!Decode(b.data(), b.size(), o));   // another type's header
    b = good; b[2] = uint8_t(kProtocolRevision - 1); CHECK(!Decode(b.data(), b.size(), o));
}

// ---------------------------------------------------------------- wire format: PropLease
static void TestPropLeaseRoundTrip() {
    PropLeaseMsg m; m.propId = MakeLevelPropId(42); m.action = LeaseAction::Request; m.level = LeaseLevel::Touch; m.slot = 4;
    const auto bytes = Encode(m);
    PropLeaseMsg o;
    CHECK(Decode(bytes.data(), bytes.size(), o));
    CHECK(o.propId == m.propId && o.action == LeaseAction::Request && o.level == LeaseLevel::Touch && o.slot == 4);

    PropLeaseMsg g; g.propId = m.propId; g.action = LeaseAction::Grant; g.level = LeaseLevel::Hold; g.slot = 0;
    const auto gb = Encode(g);
    PropLeaseMsg go;
    CHECK(Decode(gb.data(), gb.size(), go) && go.action == LeaseAction::Grant && go.level == LeaseLevel::Hold && go.slot == 0);
}

static void TestPropLeaseRejects() {
    auto bad = [](PropLeaseMsg m) { const auto b = Encode(m); PropLeaseMsg o; return !Decode(b.data(), b.size(), o); };
    PropLeaseMsg m; m.propId = MakeLevelPropId(1); m.action = LeaseAction::Request; m.level = LeaseLevel::Touch; m.slot = 1;
    CHECK(!bad(m));
    PropLeaseMsg z = m; z.propId = 0; CHECK(bad(z));
    PropLeaseMsg s = m; s.slot = uint8_t(kMaxFighters); CHECK(bad(s));
    PropLeaseMsg a = m; a.action = LeaseAction(99); CHECK(bad(a));
    PropLeaseMsg l = m; l.level = LeaseLevel(99); CHECK(bad(l));
    PropLeaseMsg freeReq = m; freeReq.level = LeaseLevel::Free; CHECK(bad(freeReq));   // Request must name Touch or Hold
    PropLeaseMsg rel = m; rel.action = LeaseAction::Release; rel.level = LeaseLevel::Free; CHECK(!bad(rel));

    const auto good = Encode(m);
    PropLeaseMsg o;
    auto b = good; b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));
    b = good; b.pop_back(); CHECK(!Decode(b.data(), b.size(), o));

    // A Grant of Free means "nobody leases it, the host streams it": it can only name the host.
    PropLeaseMsg gf = m; gf.action = LeaseAction::Grant; gf.level = LeaseLevel::Free; gf.slot = 3; CHECK(bad(gf));
    gf.slot = kHostSlot; CHECK(!bad(gf));
    PropLeaseMsg gt = m; gt.action = LeaseAction::Grant; gt.level = LeaseLevel::Touch; gt.slot = 3; CHECK(!bad(gt));
}

// propId decode bounds: a weapon id (bit 63 clear) is (owner << 8) | sheathSlot with owner a valid entity, plus a drop
// epoch in bits 16-47 (TestDropEpoch); an owner byte that is no entity is garbage, whatever the message.
static void TestPropIdBoundsOnDecode() {
    auto stateOk = [](uint64_t id) { BodyStateMsg m = SomeState(); m.objId = id; const auto b = Encode(m); BodyStateMsg o; return Decode(b.data(), b.size(), o); };
    auto leaseOk = [](uint64_t id) {
        PropLeaseMsg m; m.propId = id; m.action = LeaseAction::Request; m.level = LeaseLevel::Hold; m.slot = 1;
        const auto b = Encode(m); PropLeaseMsg o; return Decode(b.data(), b.size(), o);
    };
    for (uint64_t id : {MakeWeaponPropId(1, 0), MakeWeaponPropId(uint8_t(kMaxWillies), 255), MakeLevelPropId(1), MakeLevelPropId(65535), kPropLevelBit}) {
        CHECK(stateOk(id)); CHECK(leaseOk(id));
    }
    for (uint64_t id : {uint64_t(255), uint64_t(kMaxWillies + 1) << 8, uint64_t(1) << 16, uint64_t(1) << 40, kPropLevelBit - 1}) {
        CHECK(!stateOk(id)); CHECK(!leaseOk(id));
    }
    CHECK(MakeWeaponPropId(uint8_t(kMaxWillies + 1), 0) == 0);        // no such entity: no identity
    CHECK(MakeWeaponPropId(uint8_t(kMaxWillies), 0) != 0);
}

// The host's own code asking for Free is a no-op: no entry is ever stored at Free, and a lease is never dropped by it.
static void TestRequestFreeIsNoOp() {
    LeaseTable t;
    LeaseResult r = t.Request(1, 2, LeaseLevel::Free, 0.0);
    CHECK(!r.granted && !r.changed && r.slot == kHostSlot && r.level == LeaseLevel::Free);
    uint8_t slot; LeaseLevel lvl;
    CHECK(!t.Lessee(1, slot, lvl));
    CHECK(t.Tick(5000.0).empty());                    // nothing stored, so nothing lapses
    CHECK(t.Request(1, 2, LeaseLevel::Touch, 6000.0).granted);
    LeaseResult r2 = t.Request(1, 2, LeaseLevel::Free, 6010.0);
    CHECK(!r2.granted && !r2.changed && r2.slot == 2 && r2.level == LeaseLevel::Touch);
    CHECK(t.Lessee(1, slot, lvl) && slot == 2 && lvl == LeaseLevel::Touch);
}

// ---------------------------------------------------------------- host-side LeaseTable
static void TestGrantWhenFree() {
    LeaseTable t;
    LeaseResult r = t.Request(100, 2, LeaseLevel::Touch, 0.0);
    CHECK(r.granted && r.slot == 2 && r.level == LeaseLevel::Touch && r.changed);
    uint8_t slot; LeaseLevel lvl;
    CHECK(t.Lessee(100, slot, lvl) && slot == 2 && lvl == LeaseLevel::Touch);
}

static void TestTouchVsTouchFirstWins() {
    LeaseTable t;
    CHECK(t.Request(1, 1, LeaseLevel::Touch, 0.0).granted);
    LeaseResult r2 = t.Request(1, 2, LeaseLevel::Touch, 10.0);           // second toucher: denied
    CHECK(!r2.granted && r2.slot == 1 && r2.level == LeaseLevel::Touch);
    uint8_t slot; LeaseLevel lvl;
    CHECK(t.Lessee(1, slot, lvl) && slot == 1 && lvl == LeaseLevel::Touch);   // first keeps it
}

static void TestHoldBeatsTouch() {
    LeaseTable t;
    CHECK(t.Request(1, 1, LeaseLevel::Touch, 0.0).granted);
    LeaseResult r2 = t.Request(1, 2, LeaseLevel::Hold, 10.0);
    CHECK(r2.granted && r2.slot == 2 && r2.level == LeaseLevel::Hold && r2.changed);   // hold evicts touch
    uint8_t slot; LeaseLevel lvl;
    CHECK(t.Lessee(1, slot, lvl) && slot == 2 && lvl == LeaseLevel::Hold);
    // ... and a later touch from a third slot cannot evict the hold.
    LeaseResult r3 = t.Request(1, 3, LeaseLevel::Touch, 20.0);
    CHECK(!r3.granted && r3.slot == 2 && r3.level == LeaseLevel::Hold);
}

static void TestHoldVsHoldFirstWins() {
    LeaseTable t;
    CHECK(t.Request(1, 1, LeaseLevel::Hold, 0.0).granted);
    LeaseResult r2 = t.Request(1, 2, LeaseLevel::Hold, 10.0);
    CHECK(!r2.granted && r2.slot == 1 && r2.level == LeaseLevel::Hold);
}

static void TestRenewAndUpgrade() {
    LeaseTable t;
    CHECK(t.Request(1, 1, LeaseLevel::Touch, 0.0).granted);
    LeaseResult renew = t.Request(1, 1, LeaseLevel::Touch, 100.0);        // same slot, same level: renews, not "changed"
    CHECK(renew.granted && !renew.changed);
    LeaseResult up = t.Request(1, 1, LeaseLevel::Hold, 200.0);            // same slot upgrades touch -> hold
    CHECK(up.granted && up.changed && up.level == LeaseLevel::Hold);
    // The renewal at t=200 keeps it alive past the original 3 s window from t=0.
    std::vector<uint64_t> lapsed = t.Tick(3000.0);
    CHECK(lapsed.empty());
    uint8_t slot; LeaseLevel lvl;
    CHECK(t.Lessee(1, slot, lvl) && slot == 1 && lvl == LeaseLevel::Hold);
}

static void TestLapseAtExactly3sBoundary() {
    LeaseTable t;
    t.Request(1, 1, LeaseLevel::Touch, 0.0);
    CHECK(t.Tick(2999.999).empty());                 // not yet
    uint8_t slot; LeaseLevel lvl;
    CHECK(t.Lessee(1, slot, lvl));                    // still held
    std::vector<uint64_t> lapsed = t.Tick(3000.0);    // exactly 3 s since the last report: lapses
    CHECK((lapsed == std::vector<uint64_t>{1}));
    CHECK(!t.Lessee(1, slot, lvl));                   // FREE again
}

static void TestRelease() {
    LeaseTable t;
    t.Request(5, 1, LeaseLevel::Hold, 0.0);
    LeaseResult byOther = t.Release(5, 2);            // not the lessee: no-op
    CHECK(!byOther.granted);
    uint8_t slot; LeaseLevel lvl;
    CHECK(t.Lessee(5, slot, lvl) && slot == 1);
    LeaseResult ok = t.Release(5, 1);
    CHECK(ok.granted && ok.changed);
    CHECK(!t.Lessee(5, slot, lvl));                   // FREE
    CHECK(!t.Release(5, 1).granted);                  // already free: no-op
}

static void TestEntityLeft() {
    LeaseTable t;
    t.Request(1, 3, LeaseLevel::Touch, 0.0);
    t.Request(2, 3, LeaseLevel::Hold, 0.0);
    t.Request(3, 4, LeaseLevel::Touch, 0.0);          // another slot's lease: must survive
    std::vector<uint64_t> freed = t.EntityLeft(3);
    std::sort(freed.begin(), freed.end());
    CHECK((freed == std::vector<uint64_t>{1, 2}));
    uint8_t slot; LeaseLevel lvl;
    CHECK(!t.Lessee(1, slot, lvl) && !t.Lessee(2, slot, lvl));
    CHECK(t.Lessee(3, slot, lvl) && slot == 4);
}

static void TestHostOwnedBots() {
    // A host bot is slot kHostSlot, exactly like the host's own fighter (session/hsm_authority.h OwnerSlotOf):
    // no special-casing needed, arbitration treats it like any other requester.
    LeaseTable t;
    CHECK(t.Request(9, kHostSlot, LeaseLevel::Touch, 0.0).granted);
    LeaseResult other = t.Request(9, 1, LeaseLevel::Touch, 10.0);
    CHECK(!other.granted && other.slot == kHostSlot);
    LeaseResult evict = t.Request(9, 1, LeaseLevel::Hold, 20.0);
    CHECK(evict.granted && evict.slot == 1);          // a client's hold still beats the host-bot's touch
}

static void TestDuplicateRequestFromSameSlotIsIdempotent() {
    LeaseTable t;
    t.Request(1, 1, LeaseLevel::Touch, 0.0);
    LeaseResult dup = t.Request(1, 1, LeaseLevel::Touch, 1.0);   // stale/duplicate re-send of the same report
    CHECK(dup.granted && !dup.changed);
}

// ---------------------------------------------------------------- client-side BodyStateView
static void TestBodyStateViewStaleAndDuplicateRejected() {
    BodyStateView v;
    v.OnGrant(7, 2, LeaseLevel::Touch);               // SomeState's lessee streams it
    BodyStateMsg a = SomeState(); a.objId = 7; a.senderTimeMs = 100; a.px = 1.f;
    CHECK(v.Apply(a));
    BodyStateMsg dup = a;                             // exact duplicate (same sender time): rejected
    CHECK(!v.Apply(dup));
    BodyStateMsg stale = a; stale.senderTimeMs = 50; stale.px = 2.f;   // older: rejected
    CHECK(!v.Apply(stale));
    CHECK(v.Find(7) && v.Find(7)->px == 1.f);
    BodyStateMsg fresh = a; fresh.senderTimeMs = 150; fresh.px = 3.f;
    CHECK(v.Apply(fresh));
    CHECK(v.Find(7)->px == 3.f);
    CHECK(v.Find(123) == nullptr);
}

static void TestBodyStateViewWrapAround() {
    BodyStateView v;
    v.OnGrant(8, 2, LeaseLevel::Hold);
    BodyStateMsg a = SomeState(); a.objId = 8; a.senderTimeMs = 0xFFFFFFF0u; a.px = 1.f;
    CHECK(v.Apply(a));
    BodyStateMsg b = a; b.senderTimeMs = 4u; b.px = 2.f;              // 20 ms later across the wrap
    CHECK(v.Apply(b));
    CHECK(v.Find(8)->px == 2.f);
    BodyStateMsg c = a; c.senderTimeMs = 0xFFFFFFF8u; c.px = 3.f;     // older across the wrap: rejected
    CHECK(!v.Apply(c));
    CHECK(v.Find(8)->px == 2.f);
}

// A new streamer's clock has nothing to do with the previous one's: the host's clock may be far ahead of the new
// lessee's. Only the granted streamer's states apply, each strictly newer than that streamer's last.
static void TestBodyStateViewFollowsGrants() {
    BodyStateView v;
    CHECK(v.Streamer(9) == kHostSlot && v.Level(9) == LeaseLevel::Free);   // unknown: FREE, the host streams it
    BodyStateMsg h = SomeState(); h.objId = 9; h.lessee = kHostSlot; h.senderTimeMs = 1000000; h.px = 1.f;
    CHECK(v.Apply(h));
    BodyStateMsg early = h; early.lessee = 2; early.senderTimeMs = 500; early.px = 5.f;
    CHECK(!v.Apply(early));                           // slot 2 streams before its grant reached us: not yet
    CHECK(v.Find(9)->px == 1.f);
    v.OnGrant(9, 2, LeaseLevel::Touch);
    CHECK(v.Streamer(9) == 2 && v.Level(9) == LeaseLevel::Touch);
    CHECK(v.Find(9) && v.Find(9)->px == 1.f);         // the last state stays until the new streamer's first
    BodyStateMsg c = early; c.senderTimeMs = 600; c.px = 2.f;
    CHECK(v.Apply(c));                                // far behind the host's clock, still the newest from slot 2
    CHECK(v.Find(9)->px == 2.f);
    BodyStateMsg late = h; late.senderTimeMs = 1000050; late.px = 9.f;
    CHECK(!v.Apply(late));                            // the former streamer's delayed state: dropped
    CHECK(!v.Apply(c));                               // duplicate within the streamer
    BodyStateMsg older = c; older.senderTimeMs = 599; CHECK(!v.Apply(older));
    v.OnGrant(9, 2, LeaseLevel::Hold);                // same streamer upgrading: its clock still counts
    CHECK(v.Level(9) == LeaseLevel::Hold);
    CHECK(!v.Apply(older));
    BodyStateMsg next = c; next.senderTimeMs = 616; next.px = 3.f; CHECK(v.Apply(next));
    v.OnGrant(9, 3, LeaseLevel::Free);                // FREE always means the host, whatever slot it names
    CHECK(v.Streamer(9) == kHostSlot && v.Level(9) == LeaseLevel::Free);
    BodyStateMsg h2 = h; h2.senderTimeMs = 1000100; h2.px = 4.f; CHECK(v.Apply(h2));
    v.OnGrant(9, 4, LeaseLevel::Touch);
    BodyStateMsg s4 = h; s4.lessee = 4; s4.senderTimeMs = 7; s4.px = 6.f; CHECK(v.Apply(s4));
    CHECK(v.Find(9)->px == 6.f && v.Find(9)->lessee == 4);
    v.OnGrant(11, 2, LeaseLevel::Touch);              // a grant for a prop never streamed carries no state
    CHECK(v.Find(11) == nullptr && v.Streamer(11) == 2);
    v.Clear();
    CHECK(v.Find(9) == nullptr && v.Streamer(9) == kHostSlot);
}

static void TestLeaseStreamerAndAccepts() {
    LeaseTable t;
    CHECK(t.Streamer(1) == kHostSlot);                // FREE: the host streams it
    BodyStateMsg m = SomeState(); m.objId = 1; m.lessee = 2;
    CHECK(!t.Accepts(m, 2));                          // nobody leases it: slot 2 may not stream it
    CHECK(t.Request(1, 2, LeaseLevel::Touch, 0.0).granted);
    CHECK(t.Streamer(1) == 2);
    CHECK(t.Accepts(m, 2));
    CHECK(!t.Accepts(m, 3));                          // claims to be slot 2, sent by slot 3
    BodyStateMsg spoof = m; spoof.lessee = 3; CHECK(!t.Accepts(spoof, 3));   // slot 3 is not the lessee
    BodyStateMsg lie = m; lie.lessee = 3; CHECK(!t.Accepts(lie, 2));        // the lessee naming another slot: not relayed
    BodyStateMsg other = m; other.objId = 2; CHECK(!t.Accepts(other, 2));   // a lease covers one prop only
    CHECK(t.Request(1, 4, LeaseLevel::Hold, 10.0).granted);                  // evicted by a hold
    CHECK(!t.Accepts(m, 2) && t.Streamer(1) == 4);
    CHECK(t.Release(1, 4).granted);
    CHECK(t.Streamer(1) == kHostSlot && !t.Accepts(m, 2));
}

static void TestGrantsForLateJoiner() {
    LeaseTable t;
    CHECK(t.Grants().empty());
    t.Request(7, 2, LeaseLevel::Hold, 0.0);
    t.Request(5, 1, LeaseLevel::Touch, 0.0);
    std::vector<PropLeaseMsg> g = t.Grants();
    std::sort(g.begin(), g.end(), [](const PropLeaseMsg& a, const PropLeaseMsg& b) { return a.propId < b.propId; });
    CHECK(g.size() == 2);
    if (g.size() == 2) {
        CHECK(g[0].propId == 5 && g[0].action == LeaseAction::Grant && g[0].slot == 1 && g[0].level == LeaseLevel::Touch);
        CHECK(g[1].propId == 7 && g[1].action == LeaseAction::Grant && g[1].slot == 2 && g[1].level == LeaseLevel::Hold);
    }
    t.Release(5, 1);
    CHECK(t.Grants().size() == 1);
}

static void TestLeaseFromPeer() {
    PropLeaseMsg r; r.propId = 1; r.action = LeaseAction::Request; r.level = LeaseLevel::Touch; r.slot = 2;
    CHECK(LeaseFromPeerOk(r, 2));
    CHECK(!LeaseFromPeerOk(r, 3));                    // speaks for another slot
    PropLeaseMsg rel = r; rel.action = LeaseAction::Release; rel.level = LeaseLevel::Free;
    CHECK(LeaseFromPeerOk(rel, 2));
    CHECK(!LeaseFromPeerOk(rel, 1));
    PropLeaseMsg g = r; g.action = LeaseAction::Grant;
    CHECK(!LeaseFromPeerOk(g, 2));                    // only the host grants
}

// ---------------------------------------------------------------- each machine's claim on a prop
static void TestClaimTouchLifecycle() {
    LeaseClaim c; LeaseLevel lv = LeaseLevel::Hold;
    constexpr double never = -1.0e9;
    CHECK(StepClaim(c, false, never, true, 0.0, lv) == ClaimSend::None && lv == LeaseLevel::Free);   // nothing of mine on it
    CHECK(StepClaim(c, false, 100.0, true, 100.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);
    CHECK(StepClaim(c, false, 100.0, true, 116.0, lv) == ClaimSend::None && lv == LeaseLevel::Touch);   // asked already
    CHECK(StepClaim(c, false, 1050.0, true, 1099.0, lv) == ClaimSend::None);
    CHECK(StepClaim(c, false, 1050.0, true, 1100.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);   // renewal: 1 s
    CHECK(StepClaim(c, false, 1100.0, false, 1349.0, lv) == ClaimSend::None && lv == LeaseLevel::Touch);   // still in contact
    CHECK(StepClaim(c, false, 1100.0, false, 1350.0, lv) == ClaimSend::None && lv == LeaseLevel::Touch);   // contact over, settling
    CHECK(StepClaim(c, false, 1100.0, false, 2100.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);   // still mine: renewed
    CHECK(StepClaim(c, false, 1100.0, false, 2849.0, lv) == ClaimSend::None && lv == LeaseLevel::Touch);
    CHECK(StepClaim(c, false, 1100.0, false, 2850.0, lv) == ClaimSend::Release && lv == LeaseLevel::Free);   // 1.5 s at rest
    CHECK(StepClaim(c, false, 1100.0, false, 2900.0, lv) == ClaimSend::None && lv == LeaseLevel::Free);   // released once
    CHECK(StepClaim(c, false, 1100.0, true, 4000.0, lv) == ClaimSend::None && lv == LeaseLevel::Free);   // moved by someone else
    CHECK(StepClaim(c, false, 4100.0, true, 4100.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);   // a new touch
}

static void TestClaimHoldThrowSettle() {
    LeaseClaim c; LeaseLevel lv = LeaseLevel::Free;
    constexpr double never = -1.0e9;
    CHECK(StepClaim(c, true, never, true, 0.0, lv) == ClaimSend::Request && lv == LeaseLevel::Hold);
    CHECK(StepClaim(c, true, never, true, 500.0, lv) == ClaimSend::None && lv == LeaseLevel::Hold);
    CHECK(StepClaim(c, true, 900.0, true, 1000.0, lv) == ClaimSend::Request && lv == LeaseLevel::Hold);   // held and touching: hold
    CHECK(StepClaim(c, false, never, true, 1200.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);   // let go: downgrade now
    CHECK(StepClaim(c, false, never, true, 1300.0, lv) == ClaimSend::None && lv == LeaseLevel::Touch);   // the throw is mine
    CHECK(StepClaim(c, false, never, true, 2200.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);
    CHECK(StepClaim(c, false, never, false, 2300.0, lv) == ClaimSend::None);   // at rest from 2300
    CHECK(StepClaim(c, false, never, true, 2400.0, lv) == ClaimSend::None);    // bounced: not at rest any more
    CHECK(StepClaim(c, false, never, false, 2500.0, lv) == ClaimSend::None);   // at rest from 2500
    CHECK(StepClaim(c, false, never, false, 3200.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);
    CHECK(StepClaim(c, false, never, false, 3999.0, lv) == ClaimSend::None && lv == LeaseLevel::Touch);
    CHECK(StepClaim(c, false, never, false, 4000.0, lv) == ClaimSend::Release && lv == LeaseLevel::Free);
    CHECK(StepClaim(c, true, never, false, 4100.0, lv) == ClaimSend::Request && lv == LeaseLevel::Hold);   // grabbed again
}

static void TestClaimContactWindowAndRestReset() {
    LeaseClaim a, b; LeaseLevel lv;
    CHECK(StepClaim(a, false, 0.0, false, 249.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);
    CHECK(StepClaim(b, false, 0.0, false, 250.0, lv) == ClaimSend::None && lv == LeaseLevel::Free);   // contact too old
    // A touch while settling starts the rest wait over.
    LeaseClaim c;
    CHECK(StepClaim(c, false, 0.0, false, 0.0, lv) == ClaimSend::Request);
    CHECK(StepClaim(c, false, 0.0, false, 300.0, lv) == ClaimSend::None);     // at rest from 300
    CHECK(StepClaim(c, false, 0.0, false, 1000.0, lv) == ClaimSend::Request); // renewal
    CHECK(StepClaim(c, false, 1700.0, false, 1700.0, lv) == ClaimSend::None && lv == LeaseLevel::Touch);   // touched again
    CHECK(StepClaim(c, false, 1700.0, false, 2000.0, lv) == ClaimSend::Request && lv == LeaseLevel::Touch);   // at rest from 2000
    CHECK(StepClaim(c, false, 1700.0, false, 3000.0, lv) == ClaimSend::Request);
    CHECK(StepClaim(c, false, 1700.0, false, 3499.0, lv) == ClaimSend::None);
    CHECK(StepClaim(c, false, 1700.0, false, 3500.0, lv) == ClaimSend::Release);
}

static void TestPropAtRestAndFreeSchedule() {
    auto rest = [](float vx, float vy, float vz, float wx, float wy, float wz) {
        BodyStateMsg m = SomeState(); m.vx = vx; m.vy = vy; m.vz = vz; m.wx = wx; m.wy = wy; m.wz = wz; return PropAtRest(m);
    };
    CHECK(rest(0, 0, 0, 0, 0, 0));
    CHECK(rest(7.9f, 0, 0, 0, 0, 9.9f));
    CHECK(!rest(8.f, 0, 0, 0, 0, 0));                  // 8 cm/s: moving
    CHECK(!rest(0, 0, -8.f, 0, 0, 0));
    CHECK(!rest(5.f, 5.f, 5.f, 0, 0, 0));              // 8.7 cm/s overall, under 8 on every axis
    CHECK(!rest(0, 0, 0, 0, 10.f, 0));                 // 10 deg/s: turning
    CHECK(!rest(0, 0, 0, 6.f, -6.f, 6.f));             // 10.4 deg/s overall
    CHECK(!FreeStreamDue(0.0, false, 15.9));           // moving: as fast as a leased prop (a thrown weapon), about 60 Hz
    CHECK(FreeStreamDue(0.0, false, 16.0));
    CHECK(!FreeStreamDue(0.0, true, 999.0));           // at rest: once a second
    CHECK(FreeStreamDue(0.0, true, 1000.0));
    CHECK(FreeStreamDue(-1.0e9, true, 0.0));           // never sent
}

// ---------------------------------------------------------------- identity scheme
static void TestIdentityScheme() {
    CHECK(MakeWeaponPropId(3, 5) == MakeWeaponPropId(3, 5));
    CHECK(MakeWeaponPropId(3, 5) != MakeWeaponPropId(3, 6));
    CHECK(MakeWeaponPropId(3, 5) != MakeWeaponPropId(4, 5));
    CHECK((MakeWeaponPropId(3, 5) & kPropLevelBit) == 0);             // weapon ids never look like a level prop
    // A level prop is named by the host's census (session/hsm_prop_census.h): its index there, nothing else.
    CHECK(MakeLevelPropId(7) == MakeLevelPropId(7));
    CHECK(MakeLevelPropId(7) != MakeLevelPropId(8));
    CHECK(MakeLevelPropId(7) == (kPropLevelBit | 7));
    CHECK((MakeLevelPropId(7) & kPropLevelBit) != 0);                 // level-prop ids are always tagged
    CHECK(MakeLevelPropId(0) == kPropLevelBit && MakeLevelPropId(0) != 0);   // index 0 is a real prop, never "no id"
    CHECK(MakeLevelPropId(65535) == (kPropLevelBit | 0xFFFF));
    for (uint32_t i = 0; i <= 0xFFFF; i += 257) {
        const uint64_t id = MakeLevelPropId(uint16_t(i));
        CHECK(LevelPropIndex(id) == i && IsCanonicalLevelPropId(id) && ValidPropId(id) && IsLevelProp(id));
    }
    // A level id with anything above its index set is no census id (ValidPropId still lets it through the decoders).
    for (int bit = 16; bit < 63; ++bit) {
        const uint64_t junk = MakeLevelPropId(5) | (uint64_t(1) << bit);
        CHECK(!IsCanonicalLevelPropId(junk) && ValidPropId(junk) && LevelPropIndex(junk) == 5);
    }
    CHECK(!IsCanonicalLevelPropId(MakeWeaponPropId(3, 5)) && !IsCanonicalLevelPropId(0));
    CHECK(!IsCanonicalLevelPropId(MakeDropPropId(3, 1, 5, 0xDEADBEEFu)));
    CHECK(LevelPropIndex(MakeWeaponPropId(3, 5)) == 0 && LevelPropIndex(0) == 0);   // a weapon id has no census index
    for (uint8_t owner = 1; owner <= kMaxWillies; ++owner)
        for (uint8_t slot = 0; slot < 8; ++slot) CHECK(MakeWeaponPropId(owner, slot) != 0);
    CHECK(MakeWeaponPropId(0, 0) == 0);                                // the reserved/degenerate case: no known holder
    // What a receiver needs to find the actor: the weapon's holder entity and sheath slot back out of the id.
    CHECK(!IsLevelProp(MakeWeaponPropId(3, 5)) && IsLevelProp(MakeLevelPropId(7)));
    CHECK(PropOwnerEntity(MakeWeaponPropId(3, 5)) == 3 && PropSheathSlot(MakeWeaponPropId(3, 5)) == 5);
    CHECK(PropOwnerEntity(MakeWeaponPropId(uint8_t(kMaxWillies), 255)) == kMaxWillies && PropSheathSlot(MakeWeaponPropId(1, 255)) == 255);
    CHECK(PropOwnerEntity(MakeLevelPropId(0x0305)) == 0 && PropSheathSlot(MakeLevelPropId(0x0305)) == 0);
}

// ---------------------------------------------------------------- dropped-weapon identity (owner sync, prop_sync.cpp)
// A weapon's SheathSlot is its sheath *type* (two daggers share one), so a drop is keyed by the dropping hand and a
// per-owner sequence instead: the low byte is (seq << 1) | hand. Only the owner's machine mints them.
static void TestDropIdentity() {
    CHECK(MakeDropPropId(3, 0, 0) == MakeWeaponPropId(3, 0));
    CHECK(MakeDropPropId(3, 1, 0) == MakeWeaponPropId(3, 1));
    CHECK(MakeDropPropId(3, 1, 5) == MakeWeaponPropId(3, 11));
    CHECK(MakeDropPropId(3, 0, 127) == MakeWeaponPropId(3, 254));
    CHECK(MakeDropPropId(3, 0, 128) == MakeDropPropId(3, 0, 0));          // 7-bit sequence wraps
    CHECK(MakeDropPropId(3, 1, 255) == MakeDropPropId(3, 1, 127));
    CHECK(MakeDropPropId(3, 2, 0) == 0 && MakeDropPropId(3, 255, 4) == 0);   // only two hands
    CHECK(MakeDropPropId(0, 0, 0) == 0 && MakeDropPropId(uint8_t(kMaxWillies + 1), 1, 1) == 0);
    CHECK(MakeDropPropId(uint8_t(kMaxWillies), 1, 127) != 0);
    CHECK(PropHand(MakeDropPropId(3, 1, 5)) == 1 && PropHand(MakeDropPropId(3, 0, 127)) == 0);
    CHECK(PropDropSeq(MakeDropPropId(3, 1, 5)) == 5 && PropDropSeq(MakeDropPropId(3, 0, 127)) == 127);
    CHECK(PropDropSeq(MakeDropPropId(3, 1, 0)) == 0);
    CHECK(PropOwnerEntity(MakeDropPropId(9, 1, 77)) == 9);
    CHECK(PropHand(MakeLevelPropId(0x0303)) == 0 && PropDropSeq(MakeLevelPropId(0x0303)) == 0);
    std::vector<uint64_t> ids;
    for (int hand = 0; hand < 2; ++hand)
        for (int seq = 0; seq < 128; ++seq) {
            const uint64_t id = MakeDropPropId(4, uint8_t(hand), uint8_t(seq));
            CHECK(ValidPropId(id) && !IsLevelProp(id) && PropOwnerEntity(id) == 4);
            ids.push_back(id);
        }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());       // 256 distinct ids per owner
}

static void TestMintDropPropId() {
    uint8_t seq = 0;
    uint32_t ep = 0;                                                      // the epoch: TestDropEpoch
    auto none = [](uint64_t) { return false; };
    CHECK(MintDropPropId(5, 1, seq, ep, none) == MakeDropPropId(5, 1, 0) && seq == 1);
    CHECK(MintDropPropId(5, 1, seq, ep, none) == MakeDropPropId(5, 1, 1) && seq == 2);
    const uint64_t busy = MakeDropPropId(5, 0, 2);                        // still live from an earlier drop: skipped
    CHECK(MintDropPropId(5, 0, seq, ep, [&](uint64_t id) { return id == busy; }) == MakeDropPropId(5, 0, 3) && seq == 4);
    seq = 127;
    CHECK(MintDropPropId(5, 0, seq, ep, none) == MakeDropPropId(5, 0, 127) && seq == 0);   // wraps
    seq = 126; ep = 0;
    CHECK(MintDropPropId(5, 0, seq, ep, [](uint64_t id) { return PropDropSeq(id) == 126; }) == MakeDropPropId(5, 0, 127) && seq == 0);
    seq = 0; ep = 0;                                                      // the last free id of all 128 is found
    CHECK(MintDropPropId(5, 0, seq, ep, [](uint64_t id) { return PropDropSeq(id) != 127; }) == MakeDropPropId(5, 0, 127) && seq == 0);
    seq = 9;
    CHECK(MintDropPropId(5, 0, seq, ep, [](uint64_t) { return true; }) == 0);   // every id live: no identity
    seq = 3;
    CHECK(MintDropPropId(0, 0, seq, ep, none) == 0);                      // no such entity
    CHECK(MintDropPropId(5, 2, seq, ep, none) == 0);                      // no such hand
}

// A dropped weapon's id carries a 32-bit epoch in bits 16-47 (bits 48-62 clear): the minting machine starts it at a
// per-process value and advances it on every wrap of the 7-bit sequence, so an id is never minted twice -- not after
// 128 drops, not by a machine that reconnected -- whether or not an older prop with it is still around.
static void TestDropEpoch() {
    CHECK(MakeDropPropId(3, 1, 5, 0) == MakeDropPropId(3, 1, 5));         // epoch 0: the plain weapon id
    const uint64_t e = MakeDropPropId(3, 1, 5, 0xDEADBEEFu);
    CHECK(e != MakeDropPropId(3, 1, 5) && ValidPropId(e) && !IsLevelProp(e));
    CHECK(e == (MakeDropPropId(3, 1, 5) | (uint64_t(0xDEADBEEFu) << 16)));
    CHECK(PropOwnerEntity(e) == 3 && PropHand(e) == 1 && PropDropSeq(e) == 5 && PropDropEpoch(e) == 0xDEADBEEFu);
    CHECK(PropDropEpoch(MakeDropPropId(3, 1, 5)) == 0 && PropDropEpoch(MakeLevelPropId(0xFFFF)) == 0);
    // A level id has no drop epoch, whatever lies in bits 16-62: ValidPropId accepts any id with bit 63, so a peer can
    // send one with bits set where a weapon's epoch would be.
    CHECK(ValidPropId(kPropLevelBit | (uint64_t(0xDEADBEEFu) << 16) | 7) &&
          PropDropEpoch(kPropLevelBit | (uint64_t(0xDEADBEEFu) << 16) | 7) == 0);
    CHECK(PropDropEpoch(MakeDropPropId(3, 0, 0, 1)) == 1 && MakeDropPropId(3, 0, 0, 1) != MakeDropPropId(3, 0, 0, 2));
    CHECK(MakeDropPropId(0, 0, 0, 7) == 0 && MakeDropPropId(3, 2, 0, 7) == 0);   // no identity, whatever the epoch
    CHECK(MakeDropPropId(uint8_t(kMaxWillies + 1), 0, 0, 7) == 0);
    const uint64_t top = MakeDropPropId(uint8_t(kMaxWillies), 1, 127, 0xFFFFFFFFu);
    CHECK(top != 0 && ValidPropId(top) && PropOwnerEntity(top) == kMaxWillies && PropDropEpoch(top) == 0xFFFFFFFFu);
    // Garbage stays garbage: an owner byte that is no entity, or anything above the epoch.
    CHECK(!ValidPropId(uint64_t(0xABCD) << 16));
    CHECK(!ValidPropId((uint64_t(7) << 16) | (uint64_t(kMaxWillies + 1) << 8)));
    CHECK(!ValidPropId(MakeDropPropId(3, 1, 5) | (uint64_t(1) << 48)));
    CHECK(ValidPropId(MakeDropPropId(3, 1, 5) | (uint64_t(1) << 62)));   // rev 26: bit 62 is a spawned id (slot 0, a sequence), not a weapon id
    CHECK(!ValidPropId(MakeDropPropId(3, 1, 5) | (uint64_t(1) << 62) | (uint64_t(1) << 48)));   // ... with bits 32-55 clear or it is garbage
    CHECK(ValidPropId(MakeDropPropId(3, 1, 5) | (uint64_t(1) << 47)));
    // Both decoders take an epoch id and refuse bits above it.
    auto stateOk = [](uint64_t id) { BodyStateMsg m = SomeState(); m.objId = id; const auto b = Encode(m); BodyStateMsg o; return Decode(b.data(), b.size(), o) && o.objId == id; };
    auto leaseOk = [](uint64_t id) {
        PropLeaseMsg m; m.propId = id; m.action = LeaseAction::Request; m.level = LeaseLevel::Touch; m.slot = 1;
        const auto b = Encode(m); PropLeaseMsg o; return Decode(b.data(), b.size(), o) && o.propId == id;
    };
    CHECK(stateOk(e) && leaseOk(e) && stateOk(top) && leaseOk(top));
    CHECK(!stateOk(e | (uint64_t(1) << 50)) && !leaseOk(e | (uint64_t(1) << 50)));
    // A drop by the owner's machine is recognised whatever its epoch.
    CHECK(DropSignature(MakeDropPropId(3, 0, 0, 0x12345u), LeaseLevel::Touch, 2));

    // Minting: a wrap of the sequence starts the next epoch.
    auto none = [](uint64_t) { return false; };
    uint8_t seq = 126;
    uint32_t ep = 41;
    CHECK(MintDropPropId(5, 0, seq, ep, none) == MakeDropPropId(5, 0, 126, 41) && seq == 127 && ep == 41);
    CHECK(MintDropPropId(5, 0, seq, ep, none) == MakeDropPropId(5, 0, 127, 41) && seq == 0 && ep == 42);
    CHECK(MintDropPropId(5, 0, seq, ep, none) == MakeDropPropId(5, 0, 0, 42) && seq == 1 && ep == 42);
    // Hundreds of drops by one owner, nothing live: never the same id twice.
    std::vector<uint64_t> ids;
    seq = 0; ep = 7;
    for (int i = 0; i < 600; ++i) ids.push_back(MintDropPropId(5, uint8_t(i & 1), seq, ep, none));
    CHECK(std::count(ids.begin(), ids.end(), uint64_t(0)) == 0);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    CHECK(seq == uint8_t(600 % 128) && ep == uint32_t(7 + 600 / 128));
    // The last epoch wraps to 0 rather than past the field.
    seq = 127; ep = 0xFFFFFFFFu;
    CHECK(MintDropPropId(5, 1, seq, ep, none) == MakeDropPropId(5, 1, 127, 0xFFFFFFFFu) && seq == 0 && ep == 0);
}

static void TestDropSignature() {
    const uint64_t a = MakeDropPropId(3, 0, 0);                           // entity 3 = slot 2's fighter
    CHECK(DropSignature(a, LeaseLevel::Touch, 2));
    CHECK(!DropSignature(a, LeaseLevel::Hold, 2));                        // a pick-up is not a drop
    CHECK(!DropSignature(a, LeaseLevel::Free, 2));
    CHECK(!DropSignature(a, LeaseLevel::Touch, 1));                       // not the owner's machine
    CHECK(!DropSignature(a, LeaseLevel::Touch, kHostSlot));
    CHECK(DropSignature(MakeDropPropId(1, 1, 4), LeaseLevel::Touch, kHostSlot));               // the host's fighter
    CHECK(DropSignature(MakeDropPropId(uint8_t(kMaxFighters + 1), 0, 0), LeaseLevel::Touch, kHostSlot));   // a bot
    CHECK(!DropSignature(MakeDropPropId(uint8_t(kMaxFighters + 1), 0, 0), LeaseLevel::Touch, 1));
    CHECK(!DropSignature(MakeLevelPropId(0x0301), LeaseLevel::Touch, kHostSlot));           // level props: no stand-in
    CHECK(!DropSignature(0, LeaseLevel::Touch, kHostSlot));
}

// W5: only an id's first Grant can be its drop. A Touch Grant on an id heard of in this level (the owner stepping on a
// weapon it dropped a while ago) opens no window, so the weapon in its stand-in's hand is not forced out.
static void TestDropSignatureFirstGrantOnly() {
    const uint64_t a = MakeDropPropId(3, 0, 5, 0x77u);
    BodyStateView v;
    CHECK(!v.Known(a));
    CHECK(DropSignature(a, LeaseLevel::Touch, 2, v.Known(a)));            // the drop itself: first heard
    v.OnGrant(a, 2, LeaseLevel::Touch);
    CHECK(v.Known(a));
    v.OnGrant(a, kHostSlot, LeaseLevel::Free);                            // it came to rest and was released
    CHECK(v.Known(a));
    CHECK(!DropSignature(a, LeaseLevel::Touch, 2, v.Known(a)));           // the owner touches it again: not a drop
    BodyStateMsg s = SomeState(); s.objId = a; s.lessee = kHostSlot; s.senderTimeMs = 5;
    BodyStateView w;
    CHECK(w.Apply(s) && w.Known(a));                                      // streamed by the host: also heard of
    w.Clear();
    CHECK(!w.Known(a) && DropSignature(a, LeaseLevel::Touch, 2, w.Known(a)));   // a new level: ids start over
    BodyStateMsg early = s; early.lessee = 2;
    BodyStateView x;
    CHECK(!x.Apply(early) && !x.Known(a));                                // a state ahead of its Grant does not make it known
}

// W6: a client's lease messages count only once it reported Ready for the host's present travel.
static void TestLeaseSeqCurrent() {
    CHECK(LeaseSeqCurrent(2, 7, 7));
    CHECK(!LeaseSeqCurrent(2, 6, 7));                                     // still in the previous level
    CHECK(!LeaseSeqCurrent(2, 0, 7));                                     // never reported Ready for any travel
    CHECK(LeaseSeqCurrent(2, 0, 0));                                      // no broadcast travel yet: nothing to compare
    CHECK(LeaseSeqCurrent(kHostSlot, 0, 7));                              // the host's own slot is always current
    CHECK(!LeaseSeqCurrent(2, 8, 7));
}

static void TestResolveDropNow() {
    const uint32_t sword = 0x1234abcd, axe = 0x0badf00d;
    CHECK(!ResolveDropNow(false, 0, sword));                              // no pose from the owner yet: wait
    CHECK(ResolveDropNow(true, 0, sword));                                // the owner's hand is empty
    CHECK(!ResolveDropNow(true, sword, sword));                           // the owner still holds that weapon: wait
    CHECK(ResolveDropNow(true, axe, sword));                              // dropped it and picked up another
    CHECK(!ResolveDropNow(true, 0, 0));                                   // a copy of unknown class is never released
    CHECK(!ResolveDropNow(true, axe, 0));
}

// A lease change from the owner is a drop (two-player test 6, section 4): the stand-in of the owner holds a copy of a
// prop it already tracks, and the owner's HOLD falling to TOUCH / FREE -- or a TOUCH Grant for it -- is the owner
// letting go, heard a pose-lag earlier than the pose shows it and without the 2 s the hand matching holds the class.
static void TestDropNoticed() {
    const uint8_t owner = 2, other = 1, host = kHostSlot;
    // The owner's HOLD ends: it threw or dropped it (TOUCH: still its simulation), or the lease went FREE.
    CHECK(DropNoticed(LeaseLevel::Hold, owner, LeaseLevel::Touch, owner, owner, true));
    CHECK(DropNoticed(LeaseLevel::Hold, owner, LeaseLevel::Free, host, owner, true));
    // A TOUCH Grant for the owner's slot, from FREE or from another slot's TOUCH.
    CHECK(DropNoticed(LeaseLevel::Free, host, LeaseLevel::Touch, owner, owner, true));
    CHECK(DropNoticed(LeaseLevel::Touch, other, LeaseLevel::Touch, owner, owner, true));
    // The host's own fighter is owner slot 0 (kHostSlot): FREE is also slot 0, but only a HOLD / TOUCH names a holder.
    CHECK(DropNoticed(LeaseLevel::Hold, host, LeaseLevel::Touch, host, host, true));
    CHECK(!DropNoticed(LeaseLevel::Free, host, LeaseLevel::Free, host, host, true));
    CHECK(!DropNoticed(LeaseLevel::Touch, host, LeaseLevel::Free, host, host, true));

    // NEGATIVE CONTROL: a HOLD grant to the owner (a pick-up, or its renewal) releases nothing.
    CHECK(!DropNoticed(LeaseLevel::Free, host, LeaseLevel::Hold, owner, owner, true));
    CHECK(!DropNoticed(LeaseLevel::Touch, owner, LeaseLevel::Hold, owner, owner, true));
    CHECK(!DropNoticed(LeaseLevel::Hold, owner, LeaseLevel::Hold, owner, owner, true));
    // A TOUCH renewal is not a change.
    CHECK(!DropNoticed(LeaseLevel::Touch, owner, LeaseLevel::Touch, owner, owner, true));
    // The owner's TOUCH ending is a rest release, not a HOLD ending.
    CHECK(!DropNoticed(LeaseLevel::Touch, owner, LeaseLevel::Free, host, owner, true));
    // Another entity's grants say nothing about the owner's hand.
    CHECK(!DropNoticed(LeaseLevel::Free, host, LeaseLevel::Touch, other, owner, true));
    CHECK(!DropNoticed(LeaseLevel::Touch, other, LeaseLevel::Touch, other, owner, true));
    CHECK(!DropNoticed(LeaseLevel::Free, host, LeaseLevel::Hold, other, owner, true));
    CHECK(!DropNoticed(LeaseLevel::Touch, owner, LeaseLevel::Touch, other, owner, true));
    CHECK(!DropNoticed(LeaseLevel::Hold, other, LeaseLevel::Free, host, owner, true));      // someone else's HOLD ended
    CHECK(!DropNoticed(LeaseLevel::Hold, other, LeaseLevel::Touch, other, owner, true));
    // No stand-in of the owner holds it here: not this rule's (the existing PendingDrop path, or nothing).
    CHECK(!DropNoticed(LeaseLevel::Hold, owner, LeaseLevel::Touch, owner, owner, false));
    CHECK(!DropNoticed(LeaseLevel::Hold, owner, LeaseLevel::Free, host, owner, false));
    CHECK(!DropNoticed(LeaseLevel::Free, host, LeaseLevel::Touch, owner, owner, false));
}

// The same through the host's table and a machine's view, as PropSync::OnGrant sees them: the view's state before the
// Grant against the Grant itself. The host answers only changes, so a renewal never reaches DropNoticed at all.
static void TestDropNoticedFollowsTheTable() {
    const uint8_t owner = 2, other = 1;
    const uint64_t id = MakeWeaponPropId(3, 5);
    LeaseTable t;
    BodyStateView v;
    auto heard = [&](const LeaseResult& r) {
        const LeaseLevel pl = v.Level(id);
        const uint8_t ps = v.Streamer(id);
        v.OnGrant(id, r.slot, r.level);
        return DropNoticed(pl, ps, r.level, r.slot, owner, true);
    };
    LeaseResult r = t.Request(id, owner, LeaseLevel::Hold, 0.0);           // the owner picks it up
    CHECK(r.changed && !heard(r));
    r = t.Request(id, owner, LeaseLevel::Hold, 1000.0);                    // renewed: no Grant is sent
    CHECK(!r.changed);
    r = t.Request(id, other, LeaseLevel::Touch, 1100.0);                   // another slot touching a held prop: denied
    CHECK(!r.changed);
    r = t.Request(id, owner, LeaseLevel::Touch, 1500.0);                   // the owner lets go: HOLD -> TOUCH
    CHECK(r.changed && heard(r));
    r = t.Request(id, owner, LeaseLevel::Touch, 2500.0);                   // its renewal
    CHECK(!r.changed);
    r = t.Release(id, owner);                                              // at rest: released (TOUCH -> FREE)
    CHECK(r.changed && !heard(r));
    r = t.Request(id, other, LeaseLevel::Touch, 3000.0);                   // another slot touches it
    CHECK(r.changed && !heard(r));
    r = t.Request(id, owner, LeaseLevel::Hold, 3100.0);                    // the owner picks it up: HOLD evicts the TOUCH
    CHECK(r.changed && !heard(r));
    const std::vector<uint64_t> lapsed = t.Tick(3100.0 + kLeaseLapseMs);   // its renewals stop: the lease lapses
    CHECK(lapsed.size() == 1 && lapsed[0] == id);
    LeaseResult freed;                                                     // the host broadcasts Grant(Free, kHostSlot)
    freed.slot = kHostSlot; freed.level = LeaseLevel::Free; freed.granted = freed.changed = true;
    CHECK(heard(freed));
}

// Contact islands (two-player test 6, section 2): a prop pushed by a prop this machine streams is claimed TOUCH too, so
// one machine simulates the whole pile it disturbed instead of the host and the toucher each simulating a part.
// "Streamed here" is IslandSource: my TOUCH / HOLD lease only (a FREE prop the host streams by default is no push).
static void TestContactClaimant() {
    const uint8_t me = 3;
    CHECK(ContactClaimant(me, OtherKind::OwnedFighter, false) == me);     // a fighter simulated here: today's behaviour
    CHECK(ContactClaimant(me, OtherKind::OwnedWeapon, false) == me);      // a weapon in its hand: today's behaviour
    CHECK(ContactClaimant(me, OtherKind::Prop, true) == me);              // a prop streamed here pushed it: the island is mine
    CHECK(ContactClaimant(me, OtherKind::Prop, false) == 0);              // a prop streamed elsewhere: its streamer's island
    // NEGATIVE CONTROL: a stand-in (or its weapon) is another machine's fighter and claims nothing, whatever else is true.
    CHECK(ContactClaimant(me, OtherKind::StandIn, false) == 0);
    CHECK(ContactClaimant(me, OtherKind::StandIn, true) == 0);
    // No fighter of mine to credit: no claim, whatever touched it.
    CHECK(ContactClaimant(0, OtherKind::OwnedFighter, false) == 0);
    CHECK(ContactClaimant(0, OtherKind::Prop, true) == 0);
}

// Two-player test 10: the host claimed every FREE prop another FREE prop bumped (a stand-in knocking its copies about),
// first, so the client that pushed them lost the tie and its pushes never reached the host.
static void TestIslandSource() {
    const uint8_t me = 2, other = 1;
    CHECK(IslandSource(me, LeaseLevel::Touch, me));                       // my touch lease spreads to what it pushes
    CHECK(IslandSource(me, LeaseLevel::Hold, me));                        // so does what I hold
    CHECK(!IslandSource(kHostSlot, LeaseLevel::Free, kHostSlot));         // FREE, though the host streams it: nobody's push
    CHECK(!IslandSource(other, LeaseLevel::Touch, me));                   // another machine's lease: its island
    // NEGATIVE CONTROL: the host's own touch lease still spreads, as in test 6.
    CHECK(IslandSource(kHostSlot, LeaseLevel::Touch, kHostSlot));
}

static void TestPropStreamDue() {
    CHECK(!PropStreamDue(LeaseLevel::Hold, 0.0, false, 15.9));            // a held prop: every frame, 60 Hz at most
    CHECK(PropStreamDue(LeaseLevel::Hold, 0.0, false, 16.0));
    CHECK(PropStreamDue(LeaseLevel::Hold, 0.0, true, 16.0));              // held still is still held
    CHECK(!PropStreamDue(LeaseLevel::Touch, 0.0, false, 15.9));           // a throw or a push in flight: 60 Hz
    CHECK(PropStreamDue(LeaseLevel::Touch, 0.0, false, 16.0));
    CHECK(!PropStreamDue(LeaseLevel::Touch, 0.0, true, 999.0));           // a touched prop at rest: once a second
    CHECK(PropStreamDue(LeaseLevel::Touch, 0.0, true, 1000.0));
    CHECK(!PropStreamDue(LeaseLevel::Free, 0.0, false, 15.9));            // FREE: the host's schedule, as fast as a lease while moving
    CHECK(PropStreamDue(LeaseLevel::Free, 0.0, false, 16.0));
    CHECK(!PropStreamDue(LeaseLevel::Free, 0.0, true, 999.0));
    CHECK(PropStreamDue(LeaseLevel::Free, 0.0, true, 1000.0));
    CHECK(PropStreamDue(LeaseLevel::Touch, -1.0e9, true, 0.0));           // never sent
}

// States sent in 10 s by a stream due every periodMs, checked once a frame at fps (noise: +-noise ms of float jitter on
// alternate frames, as frame dt reads at 60 fps). carry: lastSentMs moves by NextSentMs; otherwise to nowMs (pre29).
static int SendsIn10s(double fps, double periodMs, bool carry, double noise = 0.0) {
    double last = -1e18, now = 0.0;
    int sends = 0;
    for (int f = 0; now < 10000.0; ++f) {
        now += 1000.0 / fps + ((f & 1) ? noise : -noise);
        if (now - last < periodMs) continue;
        ++sends;
        last = carry ? NextSentMs(last, periodMs, now) : now;
    }
    return sends;
}

// 0.8.0-pre29: a prop's stream rate no longer depends on the frame rate (the remainder carries, as for poses in PR 8).
static void TestPropStreamCadence() {
    CHECK(PropStreamPeriodMs(LeaseLevel::Hold, true) == kLeaseStreamMs);
    CHECK(PropStreamPeriodMs(LeaseLevel::Touch, true) == kFreeIdleMs);
    CHECK(PropStreamPeriodMs(LeaseLevel::Free, false) == kFreeStreamMs);
    const double hz = 1000.0 / kFreeStreamMs;   // 62.5
    for (double fps : {90.0, 120.0, 144.0, 165.0, 240.0}) {
        const int n = SendsIn10s(fps, kFreeStreamMs, true);
        CHECK(n >= int(hz * 10) - 2 && n <= int(hz * 10) + 2);
    }
    // Slower frames than the rate: one state a frame, never two (no catch-up burst).
    CHECK(std::abs(SendsIn10s(60.0, kFreeStreamMs, true, 0.05) - 600) <= 2);
    CHECK(std::abs(SendsIn10s(30.0, kFreeStreamMs, true) - 300) <= 2);
    // A 500 ms hitch sends one state and then one more, not thirty.
    double last = 0.0;
    CHECK(PropStreamDue(LeaseLevel::Hold, last, false, 500.0));
    last = NextSentMs(last, kLeaseStreamMs, 500.0);
    CHECK(last == 500.0 - kLeaseStreamMs);
    CHECK(PropStreamDue(LeaseLevel::Hold, last, false, 507.0));
    last = NextSentMs(last, kLeaseStreamMs, 507.0);
    CHECK(!PropStreamDue(LeaseLevel::Hold, last, false, 514.0));
    // At rest, once a second as before.
    CHECK(std::abs(SendsIn10s(144.0, kFreeIdleMs, true) - 10) <= 1);
    // NEGATIVE CONTROL: the pre29 rule (lastSentMs = nowMs) at 144 fps sends every third frame, 48 Hz.
    CHECK(SendsIn10s(144.0, kFreeStreamMs, false) <= 490);
}


// A dropped weapon lying FREE for kIdleDespawnMs goes on every machine (the lag of a pile of them).
// Spec revision (user, 2026-10-25): a fighter standing near no longer holds the minute back (fighters stand by the loot
// all wave long); the minute runs only in the bot formats (DropClearFor), counted from when the weapon went FREE.
static void TestIdleDespawn() {
    const uint64_t drop = MakeDropPropId(2, 0, 1);
    CHECK(kIdleDespawnMs == 60000.0);
    CHECK(!IdleDespawnDue(drop, 0.0, 59999.999));                        // not yet
    CHECK(IdleDespawnDue(drop, 0.0, 60000.0));                           // exactly a minute FREE: due
    CHECK(!IdleDespawnDue(drop, 30000.0, 89999.999));                    // counted from when it went FREE
    CHECK(IdleDespawnDue(drop, 30000.0, 90000.0));
    CHECK(!IdleDespawnDue(drop, -1.0, 1e9));                             // not FREE (held, touched, or no Grant yet)
    CHECK(!IdleDespawnDue(MakeLevelPropId(7), 0.0, 1e9));                // a level prop stays
}

// Loose armour pieces (dropped loot, stripped pieces) are no props: each machine times its own copies. A piece goes a
// minute after it was first seen loose; one picked up (worn or carried: not in the loose list) starts over when it lies
// loose again. The first sweep after Reset (a new match) only records what already lay loose: the level's own pieces
// stay, unless one of them is picked up and dropped again.
static void TestLooseTimer() {
    LooseTimer t;
    CHECK(t.Sweep({1, 2}, 0.0).empty());                                 // baseline: the level's pieces
    CHECK(t.Sweep({1, 2, 3}, 1000.0).empty());                           // 3 is new loot
    CHECK(t.Sweep({1, 2, 3}, 60999.999).empty());
    CHECK(t.Sweep({1, 2, 3}, 61000.0) == std::vector<uint64_t>({3}));    // a minute loose: due, then forgotten
    CHECK(t.Sweep({1, 2, 3}, 70000.0).empty());                          // reported once (destroyed by the caller)
    CHECK(t.Sweep({1, 2}, 1e9).empty());                                 // the level's pieces stay for good
    CHECK(t.Sweep({2, 4}, 2e9).empty());                                 // 1 picked up; 4 is new
    CHECK(t.Sweep({2}, 2e9 + 1000.0).empty());                           // 4 picked up within the minute...
    CHECK(t.Sweep({1, 4}, 2e9 + 2000.0).empty());                        // ...and dropped again: starts over, as does 1
    CHECK(t.Sweep({1, 4}, 2e9 + 61999.999).empty());
    CHECK(t.Sweep({1, 2, 4}, 2e9 + 62000.0) == std::vector<uint64_t>({1, 4}));   // 2 came back from the baseline: new
    CHECK(t.Sweep({2}, 2e9 + 122000.0) == std::vector<uint64_t>({2}));
    t.Reset();
    CHECK(t.Sweep({5, 6}, 3e9).empty() && t.Sweep({5, 6}, 4e9).empty()); // a new baseline
}
// M4: a slot holds at most kMaxLeasesPerSlot leases; the next fresh prop id is denied and counted, nothing is stored for
// it, other slots and renewals are unaffected, and a release or a lapse makes room again.
static void TestLeaseCapPerSlot() {
    LeaseTable t;
    for (int i = 0; i < kMaxLeasesPerSlot; ++i) CHECK(t.Request(MakeLevelPropId(uint16_t(i)), 2, LeaseLevel::Touch, 0.0).granted);
    CHECK(t.HeldBy(2) == kMaxLeasesPerSlot && t.CapDenied() == 0);
    // One more (a fresh id, TOUCH or HOLD): denied, FREE, and no entry left behind.
    const uint64_t extra = MakeLevelPropId(60000);
    LeaseResult r = t.Request(extra, 2, LeaseLevel::Touch, 1.0);
    CHECK(!r.granted && !r.changed && r.slot == kHostSlot && r.level == LeaseLevel::Free);
    r = t.Request(extra, 2, LeaseLevel::Hold, 1.0);
    CHECK(!r.granted && t.CapDenied() == 2);
    uint8_t s; LeaseLevel lv;
    CHECK(!t.Lessee(extra, s, lv) && t.Streamer(extra) == kHostSlot && t.Grants().size() == size_t(kMaxLeasesPerSlot));
    // A flood of fresh ids never grows the table past the cap.
    for (int i = 0; i < 5000; ++i) t.Request(MakeLevelPropId(uint16_t(10000 + i)), 2, LeaseLevel::Hold, 2.0);
    CHECK(t.HeldBy(2) == kMaxLeasesPerSlot && t.Grants().size() == size_t(kMaxLeasesPerSlot) && t.CapDenied() == 5002);
    // Renewing or upgrading a lease it already holds is not "one more".
    r = t.Request(MakeLevelPropId(0), 2, LeaseLevel::Touch, 3.0);
    CHECK(r.granted && !r.changed);
    r = t.Request(MakeLevelPropId(0), 2, LeaseLevel::Hold, 3.0);
    CHECK(r.granted && r.changed && r.level == LeaseLevel::Hold);
    // Taking another slot's lease by a higher level counts against the taker's cap, and the holder keeps it when denied.
    r = t.Request(MakeLevelPropId(1), 3, LeaseLevel::Hold, 4.0);   // slot 3 is empty: it takes prop 1 from slot 2
    CHECK(r.granted && r.slot == 3 && t.HeldBy(2) == kMaxLeasesPerSlot - 1 && t.HeldBy(3) == 1);
    // Other slots are not affected by slot 2's count.
    CHECK(t.Request(extra, 4, LeaseLevel::Touch, 4.0).granted);
    // Release makes room (slot 2 is at cap - 2 now, having lost prop 1 above): it can take two, then it is full again.
    CHECK(t.Release(MakeLevelPropId(5), 2).granted);
    CHECK(t.Request(extra + 1, 2, LeaseLevel::Touch, 5.0).granted && t.Request(extra + 2, 2, LeaseLevel::Touch, 5.0).granted);
    CHECK(!t.Request(extra + 6, 2, LeaseLevel::Touch, 5.0).granted && t.HeldBy(2) == kMaxLeasesPerSlot);
    // A slot full of leases cannot steal a held-by-someone-else prop by upgrading, and that prop keeps its holder.
    CHECK(t.Request(extra + 3, 5, LeaseLevel::Touch, 5.0).granted);
    r = t.Request(extra + 3, 2, LeaseLevel::Hold, 5.0);
    CHECK(!r.granted && r.slot == 5 && r.level == LeaseLevel::Touch);
    // A lapse frees the entries too.
    CHECK(t.Tick(1e6).size() > 0 && t.HeldBy(2) == 0 && t.Request(extra + 4, 2, LeaseLevel::Touch, 1e6).granted);
    // Slot 0 (the host and its bots) is capped the same way.
    LeaseTable h;
    for (int i = 0; i < kMaxLeasesPerSlot; ++i) CHECK(h.Request(MakeLevelPropId(uint16_t(i)), kHostSlot, LeaseLevel::Touch, 0.0).granted);
    CHECK(!h.Request(MakeLevelPropId(999), kHostSlot, LeaseLevel::Touch, 0.0).granted);
    // Normal play: a fighter with a weapon in each hand and a dozen touched props is nowhere near the cap.
    CHECK(kMaxLeasesPerSlot >= 64);
}

int main() {
    TestDropIdentity(); TestMintDropPropId(); TestDropEpoch(); TestDropSignature(); TestDropSignatureFirstGrantOnly(); TestLeaseSeqCurrent(); TestResolveDropNow(); TestPropStreamDue(); TestPropStreamCadence();
    TestDropNoticed(); TestDropNoticedFollowsTheTable(); TestContactClaimant(); TestIslandSource();
    TestPropStateRoundTrip(); TestPropStateRejects();
    TestPropLeaseRoundTrip(); TestPropLeaseRejects(); TestPropIdBoundsOnDecode(); TestRequestFreeIsNoOp();
    TestGrantWhenFree(); TestTouchVsTouchFirstWins(); TestHoldBeatsTouch(); TestHoldVsHoldFirstWins();
    TestRenewAndUpgrade(); TestLapseAtExactly3sBoundary(); TestRelease(); TestEntityLeft();
    TestHostOwnedBots(); TestDuplicateRequestFromSameSlotIsIdempotent(); TestLeaseCapPerSlot();
    TestBodyStateViewStaleAndDuplicateRejected(); TestBodyStateViewWrapAround(); TestBodyStateViewFollowsGrants();
    TestLeaseStreamerAndAccepts(); TestGrantsForLateJoiner(); TestLeaseFromPeer();
    TestClaimTouchLifecycle(); TestClaimHoldThrowSettle(); TestClaimContactWindowAndRestReset(); TestPropAtRestAndFreeSchedule();
    TestIdentityScheme(); TestIdleDespawn(); TestLooseTimer();
    std::printf(g_fail ? "%d FAILED\n" : "all lease tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
