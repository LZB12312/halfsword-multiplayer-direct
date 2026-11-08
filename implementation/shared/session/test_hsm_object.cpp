// Generic objects (session/hsm_object.h, rev 26): the three id spaces never collide, minting, body index, ObjectAnnounce
// and BodyState wire (round trip, every truncation, rejections), the host's proposal reconciler (two proposers, one id), ghosts.
#include "session/hsm_object.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static const uint64_t kBit63 = uint64_t(1) << 63, kBit62 = uint64_t(1) << 62;

static void TestIdSpaces() {
    CHECK(MakeSpawnedId(0, 1) == (kBit62 | 1));
    CHECK(MakeSpawnedId(3, 7) == (kBit62 | (uint64_t(3) << 56) | 7));
    CHECK(MakeSpawnedId(uint8_t(kMaxFighters), 1) == 0 && MakeSpawnedId(63, 1) == 0);
    CHECK(MakeSpawnedId(uint8_t(kMaxFighters - 1), 0xFFFFFFFFu) != 0);
    CHECK(SpawnedIdSlot(MakeSpawnedId(5, 9)) == 5 && SpawnedIdSeq(MakeSpawnedId(5, 9)) == 9);
    CHECK(SpawnedIdSlot(MakeWeaponPropId(3, 4)) == 0xFF && SpawnedIdSeq(MakeLevelPropId(2)) == 0);
    // Boundaries of each kind.
    CHECK(ValidObjectId(MakeSpawnedId(0, 0)) && ValidObjectId(MakeSpawnedId(uint8_t(kMaxFighters - 1), 0xFFFFFFFFu)));
    CHECK(!ValidObjectId(0));
    CHECK(!ValidObjectId(kBit62 | (uint64_t(kMaxFighters) << 56)));          // a slot past the last
    CHECK(!ValidObjectId(kBit62 | (uint64_t(63) << 56)));
    for (int bit = 32; bit <= 55; ++bit) CHECK(!ValidObjectId(MakeSpawnedId(1, 5) | (uint64_t(1) << bit)));   // bits 32-55 clear
    CHECK(ValidObjectId(kBit63) && ValidObjectId(kBit63 | kBit62) && ValidObjectId(~uint64_t(0)));   // level ids: any hash
    CHECK(ValidObjectId(MakeLevelPropId(0)) && ValidObjectId(MakeLevelPropId(0xFFFF)));
    CHECK(ValidObjectId(MakeWeaponPropId(1, 0)) && ValidObjectId(MakeWeaponPropId(uint8_t(kMaxWillies), 255)));
    CHECK(!ValidObjectId(MakeWeaponPropId(0, 3)) && !ValidObjectId(MakeWeaponPropId(uint8_t(kMaxWillies + 1), 3)));
    // Classification is exclusive: every id is exactly one kind, told apart by bits 63/62 alone.
    const uint64_t samples[] = {1, 0x0102, MakeWeaponPropId(3, 4), MakeDropPropId(19, 1, 127, 0xFFFFFFFFu), kBit63, kBit63 | 5,
                                kBit63 | kBit62, MakeLevelPropId(77), kBit62, MakeSpawnedId(0, 1), MakeSpawnedId(8, 99),
                                MakeSpawnedId(uint8_t(kMaxFighters - 1), 0xFFFFFFFFu), ~uint64_t(0)};
    for (uint64_t id : samples) {
        const int kinds = int(IsWeaponId(id)) + int(IsLevelProp(id)) + int(IsSpawnedId(id));
        CHECK(kinds == 1);
        CHECK(IsSpawnedId(id) == ((id & kBit62) != 0 && (id & kBit63) == 0));
        CHECK(IsLevelProp(id) == ((id & kBit63) != 0));
    }
    // A grid straddling every boundary: no id of one kind equals an id of another, and all are valid.
    std::set<uint64_t> seen;
    int made = 0;
    for (uint8_t owner = 1; owner <= kMaxWillies; ++owner)
        for (int low : {0, 1, 255}) { seen.insert(MakeWeaponPropId(owner, uint8_t(low))); ++made; }
    for (int idx : {0, 1, 0xFFFF}) { seen.insert(MakeLevelPropId(uint16_t(idx))); ++made; }
    for (uint8_t slot = 0; slot < kMaxFighters; ++slot)
        for (uint32_t seq : {0u, 1u, 0xFFFFFFFFu}) { seen.insert(MakeSpawnedId(slot, seq)); ++made; }
    CHECK(int(seen.size()) == made);
    for (uint64_t id : seen) CHECK(ValidObjectId(id));
    // The weapon helpers mean nothing for a non-weapon id.
    CHECK(PropOwnerEntity(MakeSpawnedId(2, 3)) == 0 && PropSheathSlot(MakeSpawnedId(2, 3)) == 0 && PropDropEpoch(MakeSpawnedId(2, 3)) == 0);
    CHECK(!DropSignature(MakeSpawnedId(1, 1), LeaseLevel::Touch, 0));
    CHECK(PropOwnerEntity(MakeWeaponPropId(3, 4)) == 3);
}

static void TestMinter() {
    SpawnedIdMinter m(3);
    CHECK(m.Slot() == 3 && m.NextSeq() == 1);
    const uint64_t a = m.Mint(), b = m.Mint();
    CHECK(a == MakeSpawnedId(3, 1) && b == MakeSpawnedId(3, 2) && a != b);
    CHECK(SpawnedIdSlot(a) == 3 && ValidObjectId(a));
    m.Observe(MakeSpawnedId(3, 40));                       // seen from this slot: skip past it
    CHECK(m.Mint() == MakeSpawnedId(3, 41));
    m.Observe(MakeSpawnedId(3, 10));                       // an old one changes nothing
    CHECK(m.Mint() == MakeSpawnedId(3, 42));
    m.Observe(MakeSpawnedId(4, 500));                      // another slot's id
    m.Observe(MakeLevelPropId(900)); m.Observe(MakeWeaponPropId(3, 3)); m.Observe(0);
    CHECK(m.Mint() == MakeSpawnedId(3, 43));
    // Different slots never mint the same id.
    SpawnedIdMinter h(0), c(1);
    std::set<uint64_t> ids;
    for (int i = 0; i < 100; ++i) { ids.insert(h.Mint()); ids.insert(c.Mint()); }
    CHECK(ids.size() == 200 && !ids.count(0));
    // A bad slot mints nothing; the sequence ends at 2^32-1.
    CHECK(SpawnedIdMinter(uint8_t(kMaxFighters)).Mint() == 0);
    SpawnedIdMinter e(2, 0xFFFFFFFEu);
    CHECK(e.Mint() == MakeSpawnedId(2, 0xFFFFFFFEu) && e.Mint() == MakeSpawnedId(2, 0xFFFFFFFFu) && e.Mint() == 0 && e.Mint() == 0);
    SpawnedIdMinter o(2); o.Observe(MakeSpawnedId(2, 0xFFFFFFFFu)); CHECK(o.Mint() == 0);
    SpawnedIdMinter z(2, 0); CHECK(z.Mint() == MakeSpawnedId(2, 1));   // seq 0 is never handed out
}

static void TestBodyIndex() {
    BodyIndex<int> b;
    b.Build(100, {7, 8, 9});
    CHECK(b.Count() == 4 && b.IndexOf(100) == 0 && b.IndexOf(7) == 1 && b.IndexOf(8) == 2 && b.IndexOf(9) == 3);
    CHECK(b.IndexOf(5) == -1);
    CHECK(b.KeyAt(0) && *b.KeyAt(0) == 100 && *b.KeyAt(3) == 9 && b.KeyAt(4) == nullptr && b.KeyAt(255) == nullptr);
    b.Build(1, {2, 1, 2, 3});                              // the root and repeats keep their first place
    CHECK(b.Count() == 3 && b.IndexOf(1) == 0 && b.IndexOf(2) == 1 && b.IndexOf(3) == 2);
    b.Build(1, {});
    CHECK(b.Count() == 1 && b.IndexOf(1) == 0);
    std::vector<int> many; for (int i = 10; i < 200; ++i) many.push_back(i);
    b.Build(0, many);
    CHECK(b.Count() == kMaxObjectBodies && b.IndexOf(10) == 1 && b.IndexOf(10 + kMaxObjectBodies - 2) == kMaxObjectBodies - 1);
    CHECK(b.IndexOf(10 + kMaxObjectBodies - 1) == -1);     // past the cap
}

static ObjectAnnounceMsg Proposal(uint8_t slot, uint32_t seq, uint32_t cls, float x, float y, float z) {
    ObjectAnnounceMsg m; m.objId = MakeSpawnedId(slot, seq); m.flags = kAnnounceProposal; m.classHash = cls;
    m.px = x; m.py = y; m.pz = z; m.bodyCount = 3; m.parentId = MakeLevelPropId(4);
    return m;
}

static void TestAnnounceWire() {
    ObjectAnnounceMsg p = Proposal(2, 5, 0xABCD1234u, 10.5f, -20.f, 300.f);
    const auto b = Encode(p);
    CHECK(b.size() == 46);
    ObjectAnnounceMsg o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.objId == p.objId && o.flags == kAnnounceProposal && o.classHash == p.classHash && o.px == 10.5f && o.py == -20.f && o.pz == 300.f);
    CHECK(o.bodyCount == 3 && o.parentId == p.parentId && o.proposalId == 0);
    // Every truncation, and a trailing byte.
    for (size_t n = 0; n < b.size(); ++n) { ObjectAnnounceMsg t; CHECK(!Decode(b.data(), n, t)); }
    auto longer = b; longer.push_back(0); CHECK(!Decode(longer.data(), longer.size(), o));
    // Canonical answers: any valid id, with the proposal it answers.
    ObjectAnnounceMsg c = p; c.flags = kAnnounceCanonical; c.objId = MakeSpawnedId(0, 9); c.proposalId = p.objId;
    const auto cb = Encode(c);
    CHECK(Decode(cb.data(), cb.size(), o) && o.flags == kAnnounceCanonical && o.proposalId == p.objId && o.objId == c.objId);
    ObjectAnnounceMsg c2 = c; c2.objId = MakeLevelPropId(3); c2.proposalId = 0; c2.parentId = 0;
    const auto c2b = Encode(c2); CHECK(Decode(c2b.data(), c2b.size(), o));
    ObjectAnnounceMsg c3 = c2; c3.objId = MakeDropPropId(3, 1, 5); const auto c3b = Encode(c3); CHECK(Decode(c3b.data(), c3b.size(), o));
    // Rejections, one field at a time.
    auto bad = [](const ObjectAnnounceMsg& m) { const auto e = Encode(m); ObjectAnnounceMsg x; return !Decode(e.data(), e.size(), x); };
    auto good = [](const ObjectAnnounceMsg& m) { const auto e = Encode(m); ObjectAnnounceMsg x; return Decode(e.data(), e.size(), x); };
    ObjectAnnounceMsg m = p;
    m.objId = 0; CHECK(bad(m)); m = p;
    m.objId = MakeWeaponPropId(3, 4); CHECK(bad(m));                       // a proposal's id is a provisional spawned one
    m.objId = MakeLevelPropId(1); CHECK(bad(m)); m = p;
    m.objId = kBit62 | (uint64_t(1) << 40); CHECK(bad(m)); m = p;            // bits 32-55
    m.objId = kBit62 | (uint64_t(kMaxFighters) << 56) | 1; CHECK(bad(m)); m = p;
    m.flags = 0; CHECK(bad(m)); m.flags = 4; CHECK(bad(m)); m.flags = 3; CHECK(bad(m)); m.flags = kAnnounceProposal | 0x80; CHECK(bad(m));
    m.flags = 0xFF; CHECK(bad(m)); m = p;
    m.classHash = 0; CHECK(bad(m)); m = p;
    m.px = std::nanf(""); CHECK(bad(m)); m.px = p.px; m.py = INFINITY; CHECK(bad(m)); m.py = p.py; m.pz = -INFINITY; CHECK(bad(m)); m = p;
    m.px = kMaxPropCoord * 2; CHECK(bad(m)); m.px = kMaxPropCoord; CHECK(good(m)); m = p;
    m.bodyCount = 0; CHECK(bad(m)); m.bodyCount = kMaxAnnounceBodies + 1; CHECK(bad(m));
    m.bodyCount = kMaxAnnounceBodies; CHECK(good(m)); m.bodyCount = 1; CHECK(good(m)); m = p;
    m.parentId = m.objId; CHECK(bad(m)); m.parentId = kBit62 | (uint64_t(1) << 33); CHECK(bad(m)); m.parentId = 0; CHECK(good(m)); m = p;
    m.proposalId = MakeSpawnedId(1, 1); CHECK(bad(m));                        // only a canonical answer names a proposal
    m = c; m.proposalId = MakeLevelPropId(1); CHECK(bad(m)); m.proposalId = c.objId; CHECK(bad(m)); m.proposalId = kBit62 | (uint64_t(1) << 40); CHECK(bad(m));
    // A wrong type or revision byte.
    auto wrong = b; wrong[3] = uint8_t(MsgType::BodyState); CHECK(!Decode(wrong.data(), wrong.size(), o));
    wrong = b; wrong[2] = uint8_t(kProtocolRevision - 1); CHECK(!Decode(wrong.data(), wrong.size(), o));
    CHECK(b[3] == 45);
}

static void TestBodyStateWire() {
    BodyStateMsg m; m.objId = MakeSpawnedId(2, 7); m.body = 5; m.lessee = 2; m.senderTimeMs = 99;
    m.px = 1; m.py = 2; m.pz = 3; m.rot = PackQuat(0, 0.1f, 0, 0.99f); m.vx = 4; m.vy = 5; m.vz = 6; m.wx = 7; m.wy = 8; m.wz = 9;
    const auto b = Encode(m);
    CHECK(b.size() == 62 && b[3] == 46);
    BodyStateMsg o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.objId == m.objId && o.body == 5 && o.lessee == 2 && o.senderTimeMs == 99 && o.px == 1 && o.vz == 6 && o.wz == 9);
    CHECK(o.rot.x == m.rot.x && o.rot.y == m.rot.y && o.rot.z == m.rot.z && o.rot.w == m.rot.w);
    for (size_t n = 0; n < b.size(); ++n) { BodyStateMsg t; CHECK(!Decode(b.data(), n, t)); }
    auto longer = b; longer.push_back(1); CHECK(!Decode(longer.data(), longer.size(), o));
    auto bad = [](BodyStateMsg x) { const auto e = Encode(x); BodyStateMsg y; return !Decode(e.data(), e.size(), y); };
    BodyStateMsg x = m;
    x.body = kMaxObjectBodies; CHECK(bad(x)); x.body = kMaxObjectBodies - 1; CHECK(!bad(x)); x = m;
    x.lessee = kMaxFighters; CHECK(bad(x)); x = m;
    x.objId = 0; CHECK(bad(x)); x.objId = kBit62 | (uint64_t(1) << 50); CHECK(bad(x));
    x = m; x.objId = MakeLevelPropId(5); CHECK(!bad(x)); x.objId = MakeDropPropId(3, 1, 5); CHECK(!bad(x));
}

static void TestBodyStateViewPerBody() {
    BodyStateView v;
    const uint64_t id = MakeSpawnedId(0, 1);
    BodyStateMsg a; a.objId = id; a.body = 0; a.lessee = kHostSlot; a.senderTimeMs = 100; a.px = 1;
    BodyStateMsg b = a; b.body = 1; b.px = 2;
    CHECK(v.Apply(a) && v.Apply(b));                       // bodies are independent
    CHECK(!v.Apply(a) && !v.Apply(b));                     // duplicates
    CHECK(v.Find(id, 0) && v.Find(id, 0)->px == 1 && v.Find(id, 1) && v.Find(id, 1)->px == 2 && v.Find(id, 2) == nullptr);
    BodyStateMsg b2 = b; b2.senderTimeMs = 110; b2.px = 3;
    CHECK(v.Apply(b2) && v.Find(id, 0)->px == 1 && v.Find(id, 1)->px == 3);   // body 1 advances, body 0 untouched
    // A grant moves the stream: both bodies' former-streamer times are remembered.
    v.OnGrant(id, 2, LeaseLevel::Touch);
    BodyStateMsg h = a; h.senderTimeMs = 120; CHECK(!v.Apply(h));          // the host no longer streams it
    BodyStateMsg l0 = a; l0.lessee = 2; l0.senderTimeMs = 5; l0.px = 9; CHECK(v.Apply(l0));
    v.OnGrant(id, 0, LeaseLevel::Free);                    // back to the host (A-B-A): its old per-body times stand
    BodyStateMsg late = b; late.senderTimeMs = 105; CHECK(!v.Apply(late));   // older than body 1's 110
    BodyStateMsg next = b; next.senderTimeMs = 111; CHECK(v.Apply(next));
}

static void TestReconciler() {
    ObjectReconciler r;
    const uint32_t kCls = 0x1111u;
    // A client's proposal: no match, so a canonical id in the host's slot.
    auto p1 = Proposal(2, 1, kCls, 100, 200, 300);
    auto a = r.Propose(p1, 2);
    CHECK(a.objId == MakeSpawnedId(0, 1) && a.created && SpawnedIdSlot(a.objId) == kHostSlot);
    CHECK(a.announce.flags == kAnnounceCanonical && a.announce.objId == a.objId && a.announce.proposalId == p1.objId);
    CHECK(a.announce.classHash == kCls && a.announce.px == 100 && a.announce.bodyCount == 3 && a.announce.parentId == p1.parentId);
    { const auto e = Encode(a.announce); ObjectAnnounceMsg x; CHECK(Decode(e.data(), e.size(), x)); }
    // The same object proposed by a second client (a different provisional id, a bit off): the same id, nothing new.
    auto p2 = Proposal(3, 1, kCls, 130, 190, 300);
    auto b = r.Propose(p2, 3);
    CHECK(b.objId == a.objId && !b.created && b.announce.proposalId == p2.objId && r.Size() == 1);
    // A retransmit of either gets the same answer.
    CHECK(r.Propose(p1, 2).objId == a.objId && !r.Propose(p1, 2).created && r.Propose(p2, 3).objId == a.objId && r.Size() == 1);
    // A different class, or too far away: a new object.
    auto p3 = Proposal(3, 2, 0x2222u, 100, 200, 300);
    auto c = r.Propose(p3, 3); CHECK(c.created && c.objId == MakeSpawnedId(0, 2));
    auto p4 = Proposal(2, 2, kCls, 100 + kCensusMatchUu + 50, 200, 300);
    auto d = r.Propose(p4, 2); CHECK(d.created && d.objId == MakeSpawnedId(0, 3));
    CHECK(r.Size() == 3);
    // Exactly at the tolerance matches; just past does not.
    ObjectReconciler e;
    const uint64_t home = e.SpawnLocal(kCls, 100, 200, 300, 1);
    CHECK(e.Propose(Proposal(2, 3, kCls, 100 + kCensusMatchUu, 200, 300), 2).objId == home);
    CHECK(e.Propose(Proposal(2, 4, kCls, 100 + kCensusMatchUu + 1.f, 200, 300), 2).created);
    CHECK(e.Propose(Proposal(2, 5, kCls, 100, 200 - kCensusMatchUu, 300), 2).objId == home);
    CHECK(e.Propose(Proposal(2, 6, kCls, 100, 200, 300 + kCensusMatchUu + 1.f), 2).created);
    // Nearest known wins when two are in reach.
    ObjectReconciler q;
    const uint64_t n1 = q.SpawnLocal(kCls, 0, 0, 0, 1), n2 = q.SpawnLocal(kCls, 60, 0, 0, 1);
    CHECK(n1 != n2 && SpawnedIdSlot(n1) == kHostSlot);
    CHECK(q.Propose(Proposal(1, 1, kCls, 45, 0, 0), 1).objId == n2);
    CHECK(q.Propose(Proposal(1, 2, kCls, 20, 0, 0), 1).objId == n1);
    // The object moved: UpdatePlace lets a late proposal find it where it is now.
    q.UpdatePlace(n1, 1000, 0, 0);
    CHECK(q.Propose(Proposal(1, 3, kCls, 1010, 0, 0), 1).objId == n1);
    CHECK(q.Propose(Proposal(1, 4, kCls, 5, 0, 0), 1).objId == n2);   // the old place now finds the other one
    q.UpdatePlace(n2, 5000, 0, 0);
    CHECK(q.Propose(Proposal(1, 5, kCls, -500, 0, 0), 1).created);    // nothing left near it: a new object
    // Refusals: a forged slot, a canonical flag, bad fields, a bad slot.
    ObjectReconciler s;
    CHECK(s.Propose(Proposal(2, 1, kCls, 0, 0, 0), 3).objId == 0);                 // provisional id is slot 2's, sent by slot 3
    ObjectAnnounceMsg cn = Proposal(2, 1, kCls, 0, 0, 0); cn.flags = kAnnounceCanonical; CHECK(s.Propose(cn, 2).objId == 0);
    ObjectAnnounceMsg z = Proposal(2, 1, 0, 0, 0, 0); CHECK(s.Propose(z, 2).objId == 0);
    z = Proposal(2, 1, kCls, 0, 0, 0); z.bodyCount = 0; CHECK(s.Propose(z, 2).objId == 0);
    z.bodyCount = kMaxAnnounceBodies + 1; CHECK(s.Propose(z, 2).objId == 0);
    z = Proposal(2, 1, kCls, std::nanf(""), 0, 0); CHECK(s.Propose(z, 2).objId == 0);
    z = Proposal(2, 1, kCls, 0, 0, 0); z.objId = MakeLevelPropId(3); CHECK(s.Propose(z, 2).objId == 0);
    CHECK(s.Size() == 0);
    // Removal forgets the object and its answers; an id is never reused.
    ObjectReconciler t;
    auto first = t.Propose(Proposal(1, 1, kCls, 0, 0, 0), 1);
    t.Remove(first.objId); CHECK(!t.Knows(first.objId) && t.Size() == 0);
    auto again = t.Propose(Proposal(1, 1, kCls, 0, 0, 0), 1);
    CHECK(again.created && again.objId != first.objId);
    // Late joiners get every known object as a canonical announce.
    ObjectReconciler u;
    u.Propose(Proposal(1, 1, kCls, 0, 0, 0), 1); u.SpawnLocal(0x3333u, 5, 6, 7, 2);
    const auto all = u.AnnounceAll();
    CHECK(all.size() == 2);
    for (const auto& m : all) { CHECK(m.flags == kAnnounceCanonical && m.proposalId == 0); const auto e = Encode(m); ObjectAnnounceMsg x; CHECK(Decode(e.data(), e.size(), x)); }
    ObjectAnnounceMsg one; CHECK(u.AnnounceOf(all[0].objId, one) && one.objId == all[0].objId && !u.AnnounceOf(MakeSpawnedId(0, 999), one));
    CHECK(u.SpawnLocal(0, 0, 0, 0, 1) == 0 && u.SpawnLocal(1, 0, 0, 0, 0) == 0 && u.SpawnLocal(1, std::nanf(""), 0, 0, 1) == 0);
    // The table is bounded.
    ObjectReconciler full;
    for (size_t i = 0; i < kMaxKnownObjects; ++i) CHECK(full.SpawnLocal(9, float(i) * 1000.f, 0, 0, 1) != 0);
    CHECK(full.SpawnLocal(9, 0, 5000, 0, 1) == 0 && full.Propose(Proposal(1, 1, 7, 0, 0, 0), 1).objId == 0);
    CHECK(full.Propose(Proposal(1, 2, 9, 0, 0, 0), 1).objId != 0);     // but a match to a known object still answers
    // A host in another slot (after a migration) mints in its own slot.
    ObjectReconciler other(4);
    CHECK(other.Propose(Proposal(2, 1, kCls, 0, 0, 0), 2).objId == MakeSpawnedId(4, 1));
}

static void TestGhosts() {
    GhostTable g;
    const uint64_t id = MakeSpawnedId(0, 1);
    CHECK(g.Note(id, 0) && g.IsGhost(id) && g.Size() == 1);
    CHECK(g.Expire(9999).empty() && g.IsGhost(id));
    CHECK(g.Note(id, 5000));                                   // refreshed
    CHECK(g.Expire(14999).empty() && g.Expire(15000).size() == 1 && !g.IsGhost(id));
    CHECK(g.Note(id, 0) && g.Resolve(id) && !g.Resolve(id) && g.Size() == 0);
    for (size_t i = 0; i < kMaxGhosts; ++i) CHECK(g.Note(MakeSpawnedId(0, uint32_t(i + 1)), 0));
    CHECK(!g.Note(MakeSpawnedId(1, 1), 0) && g.Note(MakeSpawnedId(0, 1), 1) && g.Size() == kMaxGhosts);
    g.Clear(); CHECK(g.Size() == 0);
}

static void TestMotionHelpers() {
    CHECK(SpeedOf(3, 4, 0) == 5.f && SpeedOf(0, 0, 0) == 0.f);
    CHECK(SpeedOf(std::nanf(""), 0, 0) == 0.f);
    CHECK(SpeedOf(1e9f, 0, 0) == kMaxPropSpeed);
    CHECK(ProposalTolUu(100.f, 0.f) == 100.f && ProposalTolUu(100.f, -5.f) == 100.f);
    CHECK(ProposalTolUu(100.f, 500.f) == 100.f + 500.f * kMotionSlackS);
    CHECK(ProposalTolUu(100.f, 1e6f) == 100.f + kMotionTolCapUu);
    float x = 0, y = 0, z = 0;
    ExtrapolatePlace(x, y, z, 1000.f, 0, -500.f, 200.0);
    CHECK(std::fabs(x - 200.f) < 0.01f && y == 0.f && std::fabs(z + 100.f) < 0.01f);
    x = y = z = 0; ExtrapolatePlace(x, y, z, 1000.f, 0, 0, 5000.0);          // bounded
    CHECK(std::fabs(x - 1000.f * float(kExtrapolateMaxMs / 1000.0)) < 0.01f);
    x = y = z = 0; ExtrapolatePlace(x, y, z, 1000.f, 0, 0, 0.0); CHECK(x == 0.f);
    ExtrapolatePlace(x, y, z, 1000.f, 0, 0, -50.0); CHECK(x == 0.f);
    ExtrapolatePlace(x, y, z, 1e30f, 0, 0, 500.0); CHECK(x == 0.f);          // off the map: unchanged
}

static void TestReconcilerMotion() {
    const uint32_t kCls = 0x1111u;
    // Two clients propose one fast object: the host's copy is 200 cm/s along x, last read at t=1000.
    ObjectReconciler r;
    const uint64_t id = r.SpawnLocal(kCls, 0, 0, 0, 1);
    r.UpdateMotion(id, 0, 0, 0, 2000.f, 0, 0, 1000.0);
    // At t=1400 it is 800 cm on; a proposal there matches, with and without the trip's own error.
    CHECK(r.Propose(Proposal(2, 1, kCls, 800, 0, 0), 2, 1400.0).objId == id);
    CHECK(r.Propose(Proposal(3, 1, kCls, 700, 0, 0), 3, 1400.0).objId == id);   // within 100 + 0.4*2000 capped at 500
    CHECK(r.Size() == 1);
    // Without a clock nothing is carried forward: the old behaviour (a miss at 800).
    ObjectReconciler r2; const uint64_t i2 = r2.SpawnLocal(kCls, 0, 0, 0, 1); r2.UpdateMotion(i2, 0, 0, 0, 2000.f, 0, 0, 1000.0);
    CHECK(r2.Propose(Proposal(2, 1, kCls, 800, 0, 0), 2).created);
    // Extrapolation is bounded: ten seconds later the object is not assumed to be 20000 cm away.
    ObjectReconciler r3; const uint64_t i3 = r3.SpawnLocal(kCls, 0, 0, 0, 1); r3.UpdateMotion(i3, 0, 0, 0, 2000.f, 0, 0, 0.0);
    CHECK(r3.Propose(Proposal(2, 1, kCls, 20000, 0, 0), 2, 10000.0).created);
    CHECK(r3.Propose(Proposal(2, 2, kCls, 1200, 0, 0), 2, 10000.0).objId == i3);   // the bounded 600 ms of travel
    // The tolerance growth is capped: a far miss stays a miss.
    ObjectReconciler r4; const uint64_t i4 = r4.SpawnLocal(kCls, 0, 0, 0, 1); r4.UpdateMotion(i4, 0, 0, 0, 3000.f, 0, 0, 0.0);
    CHECK(r4.Propose(Proposal(2, 1, kCls, 1800.f + kCensusMatchUu + kMotionTolCapUu + 10.f, 0, 0), 2, 600.0).created);
    // A resting object keeps the plain tolerance whatever the clock.
    ObjectReconciler r5; const uint64_t i5 = r5.SpawnLocal(kCls, 0, 0, 0, 1); r5.UpdateMotion(i5, 0, 0, 0, 0, 0, 0, 0.0);
    CHECK(r5.Propose(Proposal(2, 1, kCls, kCensusMatchUu + 1.f, 0, 0), 2, 100.0).created);
    // Nearest wins when two moving objects are in reach.
    ObjectReconciler r6;
    const uint64_t a = r6.SpawnLocal(kCls, 0, 0, 0, 1), b = r6.SpawnLocal(kCls, 300, 0, 0, 1);
    r6.UpdateMotion(a, 0, 0, 0, 1000.f, 0, 0, 0.0); r6.UpdateMotion(b, 300, 0, 0, 1000.f, 0, 0, 0.0);
    CHECK(r6.Propose(Proposal(2, 1, kCls, 460, 0, 0), 2, 300.0).objId == b);     // a at 300, b at 600
    CHECK(r6.Propose(Proposal(2, 2, kCls, 320, 0, 0), 2, 300.0).objId == a);
    // Another class never matches however close.
    CHECK(r6.Propose(Proposal(2, 3, 0x9999u, 300, 0, 0), 2, 300.0).created);
    // UpdatePlace drops the velocity; UpdateMotion refuses unknown ids, bad places, non-finite velocity.
    ObjectReconciler r7; const uint64_t i7 = r7.SpawnLocal(kCls, 0, 0, 0, 1);
    r7.UpdateMotion(i7, 0, 0, 0, 2000.f, 0, 0, 0.0);
    r7.UpdatePlace(i7, 0, 0, 0);
    CHECK(r7.Propose(Proposal(2, 1, kCls, 800, 0, 0), 2, 400.0).created);
    ObjectReconciler r8; const uint64_t i8 = r8.SpawnLocal(kCls, 0, 0, 0, 1);
    r8.UpdateMotion(MakeSpawnedId(0, 99), 5, 5, 5, 1, 1, 1, 0.0);
    r8.UpdateMotion(i8, std::nanf(""), 0, 0, 1, 0, 0, 0.0);
    r8.UpdateMotion(i8, 0, 0, 0, std::nanf(""), 0, 0, 0.0);
    r8.UpdateMotion(i8, 0, 0, 0, 0, std::numeric_limits<float>::infinity(), 0, 0.0);
    ObjectAnnounceMsg out; CHECK(r8.AnnounceOf(i8, out) && out.px == 0 && out.py == 0 && !r8.Knows(MakeSpawnedId(0, 99)));
    CHECK(r8.Propose(Proposal(2, 1, kCls, 300, 0, 0), 2, 400.0).created);       // nothing moving was recorded
}

// WouldCreate decides as Propose does, motion included: the known place is carried forward to nowMs and matched with the
// speed-grown tolerance. The spawn gate asks it before Propose, so a disagreement would gate a match as a spawn.
static void TestWouldCreateMotion() {
    const uint32_t kCls = 0x1111u;
    ObjectReconciler r;
    const uint64_t id = r.SpawnLocal(kCls, 0, 0, 0, 1);
    r.UpdateMotion(id, 0, 0, 0, 2000.f, 0, 0, 1000.0);                       // 2000 cm/s along x, last read at t=1000
    CHECK(!r.WouldCreate(Proposal(2, 1, kCls, 800, 0, 0), 1400.0));          // at t=1400 it is 800 cm on
    CHECK(!r.WouldCreate(Proposal(2, 1, kCls, 1300, 0, 0), 1400.0));         // 500 off: within 100 + 0.4*2000 capped at 500
    CHECK(r.WouldCreate(Proposal(2, 1, kCls, 1401, 0, 0), 1400.0));          // 601 off: past the grown tolerance
    CHECK(r.WouldCreate(Proposal(2, 1, kCls, 800, 0, 0), 1000.0));           // no time passed: still at 0, 800 is out of reach
    CHECK(r.WouldCreate(Proposal(2, 1, kCls, 800, 0, 0), -1.0));             // no clock: nothing carried forward
    CHECK(r.WouldCreate(Proposal(2, 1, kCls, 20000, 0, 0), 11000.0));        // bounded: ten seconds on it is not 20000 cm away
    CHECK(!r.WouldCreate(Proposal(2, 1, kCls, 1200, 0, 0), 11000.0));        // the bounded 600 ms of travel
    CHECK(r.WouldCreate(Proposal(2, 1, 0x9999u, 800, 0, 0), 1400.0));        // another class never matches
    // Agreement with Propose: a match is answered, not created, and an answered proposal is never a spawn again.
    CHECK(!r.Propose(Proposal(2, 2, kCls, 1300, 0, 0), 2, 1400.0).created);
    CHECK(!r.WouldCreate(Proposal(2, 2, kCls, 1300, 0, 0), -1.0));           // answered, whatever the clock
    CHECK(r.Propose(Proposal(2, 3, kCls, 5000, 0, 0), 2, 1400.0).created);
    CHECK(!r.WouldCreate(Proposal(2, 3, kCls, 5000, 0, 0), 1400.0));         // answered too
    // A resting object keeps the plain tolerance whatever the clock.
    ObjectReconciler r2; const uint64_t i2 = r2.SpawnLocal(kCls, 0, 0, 0, 1); r2.UpdateMotion(i2, 0, 0, 0, 0, 0, 0, 0.0);
    CHECK(!r2.WouldCreate(Proposal(2, 1, kCls, kCensusMatchUu, 0, 0), 100.0));
    CHECK(r2.WouldCreate(Proposal(2, 1, kCls, kCensusMatchUu + 1.f, 0, 0), 100.0));
}

static void TestSetBodyCount() {
    ObjectReconciler r(0);
    ObjectAnnounceMsg p = Proposal(2, 1, 0x2222u, 100, 0, 0);
    p.bodyCount = 3;                                          // what the proposer counted
    const auto m = r.Propose(p, 2);
    CHECK(m.created);
    ObjectAnnounceMsg out;
    CHECK(r.AnnounceOf(m.objId, out) && out.bodyCount == 3);
    r.SetBodyCount(m.objId, 22);                              // the host's actor has 22
    CHECK(r.AnnounceOf(m.objId, out) && out.bodyCount == 22);
    r.SetBodyCount(m.objId, 0);                               // NEGATIVE CONTROL: no count to take
    CHECK(r.AnnounceOf(m.objId, out) && out.bodyCount == 22);
    r.SetBodyCount(MakeSpawnedId(0, 999), 7);                 // an object it does not know: nothing happens
    CHECK(!r.Knows(MakeSpawnedId(0, 999)));
}

static void TestAdopt() {
    const uint32_t kCls = 0x1111u;
    // An old host's canonical announces, as every machine heard them.
    ObjectAnnounceMsg a1; a1.objId = MakeSpawnedId(0, 7); a1.flags = kAnnounceCanonical; a1.classHash = kCls;
    a1.px = 100; a1.py = 0; a1.pz = 0; a1.bodyCount = 2; a1.proposalId = MakeSpawnedId(2, 1);
    ObjectAnnounceMsg a2 = a1; a2.objId = MakeSpawnedId(3, 4); a2.px = 5000; a2.proposalId = 0;   // minted in a client slot
    ObjectReconciler r(1);                                  // the new host's slot
    CHECK(r.Adopt(a1) && r.Adopt(a2) && r.Size() == 2 && r.Knows(a1.objId));
    // A late proposal for an adopted object gets its id, not a new one.
    auto m = r.Propose(Proposal(2, 5, kCls, 130, 0, 0), 2);
    CHECK(m.objId == a1.objId && !m.created && r.Size() == 2);
    // A retransmit of the proposal that was answered by the old host gets that answer.
    CHECK(r.Propose(Proposal(2, 1, kCls, 9999, 9999, 0), 2).objId == a1.objId);
    // New mints do not collide with anything adopted, in any slot.
    ObjectReconciler s(0);
    ObjectAnnounceMsg h = a1; h.objId = MakeSpawnedId(0, 40); h.proposalId = 0;
    CHECK(s.Adopt(h));
    CHECK(s.SpawnLocal(kCls, 9000, 0, 0, 1) == MakeSpawnedId(0, 41));
    ObjectReconciler t(2);
    ObjectAnnounceMsg own = a1; own.objId = MakeSpawnedId(2, 12); own.proposalId = 0;   // an id of the new host's own slot
    CHECK(t.Adopt(own) && t.SpawnLocal(kCls, 9000, 0, 0, 1) == MakeSpawnedId(2, 13));
    ObjectAnnounceMsg others = a1; others.objId = MakeSpawnedId(3, 90); others.proposalId = 0;
    t.Adopt(others);
    CHECK(t.SpawnLocal(kCls, 9500, 0, 0, 1) == MakeSpawnedId(2, 14));              // another slot's ids do not move the minter
    // AnnounceAll re-announces the adopted ids.
    const auto all = r.AnnounceAll();
    CHECK(all.size() == 2);
    bool seen1 = false, seen2 = false;
    for (const auto& x : all) { seen1 |= x.objId == a1.objId; seen2 |= x.objId == a2.objId; CHECK(x.flags == kAnnounceCanonical && x.proposalId == 0); }
    CHECK(seen1 && seen2);
    // Adopting an id again replaces it.
    ObjectAnnounceMsg moved = a1; moved.px = 700; CHECK(r.Adopt(moved) && r.Size() == 2);
    CHECK(r.Propose(Proposal(3, 8, kCls, 650, 0, 0), 3).objId == a1.objId);
    // Refusals.
    ObjectReconciler u;
    ObjectAnnounceMsg bad = a1;
    bad.flags = kAnnounceProposal; CHECK(!u.Adopt(bad));
    bad = a1; bad.objId = MakeLevelPropId(3); CHECK(!u.Adopt(bad));
    bad = a1; bad.objId = 0; CHECK(!u.Adopt(bad));
    bad = a1; bad.classHash = 0; CHECK(!u.Adopt(bad));
    bad = a1; bad.bodyCount = 0; CHECK(!u.Adopt(bad));
    bad = a1; bad.bodyCount = kMaxAnnounceBodies + 1; CHECK(!u.Adopt(bad));
    bad = a1; bad.px = std::nanf(""); CHECK(!u.Adopt(bad));
    bad = a1; bad.parentId = bad.objId; CHECK(!u.Adopt(bad));
    bad = a1; bad.parentId = 12345; CHECK(!u.Adopt(bad));
    CHECK(u.Size() == 0);
    // A full table refuses a new object but still replaces a known one.
    ObjectReconciler full;
    for (size_t i = 0; i < kMaxKnownObjects; ++i) CHECK(full.SpawnLocal(9, float(i) * 1000.f, 0, 0, 1) != 0);
    CHECK(!full.Adopt(a2));
    ObjectAnnounceMsg known = a1; known.objId = MakeSpawnedId(0, 1); known.classHash = 9; known.proposalId = 0;
    CHECK(full.Adopt(known) && full.Size() == kMaxKnownObjects);
}

int main() {
    TestIdSpaces(); TestMinter(); TestBodyIndex(); TestAnnounceWire(); TestBodyStateWire(); TestBodyStateViewPerBody();
    TestReconciler(); TestGhosts();
    TestMotionHelpers(); TestReconcilerMotion(); TestWouldCreateMotion(); TestAdopt(); TestSetBodyCount();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("all object tests passed\n");
    return 0;
}
