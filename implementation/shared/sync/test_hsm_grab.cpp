// Grabs across machines (sync/hsm_grab.h). The GrabState codec round-trips exactly and rejects every malformed form:
// grabber or target out of range, a hold on the grabber itself, a held flag other than 0/1, a hold without a grab id,
// an empty, over-long or non-identifier bone, a non-finite or too-distant grip, trailing or missing bytes, another type or revision.
// The grabber's sender sends a change at once (and only then a new grab id), refreshes a hold every 100 ms, never
// sends while nothing is held, and treats an invalid hold as none. The victim's inbox keeps only the newest state per
// grabber (an older state is never taken back, however long the silence; a grabber that left or respawned starts
// afresh, every grab it made before retired for the lease time) and pulls only fresh, valid holds on its own fighters
// from living grabbers; a hold that broke free, or whose target was replaced (DropTarget), stays dead until a new grab. The pull law rests at equilibrium, converges
// without overshoot, follows a moving hand with no lag, is bounded by stiffness x lead, capped per step, stable at
// any frame time, and inert on non-finite input.
#include "sync/hsm_grab.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using hsm::sync::GrabInbox;
using hsm::sync::GrabPull;
using hsm::sync::GrabPullDv;
using hsm::sync::GrabPullParams;
using hsm::sync::GrabSend;
using hsm::sync::GrabSender;
using hsm::sync::V3;

static double Len(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
static bool Near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
static GrabHand Hold(uint8_t target, uint32_t id, const std::string& bone, V3 grip) {
    GrabHand h; h.held = true; h.target = target; h.grabId = id; h.bone = bone; h.grip = grip; return h;
}

static GrabStateMsg SomeState() {
    GrabStateMsg m;
    m.grabber = 2; m.seq = 77;
    m.hands[0] = Hold(5, 9, "lowerarm_l", V3{ 1.5, -2.25, 10 });
    return m;
}

// ---------------------------------------------------------------- codec

static void TestRoundTrip() {
    CHECK(uint8_t(MsgType::GrabState) == 27);                   // 26 and 29 are the sever messages; the wire id is part of rev 9
    GrabStateMsg m = SomeState();
    m.hands[1] = Hold(3, 0xFFFFFFF0u, "spine_03", V3{ -40, 20.5, 0.125 });
    std::vector<uint8_t> b = Encode(m);
    CHECK(b.size() == 4 + 1 + 4 + (1 + 1 + 4 + 1 + 10 + 12) + (1 + 1 + 4 + 1 + 8 + 12));
    GrabStateMsg o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.grabber == 2 && o.seq == 77);
    CHECK(o.hands[0].held && o.hands[0].target == 5 && o.hands[0].grabId == 9 && o.hands[0].bone == "lowerarm_l");
    CHECK(o.hands[0].grip.x == 1.5 && o.hands[0].grip.y == -2.25 && o.hands[0].grip.z == 10);
    CHECK(o.hands[1].held && o.hands[1].target == 3 && o.hands[1].grabId == 0xFFFFFFF0u && o.hands[1].bone == "spine_03");
    CHECK(o.hands[1].grip.x == -40 && o.hands[1].grip.y == 20.5 && o.hands[1].grip.z == 0.125);

    // A free hand is one byte, and decodes to an empty hand whatever the decoder's struct held before.
    m = SomeState(); m.hands[0] = GrabHand{};
    b = Encode(m);
    CHECK(b.size() == 4 + 1 + 4 + 1 + 1);
    o = SomeState(); o.hands[1] = Hold(4, 1, "head", V3{ 1, 1, 1 });
    CHECK(Decode(b.data(), b.size(), o));
    for (const GrabHand& h : o.hands) CHECK(!h.held && h.target == 0 && h.grabId == 0 && h.bone.empty() && Len(h.grip) == 0);

    // Both hands on one body (a two-handed hold), the largest entity and the wire's grip bound itself are fine.
    m = SomeState(); m.grabber = uint8_t(kMaxWillies); m.hands[1] = Hold(5, 10, "lowerarm_l", V3{ 0, 0, -60 });
    m.hands[0].grip = V3{ 60, 0, 0 };
    b = Encode(m);
    CHECK(Decode(b.data(), b.size(), o) && o.hands[0].target == 5 && o.hands[1].target == 5 && o.hands[1].grip.z == -60);
    m.hands[0].bone = std::string(kMaxGrabBone, 'b');
    b = Encode(m);
    CHECK(Decode(b.data(), b.size(), o) && o.hands[0].bone.size() == kMaxGrabBone);
}

static void TestRejects() {
    auto bad = [](const GrabStateMsg& m) { const auto b = Encode(m); GrabStateMsg o; return !Decode(b.data(), b.size(), o); };
    CHECK(!bad(SomeState()));
    GrabStateMsg m = SomeState(); m.grabber = 0; CHECK(bad(m));
    m = SomeState(); m.grabber = uint8_t(kMaxWillies + 1); CHECK(bad(m));
    m = SomeState(); m.hands[0].target = 0; CHECK(bad(m));
    m = SomeState(); m.hands[0].target = uint8_t(kMaxWillies + 1); CHECK(bad(m));
    m = SomeState(); m.hands[0].target = m.grabber; CHECK(bad(m));                  // a hold on oneself
    m = SomeState(); m.hands[1] = Hold(2, 4, "head", V3{}); CHECK(bad(m));         // ... by either hand
    m = SomeState(); m.hands[0].grabId = 0; CHECK(bad(m));
    m = SomeState(); m.hands[0].bone.clear(); CHECK(bad(m));
    // A bone is an identifier: nothing else can be interned as a name on the receiving machine.
    for (const char* name : { "lower arm", "lowerarm.l", "lowerarm-l", "lowerarm/l", "\xFF" })
        { m = SomeState(); m.hands[0].bone = name; CHECK(bad(m)); }
    m = SomeState(); m.hands[1] = Hold(4, 3, std::string("ab\0cd", 5), V3{}); CHECK(bad(m));       // an embedded NUL
    m = SomeState(); m.hands[0].bone = "Bone_09"; CHECK(!bad(m));
    m = SomeState(); m.hands[0].grip.x = std::numeric_limits<double>::quiet_NaN(); CHECK(bad(m));
    m = SomeState(); m.hands[1] = Hold(4, 3, "head", V3{ 0, std::numeric_limits<double>::infinity(), 0 }); CHECK(bad(m));
    m = SomeState(); m.hands[0].grip = V3{ 50, 40, 0 }; CHECK(bad(m));              // 64 uu: farther than any hand holds
    m = SomeState(); m.hands[0].grip = V3{ 0, 0, 60.5 }; CHECK(bad(m));

    const std::vector<uint8_t> good = Encode(SomeState());
    CHECK(good.size() == 4 + 1 + 4 + (1 + 1 + 4 + 1 + 10 + 12) + 1);
    if (good.size() != 39) return;                                                          // the forgeries below need it
    GrabStateMsg o;
    std::vector<uint8_t> b = good; b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));   // trailing byte
    b = good; b.pop_back(); CHECK(!Decode(b.data(), b.size(), o));                          // missing byte
    for (size_t n = 0; n < good.size(); ++n) CHECK(!Decode(good.data(), n, o));             // every truncation
    b = good; b[3] = uint8_t(MsgType::Pose); CHECK(!Decode(b.data(), b.size(), o));         // another type's header
    b = good; b[2] = uint8_t(kProtocolRevision - 1); CHECK(!Decode(b.data(), b.size(), o)); // another revision
    b = good; b[9] = 2; CHECK(!Decode(b.data(), b.size(), o));                              // held is 0 or 1 ...
    b = good; b.back() = 3; CHECK(!Decode(b.data(), b.size(), o));                          // ... on either hand

    // An over-long bone cannot be encoded (Writer::str truncates), so it is forged: length byte at 4+1+4+1+1+4.
    GrabStateMsg longBone = SomeState(); longBone.hands[0].bone = std::string(kMaxGrabBone, 'b');
    b = Encode(longBone);
    CHECK(b.size() == 39 - 10 + kMaxGrabBone);
    if (b.size() != 39 - 10 + kMaxGrabBone) return;
    CHECK(b[15] == kMaxGrabBone);
    b[15] = uint8_t(kMaxGrabBone + 1); b.insert(b.begin() + 16, uint8_t('b'));
    CHECK(!Decode(b.data(), b.size(), o));
    std::vector<uint8_t> big(kMaxPacket + 1, 0);                                            // nothing past a packet
    std::copy(good.begin(), good.end(), big.begin());
    CHECK(!Decode(big.data(), big.size(), o));
}

static void TestBoneNames() {
    // Every byte, against the plain definition: letters, digits and underscore, in any position.
    for (int c = 0; c < 256; ++c) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        const std::string one(1, char(c));
        CHECK(ValidBoneName(one) == ok);
        CHECK(ValidBoneName("b" + one + "b") == ok);
    }
    for (const char* real : { "pelvis", "spine_03", "lowerarm_l", "clavicle_r", "thigh_l", "head", "neck_02", "hand_r" })
        CHECK(ValidBoneName(real));
    CHECK(!ValidBoneName(""));
    CHECK(ValidBoneName(std::string(kMaxGrabBone, 'b')));
    CHECK(!ValidBoneName(std::string(kMaxGrabBone + 1, 'b')));
    CHECK(!ValidBoneName(std::string(2 * kMaxGrabBone, 'b')));
}

// ---------------------------------------------------------------- the grabber's sender

static std::array<GrabHand, kGrabHands> Reads(GrabHand r = {}, GrabHand l = {}) { return { r, l }; }

static void TestSender() {
    GrabSender s;
    GrabStateMsg out;
    CHECK(s.Update(2, Reads(), 0, out) == GrabSend::None);                        // nothing held: nothing sent, ever
    CHECK(s.Update(2, Reads(), 10000, out) == GrabSend::None);

    CHECK(s.Update(2, Reads(Hold(5, 0, "lowerarm_l", V3{ 100, 0, 0 })), 10000, out) == GrabSend::Change);
    CHECK(out.grabber == 2 && out.seq == 1);
    const uint32_t first = out.hands[0].grabId;
    CHECK(out.hands[0].held && out.hands[0].target == 5 && first != 0 && out.hands[0].bone == "lowerarm_l");
    CHECK(Near(out.hands[0].grip.x, sync::kGripMaxUu) && out.hands[0].grip.y == 0);   // clamped to 55 uu
    CHECK(!out.hands[1].held);
    { const auto b = Encode(out); GrabStateMsg o; CHECK(Decode(b.data(), b.size(), o)); }   // what it sends decodes

    // The same hold: a moving grip is no change; it goes out with the next refresh, 100 ms after the last send.
    CHECK(s.Update(2, Reads(Hold(5, 0, "lowerarm_l", V3{ 1, 2, 3 })), 10010, out) == GrabSend::None);
    CHECK(s.Update(2, Reads(Hold(5, 0, "lowerarm_l", V3{ 1, 2, 3 })), 10099, out) == GrabSend::None);
    CHECK(s.Update(2, Reads(Hold(5, 0, "lowerarm_l", V3{ 1, 2, 3 })), 10100, out) == GrabSend::Refresh);
    CHECK(out.seq == 2 && out.hands[0].grabId == first && out.hands[0].grip.z == 3);
    CHECK(s.Update(2, Reads(Hold(5, 0, "lowerarm_l", V3{ 1, 2, 3 })), 10150, out) == GrabSend::None);

    // Another bone, another body: a change at once, and a new grab.
    CHECK(s.Update(2, Reads(Hold(5, 0, "upperarm_l", V3{})), 10160, out) == GrabSend::Change);
    const uint32_t second = out.hands[0].grabId;
    CHECK(out.seq == 3 && second != 0 && second != first && out.hands[0].bone == "upperarm_l");
    CHECK(s.Update(2, Reads(Hold(6, 0, "upperarm_l", V3{})), 10170, out) == GrabSend::Change);
    const uint32_t third = out.hands[0].grabId;
    CHECK(out.seq == 4 && third != second && third != first && out.hands[0].target == 6);

    // The left hand joins: a change; the right hand keeps its grab.
    CHECK(s.Update(2, Reads(Hold(6, 0, "upperarm_l", V3{}), Hold(6, 0, "head", V3{})), 10180, out) == GrabSend::Change);
    CHECK(out.hands[0].grabId == third && out.hands[1].held && out.hands[1].grabId != 0 && out.hands[1].grabId != third);

    // Let go: a change with canonical free hands, then silence (no refresh of nothing).
    CHECK(s.Update(2, Reads(), 10190, out) == GrabSend::Change);
    CHECK(out.seq == 6);
    for (const GrabHand& h : out.hands) CHECK(!h.held && h.target == 0 && h.grabId == 0 && h.bone.empty() && Len(h.grip) == 0);
    CHECK(s.Update(2, Reads(), 10400, out) == GrabSend::None);
    CHECK(s.Update(2, Reads(), 20000, out) == GrabSend::None);

    // The same body and bone again is a new grab (an earlier break-free does not carry over).
    CHECK(s.Update(2, Reads(Hold(5, 0, "lowerarm_l", V3{})), 20010, out) == GrabSend::Change);
    CHECK(out.hands[0].grabId != 0 && out.hands[0].grabId != first && out.hands[0].grabId != second && out.hands[0].grabId != third);

    // Reset (the fighter was replaced): the next hold is a change; seq keeps growing so receivers accept it.
    s.Reset();
    CHECK(s.Update(2, Reads(Hold(5, 0, "lowerarm_l", V3{})), 20020, out) == GrabSend::Change && out.seq == 8);

    // Invalid holds count as free hands.
    GrabSender t;
    CHECK(t.Update(2, Reads(Hold(2, 0, "head", V3{})), 0, out) == GrabSend::None);                       // itself
    CHECK(t.Update(2, Reads(Hold(0, 0, "head", V3{})), 0, out) == GrabSend::None);                       // nobody
    CHECK(t.Update(2, Reads(Hold(uint8_t(kMaxWillies + 1), 0, "head", V3{})), 0, out) == GrabSend::None);
    CHECK(t.Update(2, Reads(GrabHand{ false, 5, 0, "head", V3{} }), 0, out) == GrabSend::None);          // not held
    CHECK(t.Update(2, Reads(Hold(5, 0, "", V3{})), 0, out) == GrabSend::None);                           // no bone
    CHECK(t.Update(2, Reads(Hold(5, 0, "lower arm", V3{})), 0, out) == GrabSend::None);                  // not an identifier
    CHECK(t.Update(2, Reads(Hold(5, 0, "\xFF", V3{})), 0, out) == GrabSend::None);
    CHECK(t.Update(2, Reads(Hold(5, 0, std::string(kMaxGrabBone + 1, 'b'), V3{})), 0, out) == GrabSend::None);
    CHECK(t.Update(2, Reads(Hold(5, 0, "head", V3{ std::numeric_limits<double>::quiet_NaN(), 0, 0 })), 0, out) == GrabSend::None);
    CHECK(t.Update(2, Reads(Hold(5, 0, "head", V3{}), Hold(2, 0, "head", V3{})), 0, out) == GrabSend::Change);
    CHECK(out.hands[0].held && !out.hands[1].held);

    // SameHolds: grip and seq are no change; held, target, bone and grab id are.
    GrabStateMsg a = SomeState(), c = a;
    c.seq = 99; c.hands[0].grip = V3{ 9, 9, 9 }; CHECK(sync::SameHolds(a, c));
    c = a; c.hands[0].target = 6; CHECK(!sync::SameHolds(a, c));
    c = a; c.hands[0].bone = "head"; CHECK(!sync::SameHolds(a, c));
    c = a; c.hands[0].grabId = 10; CHECK(!sync::SameHolds(a, c));
    c = a; c.hands[1] = Hold(5, 1, "head", V3{}); CHECK(!sync::SameHolds(a, c));
    c = a; c.grabber = 3; CHECK(!sync::SameHolds(a, c));
}

// ---------------------------------------------------------------- the victim's inbox

static void TestInbox() {
    const auto mine = [](uint8_t e) { return e == 5; };
    const auto alive = [](uint8_t) { return false; };
    GrabInbox in;
    GrabStateMsg m = SomeState();                                // grabber 2's right hand on my 5's lower arm
    CHECK(in.Pulls(0, mine, alive).empty());
    CHECK(in.Accept(m, 1000));
    std::vector<GrabPull> p = in.Pulls(1000, mine, alive);
    CHECK(p.size() == 1);
    if (p.size() == 1) {
        CHECK(p[0].grabber == 2 && p[0].hand == 0 && p[0].target == 5 && p[0].grabId == 9 && p[0].bone == "lowerarm_l");
        CHECK(p[0].grip.x == 1.5 && p[0].grip.y == -2.25 && p[0].grip.z == 10);
    }
    CHECK(in.Pulls(1000, [](uint8_t e) { return e == 6; }, alive).empty());   // not a fighter of mine
    CHECK(in.Pulls(1000, mine, [](uint8_t e) { return e == 2; }).empty());     // the grabber is dead
    CHECK(in.Pulls(1000 + sync::kGrabLeaseMs, mine, alive).size() == 1);       // fresh up to the lease ...
    CHECK(in.Pulls(1000 + sync::kGrabLeaseMs + 1, mine, alive).empty());      // ... then released

    // Only the newest state of a grabber counts: repeats and late ones are dropped, wrap-aware.
    GrabStateMsg free = m; free.seq = 78; free.hands[0] = GrabHand{};
    CHECK(!in.Accept(m, 1100));
    GrabStateMsg older = m; older.seq = 76; CHECK(!in.Accept(older, 1100));
    CHECK(in.Accept(free, 1100));
    CHECK(in.Pulls(1100, mine, alive).empty());                  // let go
    CHECK(!in.Accept(m, 1200));                                  // the late hold does not come back
    CHECK(in.Pulls(1200, mine, alive).empty());
    // Silence is the normal state after a let-go, so it never lets an older state back in: a hold delayed on the link
    // past the lease (a duplicate or reordered refresh) stays dead, however late it arrives.
    CHECK(!in.Accept(m, 1100 + sync::kGrabLeaseMs + 1));
    CHECK(!in.Accept(older, 5000));
    CHECK(in.Pulls(5000, mine, alive).empty());
    // A grabber whose machine restarted its counter has left first (the session forgets it), and then starts afresh.
    // Grab 9 was made before the Forget, so it is retired for the lease time; after that a restarted counter's grab 9
    // is its own (a rejoin takes longer than that anyway).
    GrabStateMsg restart = m; restart.seq = 1;
    CHECK(!in.Accept(restart, 6000));
    in.Forget(2, 6000);
    CHECK(in.Accept(restart, 6000));
    CHECK(in.Pulls(6000, mine, alive).empty());
    CHECK(in.Pulls(6000 + sync::kGrabLeaseMs, mine, alive).size() == 1);
    GrabInbox wrap;
    GrabStateMsg wrapA = m; wrapA.seq = 0xFFFFFFFEu; CHECK(wrap.Accept(wrapA, 0));
    GrabStateMsg wrapB = m; wrapB.seq = 0; CHECK(wrap.Accept(wrapB, 10));
    CHECK(!wrap.Accept(wrapA, 20));
    wrapA.seq = 0xFFFFFFFFu; CHECK(!wrap.Accept(wrapA, 30));

    // Two grabbers, two hands each on different fighters: every hold on mine pulls.
    GrabInbox two;
    GrabStateMsg g3; g3.grabber = 3; g3.seq = 5;
    g3.hands[0] = Hold(5, 1, "head", V3{}); g3.hands[1] = Hold(6, 2, "head", V3{});
    CHECK(two.Accept(SomeState(), 0) && two.Accept(g3, 0));
    CHECK(two.Pulls(0, mine, alive).size() == 2);
    CHECK(two.Pulls(0, [](uint8_t e) { return e == 5 || e == 6; }, alive).size() == 3);
    two.Forget(3, 0);                                             // grabber 3 left or respawned
    CHECK(two.Pulls(0, mine, alive).size() == 1);
    GrabStateMsg g3old = g3; g3old.seq = 1; CHECK(two.Accept(g3old, 10));   // its next stream starts afresh ...
    CHECK(two.Pulls(10, mine, alive).size() == 1);                // ... but grabs 1 and 2, made before, stay retired
    GrabStateMsg g3new = g3old; g3new.seq = 2; g3new.hands[0].grabId = 21; g3new.hands[1].grabId = 22;
    CHECK(two.Accept(g3new, 10));                                 // its new grabs
    two.Forget(5, 10);                                            // a target leaving drops nothing of the grabbers
    CHECK(two.Pulls(10, mine, alive).size() == 2);
    two.Clear();
    CHECK(two.Pulls(10, mine, alive).empty());
    CHECK(!two.Accept(GrabStateMsg{}, 10));                       // no grabber: never accepted
}

static void TestBreakFree() {
    const auto mine = [](uint8_t e) { return e == 5; };
    const auto alive = [](uint8_t) { return false; };
    GrabInbox in;
    GrabStateMsg m = SomeState();
    CHECK(in.Accept(m, 0));
    const std::vector<GrabPull> ps = in.Pulls(0, mine, alive);
    CHECK(ps.size() == 1);
    if (ps.size() != 1) return;
    const GrabPull p = ps[0];
    // Over 80 uu, then back under: the timer starts over.
    CHECK(sync::kGrabBreakUu == 80 && sync::kGrabBreakMs == 300);
    CHECK(!in.NoteDistance(p, 81, 0));
    CHECK(!in.NoteDistance(p, 81, 200));
    CHECK(!in.NoteDistance(p, 79, 250));
    CHECK(!in.NoteDistance(p, 81, 400));
    CHECK(!in.NoteDistance(p, 81, 699));
    CHECK(in.Pulls(699, mine, alive).size() == 1);
    CHECK(!in.NoteDistance(p, sync::kGrabBreakUu, 1101));        // exactly 80 is not past it
    CHECK(!in.NoteDistance(p, 81, 1101));
    CHECK(!in.NoteDistance(p, 81, 1400));
    CHECK(in.NoteDistance(p, 81, 1401));                          // 300 ms past 80 uu: broke free
    CHECK(in.Pulls(1401, mine, alive).empty());
    CHECK(in.NoteDistance(p, 0, 1402));                           // and stays broken, however close
    m.seq = 78; CHECK(in.Accept(m, 1410));                         // the grabber's refreshes of that grab
    CHECK(in.Pulls(1410, mine, alive).empty());
    m.seq = 79; m.hands[1] = Hold(5, 1, "head", V3{}); CHECK(in.Accept(m, 1420));
    std::vector<GrabPull> left = in.Pulls(1420, mine, alive);      // the other hand is another grab
    CHECK(left.size() == 1 && left.at(0).hand == 1);
    m.seq = 80; m.hands[0].grabId = 11; CHECK(in.Accept(m, 1430));   // a new grab of the broken hand pulls again
    CHECK(in.Pulls(1430, mine, alive).size() == 2);
    const std::vector<GrabPull> both = in.Pulls(1430, mine, alive);
    CHECK(both.size() == 2);
    if (both.size() != 2) return;
    const GrabPull fresh = both[0];
    CHECK(fresh.grabId == 11 && !in.NoteDistance(fresh, 131, 1430));
    in.Forget(2, 1430);
    CHECK(in.Pulls(1430, mine, alive).empty());
}

// The grabber was replaced (it respawned, or left): the session forgets it. Its stream starts afresh, but every grab it
// made so far -- any id up to the highest heard, held now or let go of -- is retired for kGrabLeaseMs: refreshes its
// old body sent before its machine saw the respawn are still on the way. A new grab (a higher id) pulls at once.
static void TestForgetRetiresHolds() {
    const auto mine = [](uint8_t e) { return e == 5; };
    const auto alive = [](uint8_t) { return false; };
    GrabInbox in;
    GrabStateMsg m = SomeState();                                // grabber 2 on my 5: grab 9, seq 77
    CHECK(in.Accept(m, 900));
    CHECK(in.Pulls(900, mine, alive).size() == 1);
    in.Forget(2, 1000);                                          // grabber 2 respawned
    CHECK(in.Pulls(1000, mine, alive).empty());
    m.seq = 78; CHECK(in.Accept(m, 1100));                       // a refresh its old body sent: taken ...
    CHECK(in.Pulls(1100, mine, alive).empty());                  // ... and not pulled
    GrabStateMsg next = m; next.seq = 79; next.hands[0].grabId = 10;   // its new body's grab
    CHECK(in.Accept(next, 1200));
    const std::vector<GrabPull> p = in.Pulls(1200, mine, alive);
    CHECK(p.size() == 1 && p.at(0).grabId == 10);
    GrabStateMsg let = next; let.seq = 80; let.hands[0] = GrabHand{};   // and its let-go lands as usual
    CHECK(in.Accept(let, 1300));
    CHECK(in.Pulls(1300, mine, alive).empty());

    // The retirement ends kGrabLeaseMs after the Forget, to the millisecond.
    const double until = 1000 + sync::kGrabLeaseMs;
    GrabInbox late;
    GrabStateMsg o = SomeState();
    CHECK(late.Accept(o, 900));
    late.Forget(2, 1000);
    o.seq = 78; CHECK(late.Accept(o, until - 100));
    CHECK(late.Pulls(until - 1, mine, alive).empty());
    CHECK(late.Pulls(until, mine, alive).size() == 1);

    // Every id up to the highest is retired, whichever hand held it.
    GrabInbox both;
    GrabStateMsg b = SomeState(); b.hands[1] = Hold(5, 4, "head", V3{});   // grabs 9 (right) and 4 (left)
    CHECK(both.Accept(b, 0));
    CHECK(both.Pulls(0, mine, alive).size() == 2);
    both.Forget(2, 10);
    b.seq = 78; CHECK(both.Accept(b, 20));
    CHECK(both.Pulls(20, mine, alive).empty());

    // A grab let go of before the Forget is retired too: an older refresh of it, reordered past the let-go and the
    // Forget, is taken (the stream starts afresh) but never pulled.
    GrabInbox re;
    const GrabStateMsg h9 = SomeState();
    CHECK(re.Accept(h9, 0));
    GrabStateMsg go = h9; go.seq = 78; go.hands[0] = GrabHand{};
    CHECK(re.Accept(go, 10));
    re.Forget(2, 20);
    CHECK(re.Accept(h9, 30));
    CHECK(re.Pulls(30, mine, alive).empty());

    // Forgotten twice before anything is heard again (a Despawn, then a leave): the first retirement stands.
    GrabInbox twice;
    CHECK(twice.Accept(SomeState(), 0));
    twice.Forget(2, 100);
    twice.Forget(2, 200);
    GrabStateMsg r = SomeState(); r.seq = 78; CHECK(twice.Accept(r, 300));
    CHECK(twice.Pulls(300, mine, alive).empty());

    // Forgotten again while a retirement runs, after a new grab was heard (respawned twice within kGrabLeaseMs): the
    // new grab is retired too, and the old ones still are.
    GrabInbox inside;
    CHECK(inside.Accept(SomeState(), 0));                        // grab 9
    inside.Forget(2, 100);                                       // up to 9, until 1600
    GrabStateMsg n10 = SomeState(); n10.seq = 1; n10.hands[0].grabId = 10;
    CHECK(inside.Accept(n10, 200));                              // its new body's grab 10
    CHECK(inside.Pulls(200, mine, alive).size() == 1);
    inside.Forget(2, 300);                                       // respawned again: up to 10, until 1800
    n10.seq = 2; CHECK(inside.Accept(n10, 400));
    CHECK(inside.Pulls(400, mine, alive).empty());
    GrabStateMsg o9 = SomeState(); o9.seq = 3; CHECK(inside.Accept(o9, 500));
    CHECK(inside.Pulls(500, mine, alive).empty());

    // After a retirement is over, a later Forget retires only what was heard since: a restarted machine's grab 1, not
    // the grab 9 of the machine before it, so the restarted machine's grab 2 pulls at once.
    GrabInbox again;
    CHECK(again.Accept(SomeState(), 0));                         // grab 9
    again.Forget(2, 100);                                        // left: up to 9, until 1600
    GrabStateMsg s1 = SomeState(); s1.seq = 1; s1.hands[0].grabId = 1;
    CHECK(again.Accept(s1, 2000));                               // the restarted machine's grab 1
    CHECK(again.Pulls(2000, mine, alive).size() == 1);
    again.Forget(2, 2100);                                       // it respawned: grab 1 is retired
    s1.seq = 2; CHECK(again.Accept(s1, 2200));
    CHECK(again.Pulls(2200, mine, alive).empty());
    GrabStateMsg s2 = s1; s2.seq = 3; s2.hands[0].grabId = 2;
    CHECK(again.Accept(s2, 2300));
    CHECK(again.Pulls(2300, mine, alive).size() == 1);

    // A retirement is per grabber: grabber 3's hold pulls throughout grabber 2's.
    GrabInbox other;
    GrabStateMsg g3; g3.grabber = 3; g3.seq = 5; g3.hands[0] = Hold(5, 1, "head", V3{});
    CHECK(other.Accept(SomeState(), 0) && other.Accept(g3, 0));
    other.Forget(2, 10);
    const std::vector<GrabPull> o3 = other.Pulls(20, mine, alive);
    CHECK(o3.size() == 1 && o3.at(0).grabber == 3);
    CHECK(!other.Accept(GrabStateMsg{}, 30));                    // no grabber: never accepted
    other.Forget(0, 30);                                         // nor forgotten
    other.Forget(uint8_t(kMaxWillies + 1), 30);
    CHECK(other.Pulls(30, mine, alive).size() == 1);
}

// A fighter of mine was replaced (it respawned): a hold on its old body is stale, whatever the grabber still sends.
static void TestDropTarget() {
    const auto mine = [](uint8_t e) { return e == 5 || e == 6; };
    const auto alive = [](uint8_t) { return false; };
    const auto onlyTargets = [](const std::vector<GrabPull>& p) {   // "hand:target" per pull, in order
        std::string s;
        for (const GrabPull& x : p) s += std::to_string(x.hand) + ":" + std::to_string(x.target) + " ";
        return s;
    };
    GrabInbox in;
    GrabStateMsg m = SomeState();                                // grabber 2: right hand on my 5 (grab 9) ...
    m.hands[1] = Hold(6, 4, "head", V3{});                       // ... left hand on my 6 (grab 4)
    CHECK(in.Accept(m, 0));
    CHECK(onlyTargets(in.Pulls(0, mine, alive)) == "0:5 1:6 ");
    in.DropTarget(5);                                            // 5 respawned
    CHECK(onlyTargets(in.Pulls(0, mine, alive)) == "1:6 ");      // only the hold on 5 is gone
    m.seq = 78; CHECK(in.Accept(m, 100));                        // the grabber's refresh of that very grab
    CHECK(onlyTargets(in.Pulls(100, mine, alive)) == "1:6 ");    // does not bring it back
    m.seq = 79; m.hands[0].grabId = 10; CHECK(in.Accept(m, 200));   // a new grab of the new body pulls
    CHECK(onlyTargets(in.Pulls(200, mine, alive)) == "0:5 1:6 ");
    m.seq = 80; CHECK(in.Accept(m, 300));
    CHECK(onlyTargets(in.Pulls(300, mine, alive)) == "0:5 1:6 ");   // and keeps pulling

    // A fighter held by both hands, by two grabbers: every hold on it goes, nothing else does.
    GrabInbox two;
    GrabStateMsg a = SomeState(); a.hands[1] = Hold(5, 11, "head", V3{});
    GrabStateMsg b; b.grabber = 3; b.seq = 5; b.hands[0] = Hold(5, 1, "spine_03", V3{}); b.hands[1] = Hold(6, 2, "head", V3{});
    CHECK(two.Accept(a, 0) && two.Accept(b, 0));
    CHECK(two.Pulls(0, mine, alive).size() == 4);
    two.DropTarget(5);
    CHECK(onlyTargets(two.Pulls(0, mine, alive)) == "1:6 ");
    two.DropTarget(6);
    CHECK(two.Pulls(0, mine, alive).empty());

    // Before any hold, and for a fighter nobody holds: nothing to drop, and a hold that comes later pulls.
    GrabInbox fresh;
    fresh.DropTarget(5); fresh.DropTarget(0); fresh.DropTarget(uint8_t(kMaxWillies + 1)); fresh.DropTarget(255);
    CHECK(fresh.Accept(SomeState(), 0));
    CHECK(fresh.Pulls(0, mine, alive).size() == 1);
    fresh.DropTarget(6); fresh.DropTarget(0); fresh.DropTarget(uint8_t(kMaxWillies + 1));
    CHECK(fresh.Pulls(0, mine, alive).size() == 1);

    // A free hand is no hold, whatever its stale fields say: dropping its old target leaves a later hold alone.
    GrabInbox stale;
    GrabStateMsg s = SomeState(); s.hands[0] = GrabHand{ false, 5, 9, "lowerarm_l", V3{} };
    CHECK(stale.Accept(s, 0));
    stale.DropTarget(5);
    s.seq = 78; s.hands[0] = Hold(5, 9, "lowerarm_l", V3{});
    CHECK(stale.Accept(s, 10));
    CHECK(stale.Pulls(10, mine, alive).size() == 1);

    // A hold already broken free, or mid-way to it, is replaced by the drop and comes back only as a new grab.
    GrabInbox brk;
    GrabStateMsg g = SomeState();
    CHECK(brk.Accept(g, 0));
    const std::vector<GrabPull> ps = brk.Pulls(0, mine, alive);
    CHECK(ps.size() == 1);
    if (ps.size() != 1) return;
    CHECK(!brk.NoteDistance(ps[0], 81, 0));
    brk.DropTarget(5);
    CHECK(brk.NoteDistance(ps[0], 0, 10));                       // still broken for that grab (NoteDistance keeps it so)
    g.seq = 78; g.hands[0].grabId = 12; CHECK(brk.Accept(g, 20));
    const std::vector<GrabPull> again = brk.Pulls(20, mine, alive);
    CHECK(again.size() == 1);
    if (again.size() == 1) CHECK(!brk.NoteDistance(again[0], 81, 20) && !brk.NoteDistance(again[0], 81, 319));   // a fresh timer
}

// ---------------------------------------------------------------- the pull law

static V3 Dv(V3 grip, V3 vel, V3 anchor, V3 anchorVel, double dt) { return GrabPullDv(grip, vel, anchor, anchorVel, dt); }

static void TestPullLaw() {
    const GrabPullParams d;
    CHECK(d.stiffness == 600 && d.damping == 75 && d.leadUu == 30 && d.maxDvUu == 300);       // 0.7.3: about 18 g at most
    CHECK(d.damping > 2 * std::sqrt(d.stiffness));                                             // overdamped
    const double dt = 1.0 / 60;

    // At rest on the hand, and moving with it: nothing to do.
    CHECK(Len(Dv(V3{ 10, 20, 30 }, V3{}, V3{ 10, 20, 30 }, V3{}, dt)) < 1e-9);
    CHECK(Len(Dv(V3{ 10, 20, 30 }, V3{ 150, -40, 9 }, V3{ 10, 20, 30 }, V3{ 150, -40, 9 }, dt)) < 1e-9);

    // Toward the hand, along the line only; the pinned value is the implicit spring-damper step from rest.
    const V3 toward = Dv(V3{}, V3{}, V3{ 15, 0, 0 }, V3{}, dt);
    CHECK(toward.x > 0 && toward.y == 0 && toward.z == 0);
    const double atLead = dt * 600 * 30 / (1 + dt * 75 + dt * dt * 600);
    CHECK(Near(Dv(V3{}, V3{}, V3{ 30, 0, 0 }, V3{}, dt).x, atLead, 1e-9));
    CHECK(Near(toward.x, atLead / 2, 1e-9));
    // Farther than the lead pulls no harder: the pull never exceeds stiffness x lead (the victim can struggle).
    CHECK(Near(Dv(V3{}, V3{}, V3{ 0, -300, 0 }, V3{}, dt).y, -atLead, 1e-9));
    CHECK(Near(Len(Dv(V3{}, V3{}, V3{ 3000, 4000, 0 }, V3{}, dt)), atLead, 1e-9));

    // Converges from inside the lead, never past the hand (overdamped: zeta = 75 / (2 sqrt 600) ~ 1.5).
    V3 x{}, v{};
    const V3 hand{ 30, -10, 5 };
    bool overshot = false;
    for (int i = 0; i < 60; ++i) {
        const V3 dv = Dv(x, v, hand, V3{}, dt);
        v = V3{ v.x + dv.x, v.y + dv.y, v.z + dv.z };
        x = V3{ x.x + v.x * dt, x.y + v.y * dt, x.z + v.z * dt };
        overshot = overshot || x.x > hand.x + 1e-9 || x.y < hand.y - 1e-9 || x.z > hand.z + 1e-9;
    }
    CHECK(!overshot);
    CHECK(Len(V3{ hand.x - x.x, hand.y - x.y, hand.z - x.z }) < 0.5);

    // From far away it closes at the bounded pull, then settles.
    x = V3{ -300, 0, 0 }; v = V3{};
    double prev = 1e9;
    bool monotone = true;
    for (int i = 0; i < 240; ++i) {
        const V3 dv = Dv(x, v, V3{}, V3{}, dt);
        CHECK(Len(dv) <= d.maxDvUu + 1e-9);
        v = V3{ v.x + dv.x, v.y + dv.y, v.z + dv.z };
        x = V3{ x.x + v.x * dt, x.y + v.y * dt, x.z + v.z * dt };
        monotone = monotone && std::fabs(x.x) <= prev + 1e-9;
        prev = std::fabs(x.x);
    }
    CHECK(monotone && std::fabs(x.x) < 0.5);

    // Follows a hand moving at 200 cm/s with no steady lag (the hand's velocity is fed forward).
    x = V3{}; v = V3{};
    V3 a{ 10, 0, 0 };
    for (int i = 0; i < 120; ++i) {
        const V3 av{ 200, 0, 0 };
        const V3 dv = Dv(x, v, a, av, dt);
        v = V3{ v.x + dv.x, v.y + dv.y, v.z + dv.z };
        x = V3{ x.x + v.x * dt, x.y + v.y * dt, x.z + v.z * dt };
        a = V3{ a.x + av.x * dt, a.y, a.z };
    }
    CHECK(std::fabs(a.x - x.x) < 0.5 && std::fabs(v.x - 200) < 1);

    // Capped per step whatever the inputs: an absurd hand speed, an absurd own speed.
    CHECK(Near(Len(Dv(V3{}, V3{}, V3{}, V3{ 1e6, 0, 0 }, dt)), d.maxDvUu, 1e-6));
    CHECK(Near(Len(Dv(V3{}, V3{ 0, 0, -1e5 }, V3{}, V3{}, dt)), d.maxDvUu, 1e-6));

    // The cap is per 60 Hz step: a slower grabber's longer step gets a longer cap, so a cap-bound pull is the same cm/s per
    // second at 20 and 60 fps (0.8.0-pre19: a 15-26 fps grabber pulled a held body a third as hard). Never below one step.
    {
        const V3 hard{ 1e6, 0, 0 };
        const double at60 = Len(Dv(V3{}, V3{}, V3{}, hard, 1.0 / 60)) * 60, at20 = Len(Dv(V3{}, V3{}, V3{}, hard, 1.0 / 20)) * 20;
        const double at30 = Len(Dv(V3{}, V3{}, V3{}, hard, 1.0 / 30)) * 30;
        CHECK(Near(at60, d.maxDvUu * 60, 1e-6) && Near(at30, at60, 1e-6) && Near(at20, at60, 1e-6));
        CHECK(Near(Len(Dv(V3{}, V3{}, V3{}, hard, 1.0 / 20)), d.maxDvUu * 3, 1e-6));
        CHECK(Near(Len(Dv(V3{}, V3{}, V3{}, hard, 1.0 / 120)), d.maxDvUu, 1e-6));       // a fast machine: still one step's cap
        CHECK(Near(Len(Dv(V3{}, V3{}, V3{}, hard, 1.0)), d.maxDvUu * 4, 1e-6));           // the frame clamp (80 ms) bounds it: 4 steps
        CHECK(Near(Len(GrabPullDv(V3{}, V3{}, V3{}, hard, 1.0 / 20, hsm::sync::DownPullParams())), 1500 * 3, 1e-6));
    }

    // Stable at the longest frame (80 ms; longer frames are clamped to it), and a zero or negative frame is 4 ms.
    x = V3{ 35, 0, 0 }; v = V3{};
    bool grew = false;
    double last = 35;
    for (int i = 0; i < 60; ++i) {
        const V3 dv = Dv(x, v, V3{}, V3{}, 0.5);
        v = V3{ v.x + dv.x, v.y + dv.y, v.z + dv.z };
        x = V3{ x.x + v.x * 0.08, x.y, x.z };
        grew = grew || std::fabs(x.x) > last + 1e-9;
        last = std::fabs(x.x);
    }
    CHECK(!grew && std::fabs(x.x) < 0.5);
    CHECK(Near(Dv(V3{}, V3{}, V3{ 20, 0, 0 }, V3{}, 0.5).x, Dv(V3{}, V3{}, V3{ 20, 0, 0 }, V3{}, 0.08).x));
    const V3 tiny = Dv(V3{}, V3{}, V3{ 20, 0, 0 }, V3{}, 0);
    CHECK(Near(tiny.x, Dv(V3{}, V3{}, V3{ 20, 0, 0 }, V3{}, 0.004).x) && tiny.x > 0);
    CHECK(Near(Dv(V3{}, V3{}, V3{ 20, 0, 0 }, V3{}, -1).x, tiny.x));

    // Garbage in, nothing out.
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    CHECK(Len(Dv(V3{ nan, 0, 0 }, V3{}, V3{ 20, 0, 0 }, V3{}, dt)) == 0);
    CHECK(Len(Dv(V3{}, V3{}, V3{ inf, 0, 0 }, V3{}, dt)) == 0);
    CHECK(Len(Dv(V3{}, V3{ 0, nan, 0 }, V3{ 20, 0, 0 }, V3{}, dt)) == 0);
    CHECK(Len(Dv(V3{}, V3{}, V3{ 20, 0, 0 }, V3{ 0, 0, -inf }, dt)) == 0);
    CHECK(Len(Dv(V3{}, V3{}, V3{ 20, 0, 0 }, V3{}, nan)) == 0);
}

// Test 2026-10-25: a grabbed player's body followed on a soft spring and did not feel like the game's own carry. A body
// that cannot struggle is pulled stiffer (still without overshoot) and breaks free only on a glitch; the pull aims ahead
// of the grabber's late stand-in hand; the grabber's screen leaves the held limb to its native grab.
static void TestDownCarry() {
    using hsm::sync::DownPullParams;
    const GrabPullParams soft, down = DownPullParams();
    CHECK(down.stiffness == 4500 && down.damping == 200 && down.leadUu == 80 && down.maxDvUu == 1500);   // carrying: unchanged
    CHECK(down.stiffness > soft.stiffness && down.leadUu > soft.leadUu && down.maxDvUu > soft.maxDvUu);
    CHECK(down.damping > 2 * std::sqrt(down.stiffness));                             // overdamped
    const double dt = 1.0 / 60;
    const V3 far{ 60, 0, 0 };
    CHECK(GrabPullDv(V3{}, V3{}, far, V3{}, dt, down).x > GrabPullDv(V3{}, V3{}, far, V3{}, dt, soft).x);   // pulls harder
    V3 x{ -200, 0, 0 }, v{};
    bool overshot = false;
    for (int i = 0; i < 240; ++i) {
        const V3 dv = GrabPullDv(x, v, V3{}, V3{}, dt, down);
        CHECK(Len(dv) <= down.maxDvUu + 1e-9);
        v = V3{ v.x + dv.x, v.y + dv.y, v.z + dv.z };
        x = V3{ x.x + v.x * dt, x.y + v.y * dt, x.z + v.z * dt };
        overshot = overshot || x.x > 1e-9;
    }
    CHECK(!overshot && Len(x) < 0.5);

    // Lift cap (0.8.0-pre7, #83/#84): a carried body is never driven up faster than kGrabDownMaxUpUu, whatever the hand
    // does above it; sideways and downward pulls are untouched, and the soft hold has no cap.
    CHECK(down.maxUpUu == hsm::sync::kGrabDownMaxUpUu && soft.maxUpUu < 0);
    const V3 above{ 0, 0, 80 }, up{ 0, 0, 900 };
    CHECK(GrabPullDv(V3{}, V3{}, above, up, dt, down).z <= down.maxUpUu + 1e-9);
    CHECK(GrabPullDv(V3{}, V3{ 0, 0, 500 }, above, up, dt, down).z <= down.maxUpUu - 500 + 1e-9);   // already rising: slowed to the cap
    CHECK(GrabPullDv(V3{}, V3{}, above, up, dt, soft).z > soft.maxDvUu - 1e-9);                     // uncapped: the step cap alone
    CHECK(GrabPullDv(V3{}, V3{}, V3{ 0, 0, -80 }, V3{ 0, 0, -900 }, dt, down).z < -down.maxUpUu);  // downward: no cap
    CHECK(GrabPullDv(V3{}, V3{}, V3{ 80, 0, 0 }, V3{ 900, 0, 0 }, dt, down).x > down.maxUpUu);      // sideways: no cap
    {
        GrabPullParams capped = down;
        capped.maxUpUu = 0.0;
        CHECK(GrabPullDv(V3{}, V3{}, above, up, dt, capped).z <= 1e-9);                              // 0: never lifted
    }

    // Break-free: the soft hold breaks past 80 uu for 300 ms; a down body's only past 250 uu for 1 s.
    const auto mine = [](uint8_t e) { return e == 5; };
    const auto alive = [](uint8_t) { return false; };
    GrabInbox in;
    CHECK(in.Accept(SomeState(), 0));
    const std::vector<GrabPull> ps = in.Pulls(0, mine, alive);
    CHECK(ps.size() == 1);
    if (ps.size() != 1) return;
    using hsm::sync::kGrabDownBreakMs;
    using hsm::sync::kGrabDownBreakUu;
    CHECK(!in.NoteDistance(ps[0], 200, 0, kGrabDownBreakUu, kGrabDownBreakMs));
    CHECK(!in.NoteDistance(ps[0], 200, 900, kGrabDownBreakUu, kGrabDownBreakMs));       // under 250: never far
    CHECK(!in.NoteDistance(ps[0], 260, 1000, kGrabDownBreakUu, kGrabDownBreakMs));
    CHECK(!in.NoteDistance(ps[0], 260, 1999, kGrabDownBreakUu, kGrabDownBreakMs));
    CHECK(in.NoteDistance(ps[0], 260, 2000, kGrabDownBreakUu, kGrabDownBreakMs));

    // The anchor: ahead along the hand's velocity by 80 ms, at most 40 uu; the hand itself on bad input.
    using hsm::sync::GrabAnchor;
    const V3 a = GrabAnchor(V3{ 10, 0, 0 }, V3{ 100, 0, 0 });
    CHECK(Near(a.x, 18) && a.y == 0 && a.z == 0);
    CHECK(Near(GrabAnchor(V3{}, V3{ 0, 5000, 0 }).y, 40));
    CHECK(GrabAnchor(V3{ 1, 2, 3 }, V3{ std::numeric_limits<double>::quiet_NaN(), 0, 0 }).x == 1);

    // The bones the grabber's screen leaves to its grab.
    using hsm::sync::PoseBoneIndex;
    using hsm::sync::YieldBones;
    CHECK(PoseBoneIndex("hand_r") == sync::kBoneHandR && PoseBoneIndex("pelvis") == 0 && PoseBoneIndex("weapon") == -1);
    CHECK(YieldBones(-1, true) == 0 && YieldBones(sync::kPoseBones, false) == 0);
    CHECK(YieldBones(sync::kBoneHandR, false) == ((1u << 16) | (1u << 15)));          // hand and lower arm
    CHECK(YieldBones(sync::kBoneHandR, true) == ((1u << 16) | (1u << 15) | (1u << 14) | (1u << 13)));   // the arm to the chest
    CHECK(YieldBones(sync::kBoneFootL, true) == ((1u << 19) | (1u << 18) | (1u << 17)));   // the leg to the pelvis
    CHECK(YieldBones(0, true) == 1u && YieldBones(3, true) == (1u << 3));               // a torso bone: only itself
    for (int b = 0; b < sync::kPoseBones; ++b) {
        CHECK((YieldBones(b, true) & (1u << b)) != 0);
        CHECK(YieldBones(b, true) != (1u << sync::kPoseBones) - 1u);                    // never the whole (gravity-free) body
    }

    // The game's bone names in any case (a grab's "Head").
    CHECK(PoseBoneIndex("Head") == sync::kBoneHead && PoseBoneIndex("HAND_R") == sync::kBoneHandR && PoseBoneIndex("Pelvis") == 0);
    CHECK(PoseBoneIndex("hea") == -1 && PoseBoneIndex("heads") == -1 && PoseBoneIndex("") == -1);
    CHECK(YieldBones(PoseBoneIndex("Head"), true) == ((1u << 8) | (1u << 7) | (1u << 6)));   // head and neck

    // grab_drag: every hold yields like a carried foot.
    using hsm::sync::DragYieldBones;
    CHECK(DragYieldBones(-1) == 0 && DragYieldBones(sync::kPoseBones) == 0);
    CHECK(DragYieldBones(sync::kBoneFootL) == YieldBones(sync::kBoneFootL, true));      // the leg, standing or down
    CHECK(DragYieldBones(sync::kBoneHandR) == ((1u << 16) | (1u << 15) | (1u << 14) | (1u << 13)));
    CHECK(DragYieldBones(sync::kBoneHead) == ((1u << 8) | (1u << 7) | (1u << 6)));
    CHECK(DragYieldBones(3) == ((1u << 2) | (1u << 3) | (1u << 4)));                    // spine_03 and its neighbours
    CHECK(DragYieldBones(5) == ((1u << 4) | (1u << 5)));
    CHECK(DragYieldBones(2) == ((1u << 2) | (1u << 3)));
    CHECK(DragYieldBones(0) == (1u | (1u << 2)));
    const uint32_t legsAndArms = (1u << 19) | (1u << 22) | (1u << 12) | (1u << 16);
    for (int b = 0; b < sync::kPoseBones; ++b) {
        CHECK((DragYieldBones(b) & (1u << b)) != 0);
        CHECK((DragYieldBones(b) & legsAndArms) != legsAndArms);                         // never the whole body
        if (b != 0 && b != 2) CHECK((DragYieldBones(b) & 1u) == 0);                       // the pelvis stays servoed
    }

    // The pull's share on the pelvis and chest.
    using hsm::sync::GrabSpread;
    CHECK(GrabSpread(true, false) == sync::kGrabDownSpread && GrabSpread(true, true) == sync::kGrabDownSpread);
    CHECK(GrabSpread(false, false) == 0.0 && GrabSpread(false, true) > 0.0 && GrabSpread(false, true) < sync::kGrabDownSpread);

    // A held stand-in is not snapped unless far off; not held: the servo's own rule.
    using hsm::sync::GrabBlocksSnap;
    CHECK(GrabBlocksSnap(1u << 3, 160.0) && GrabBlocksSnap(1u << 3, -1.0));
    CHECK(!GrabBlocksSnap(1u << 3, 400.0) && !GrabBlocksSnap(0, 160.0));
}

int main() {
    TestRoundTrip();
    TestRejects();
    TestBoneNames();
    TestSender();
    TestInbox();
    TestBreakFree();
    TestForgetRetiresHolds();
    TestDropTarget();
    TestPullLaw();
    TestDownCarry();
    // A Grappler carries a fighter on its feet the way a body is carried, and nobody else does.
    if (!(sync::GrabCarried(false, true) && sync::GrabCarried(true, false) && !sync::GrabCarried(false, false))) { std::printf("GrabCarried\n"); ++g_fail; }
    if (g_fail) { std::printf("test_hsm_grab: %d check(s) failed\n", g_fail); return 1; }
    std::printf("test_hsm_grab: all checks passed\n");
    return 0;
}
