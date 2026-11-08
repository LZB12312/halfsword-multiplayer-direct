// The host's census of level physics props (session/hsm_prop_census.h): the PropCensus wire format (round trip, the
// byte layout, every reject: part bounds, entry count, an index twice, class 0, a place out of range or not finite,
// truncation, trailing bytes, another type or revision), SplitCensus (part sizes, the empty census, the cap), the
// client's CensusAssembler (in and out of order, duplicate and contradicting parts, an index in two parts, newer and
// stale censuses, wrap), MatchCensus (nearest, same class, the reach boundary, no candidate twice, nearest pairs
// first, ties), CensusMatchPlace and the measured Alley case (the early place), and the host's CensusBook (stable indices,
// homes kept, gone props, never reused, left-out keys, cap).
#include "session/hsm_prop_census.h"
#include <cstdio>
#include <limits>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static CensusEntry E(uint16_t index, uint32_t cls, float x, float y = 0, float z = 0) {
    CensusEntry e; e.index = index; e.classHash = cls; e.px = x; e.py = y; e.pz = z; return e;
}
static bool Same(const CensusEntry& a, const CensusEntry& b) {
    return a.index == b.index && a.classHash == b.classHash && a.px == b.px && a.py == b.py && a.pz == b.pz;
}
static bool SameList(const std::vector<CensusEntry>& a, const std::vector<CensusEntry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!Same(a[i], b[i])) return false;
    return true;
}
static PropCensusMsg Part(uint16_t seq, uint8_t part, uint8_t parts, std::vector<CensusEntry> entries) {
    PropCensusMsg m; m.seq = seq; m.part = part; m.parts = parts; m.entries = std::move(entries); return m;
}
static bool DecodeOk(const PropCensusMsg& m) { const auto b = Encode(m); PropCensusMsg o; return Decode(b.data(), b.size(), o); }

// ---------------------------------------------------------------- sequence numbers
static void TestCensusSeqNewer() {
    CHECK(CensusSeqNewer(1, 0) && CensusSeqNewer(0, 65535) && CensusSeqNewer(32767, 0));
    CHECK(!CensusSeqNewer(0, 0) && !CensusSeqNewer(0, 1) && !CensusSeqNewer(65535, 0) && !CensusSeqNewer(32768, 0));
}

// ---------------------------------------------------------------- wire format
static void TestCensusRoundTrip() {
    const PropCensusMsg m = Part(513, 2, 5, {E(0, 0xA1B2C3D4u, 10.5f, -20.25f, 5.f), E(7, 1, -kMaxPropCoord, 0, kMaxPropCoord),
                                             E(65535, 0xFFFFFFFFu, 1.f, 2.f, 3.f)});
    const auto bytes = Encode(m);
    CHECK(bytes.size() == 4 + 5 + 3 * 18);                         // header, seq part parts count, 18 B per entry
    CHECK(bytes.size() >= 9 && bytes[3] == uint8_t(MsgType::PropCensus));
    if (bytes.size() >= 9) CHECK(bytes[4] == 0x01 && bytes[5] == 0x02 && bytes[6] == 2 && bytes[7] == 5 && bytes[8] == 3);
    PropCensusMsg o;
    CHECK(Decode(bytes.data(), bytes.size(), o));
    CHECK(o.seq == 513 && o.part == 2 && o.parts == 5 && SameList(o.entries, m.entries));

    const PropCensusMsg empty = Part(9, 0, 1, {});
    const auto eb = Encode(empty);
    PropCensusMsg eo; eo.entries.push_back(E(1, 1, 0));            // a decode replaces whatever was there
    CHECK(eb.size() == 9 && Decode(eb.data(), eb.size(), eo) && eo.seq == 9 && eo.parts == 1 && eo.entries.empty());

    std::vector<CensusEntry> full;
    for (uint16_t i = 0; i < kCensusEntriesPerPart; ++i) full.push_back(E(uint16_t(i * 3), 77, float(i), -float(i), 0.5f));
    const auto fb = Encode(Part(0, 31, kMaxCensusParts, full));
    PropCensusMsg fo;
    CHECK(fb.size() == 9 + 18 * kCensusEntriesPerPart && fb.size() <= kMaxPacket);
    CHECK(Decode(fb.data(), fb.size(), fo) && fo.part == 31 && fo.parts == kMaxCensusParts && SameList(fo.entries, full));
}

static void TestCensusRejects() {
    const PropCensusMsg good = Part(4, 1, 3, {E(1, 5, 0), E(2, 5, 1)});
    CHECK(DecodeOk(good));
    PropCensusMsg m = good; m.part = 3; CHECK(!DecodeOk(m));                   // part beyond the count
    m = good; m.part = 0; m.parts = 0; CHECK(!DecodeOk(m));                    // no parts at all
    m = good; m.part = 0; m.parts = uint8_t(kMaxCensusParts + 1); CHECK(!DecodeOk(m));
    m = good; m.part = uint8_t(kMaxCensusParts - 1); m.parts = kMaxCensusParts; CHECK(DecodeOk(m));
    m = good; m.part = 2; CHECK(DecodeOk(m));                                  // the last part
    m = good; m.entries.clear();
    for (uint16_t i = 0; i <= kCensusEntriesPerPart; ++i) m.entries.push_back(E(i, 5, 0));
    CHECK(!DecodeOk(m));                                                       // 65 entries in one part
    m.entries.pop_back(); CHECK(DecodeOk(m));
    m = good; m.entries[1].index = 1; CHECK(!DecodeOk(m));                     // an index twice
    m = good; m.entries[0].classHash = 0; CHECK(!DecodeOk(m));                 // no class
    for (int axis = 0; axis < 3; ++axis) {
        for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          kMaxPropCoord * 1.01f, -kMaxPropCoord * 1.01f}) {
            m = good; float* p = axis == 0 ? &m.entries[1].px : axis == 1 ? &m.entries[1].py : &m.entries[1].pz;
            *p = bad; CHECK(!DecodeOk(m));
        }
    }
    const auto b0 = Encode(good);
    PropCensusMsg o;
    auto b = b0; b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));       // trailing byte
    for (size_t n = 0; n < b0.size(); ++n) CHECK(!Decode(b0.data(), n, o));     // every truncation
    b = b0; if (b.size() > 8) { b[8] = 3; CHECK(!Decode(b.data(), b.size(), o)); }   // count says more than there is
    b = b0; if (b.size() > 8) { b[8] = 1; CHECK(!Decode(b.data(), b.size(), o)); }   // ... or less
    b = b0; if (b.size() > 3) { b[3] = uint8_t(MsgType::PropLease); CHECK(!Decode(b.data(), b.size(), o)); }
    b = b0; if (b.size() > 2) { b[2] = uint8_t(kProtocolRevision - 1); CHECK(!Decode(b.data(), b.size(), o)); }
}

// ---------------------------------------------------------------- the host splits its census
static void TestSplitCensus() {
    auto list = [](size_t n) { std::vector<CensusEntry> v; for (size_t i = 0; i < n; ++i) v.push_back(E(uint16_t(i), 9, float(i))); return v; };
    auto check = [&](size_t n, size_t wantParts) {
        const auto all = list(n);
        const auto parts = SplitCensus(77, all);
        CHECK(parts.size() == wantParts);
        std::vector<CensusEntry> back;
        for (size_t p = 0; p < parts.size(); ++p) {
            CHECK(parts[p].seq == 77 && parts[p].part == p && parts[p].parts == wantParts);
            CHECK(parts[p].entries.size() <= kCensusEntriesPerPart && DecodeOk(parts[p]));
            back.insert(back.end(), parts[p].entries.begin(), parts[p].entries.end());
        }
        CHECK(SameList(back, all));                                            // every entry once, in order
    };
    check(0, 1);                                                               // the empty census is one empty part
    check(1, 1);
    check(kCensusEntriesPerPart, 1);
    check(kCensusEntriesPerPart + 1, 2);
    check(kMaxCensusProps, kMaxCensusParts);
    const auto two = SplitCensus(1, list(kCensusEntriesPerPart + 1));
    if (two.size() == 2) CHECK(two[0].entries.size() == kCensusEntriesPerPart && two[1].entries.size() == 1);
    CHECK(SplitCensus(1, list(kMaxCensusProps + 1)).empty());                  // too many: nothing
}

// ---------------------------------------------------------------- a client assembles it
static void TestAssemblerInOrder() {
    using R = CensusAssembler::Result;
    CensusAssembler a;
    CHECK(!a.HaveCensus() && a.Census().empty());
    CHECK(a.Add(Part(5, 0, 3, {E(4, 1, 0), E(0, 1, 1)})) == R::Pending);
    CHECK(a.Add(Part(5, 1, 3, {E(2, 1, 2)})) == R::Pending && !a.HaveCensus());
    CHECK(a.Add(Part(5, 2, 3, {E(1, 2, 3), E(3, 2, 4)})) == R::Complete);
    CHECK(a.HaveCensus() && a.Seq() == 5);
    CHECK(SameList(a.Census(), {E(0, 1, 1), E(1, 2, 3), E(2, 1, 2), E(3, 2, 4), E(4, 1, 0)}));   // sorted by index
    CHECK(a.Add(Part(5, 0, 3, {E(4, 1, 0)})) == R::Stale);                    // the census already complete
    CHECK(a.Add(Part(4, 0, 1, {})) == R::Stale);                               // an older one
    CHECK(a.HaveCensus() && a.Seq() == 5 && a.Census().size() == 5);

    CHECK(a.Add(Part(6, 0, 1, {})) == R::Complete);                            // the empty census replaces it
    CHECK(a.HaveCensus() && a.Seq() == 6 && a.Census().empty());
}

static void TestAssemblerOutOfOrder() {
    using R = CensusAssembler::Result;
    CensusAssembler a;
    CHECK(a.Add(Part(9, 2, 3, {E(20, 1, 0)})) == R::Pending);
    CHECK(a.Add(Part(9, 0, 3, {E(0, 1, 0)})) == R::Pending);
    CHECK(a.Add(Part(9, 0, 3, {E(0, 1, 0)})) == R::Rejected);                 // a part twice
    CHECK(a.Add(Part(9, 1, 4, {E(10, 1, 0)})) == R::Rejected);                // another part count
    CHECK(!a.HaveCensus());
    CHECK(a.Add(Part(9, 1, 3, {E(10, 1, 0)})) == R::Complete);                // the census being assembled stayed
    CHECK(a.Seq() == 9 && SameList(a.Census(), {E(0, 1, 0), E(10, 1, 0), E(20, 1, 0)}));
}

static void TestAssemblerNewerAndStale() {
    using R = CensusAssembler::Result;
    CensusAssembler a;
    CHECK(a.Add(Part(3, 0, 1, {E(1, 1, 0)})) == R::Complete);
    CHECK(a.Add(Part(4, 0, 2, {E(1, 1, 0)})) == R::Pending);
    CHECK(a.Add(Part(5, 0, 2, {E(2, 1, 0)})) == R::Pending);                  // newer: census 4 is dropped
    CHECK(a.Add(Part(4, 1, 2, {E(3, 1, 0)})) == R::Stale);                    // older than the one being assembled
    CHECK(a.Seq() == 3 && a.Census().size() == 1);                             // still census 3
    CHECK(a.Add(Part(5, 1, 2, {E(4, 1, 0)})) == R::Complete);
    CHECK(a.Seq() == 5 && SameList(a.Census(), {E(2, 1, 0), E(4, 1, 0)}));

    // An index in two parts: the census is dropped, the last good one stays.
    CHECK(a.Add(Part(6, 0, 2, {E(7, 1, 0)})) == R::Pending);
    CHECK(a.Add(Part(6, 1, 2, {E(7, 2, 5)})) == R::Rejected);
    CHECK(a.Seq() == 5 && a.Census().size() == 2);
    CHECK(a.Add(Part(6, 0, 2, {E(7, 1, 0)})) == R::Pending);                  // census 6 starts over
    CHECK(a.Add(Part(6, 1, 2, {E(8, 1, 0)})) == R::Complete && a.Seq() == 6);

    // Wrap: 0 is newer than 65535.
    CensusAssembler w;
    CHECK(w.Add(Part(65535, 0, 1, {})) == R::Complete);
    CHECK(w.Add(Part(0, 0, 1, {E(1, 1, 0)})) == R::Complete && w.Seq() == 0 && w.Census().size() == 1);
    CHECK(w.Add(Part(65535, 0, 1, {})) == R::Stale);

    // A part no decoder would pass is refused here too.
    CensusAssembler h;
    CHECK(h.Add(Part(1, 1, 1, {})) == R::Rejected);                            // part beyond the count
    CHECK(h.Add(Part(1, 0, 0, {})) == R::Rejected);
    CHECK(h.Add(Part(1, 0, uint8_t(kMaxCensusParts + 1), {})) == R::Rejected);
    CHECK(!h.HaveCensus());
    CHECK(h.Add(Part(1, 0, 1, {})) == R::Complete);

    // Clear: a level change; any census is accepted again.
    a.Clear();
    CHECK(!a.HaveCensus() && a.Census().empty());
    CHECK(a.Add(Part(2, 0, 1, {E(1, 1, 0)})) == R::Complete && a.Seq() == 2);
}

// ---------------------------------------------------------------- a client matches it to its own props
static CensusCandidate C(uint32_t cls, float x, float y = 0, float z = 0) { CensusCandidate c; c.classHash = cls; c.px = x; c.py = y; c.pz = z; return c; }

static void TestMatchCensus() {
    // Nearest of the same class.
    auto r = MatchCensus({E(0, 1, 0)}, {C(1, 50), C(1, 10), C(2, 1)});
    CHECK(r.size() == 1 && r[0] == 1);
    // Another class never matches, however close.
    r = MatchCensus({E(0, 1, 0)}, {C(2, 0)});
    CHECK(r.size() == 1 && r[0] == -1);
    // The reach: exactly kCensusMatchUu matches, beyond it misses (all three axes count).
    r = MatchCensus({E(0, 1, 0), E(1, 1, 1000)}, {C(1, 100), C(1, 1000, 60, 80.5f)});
    CHECK(r.size() == 2 && r[0] == 0 && r[1] == -1);
    r = MatchCensus({E(0, 1, 0, 0, 0)}, {C(1, 0, 60, 80)});
    CHECK(r.size() == 1 && r[0] == 0);
    r = MatchCensus({E(0, 1, 0)}, {C(1, 100.01f)});
    CHECK(r.size() == 1 && r[0] == -1);
    // A candidate is used once: two entries on one candidate, the nearer gets it.
    r = MatchCensus({E(0, 1, 0), E(1, 1, 30)}, {C(1, 20)});
    CHECK(r.size() == 2 && r[0] == -1 && r[1] == 0);
    // Nearest pairs first, over the whole census: entry 0's nearest is the candidate entry 1 sits on, so entry 0 takes
    // the other one (an entry-by-entry greedy would leave entry 1 with nothing).
    r = MatchCensus({E(0, 1, 0), E(1, 1, 50)}, {C(1, 45), C(1, -60)});
    CHECK(r.size() == 2 && r[0] == 1 && r[1] == 0);
    // Ties: the lower entry, then the lower candidate.
    r = MatchCensus({E(0, 1, 0)}, {C(1, 50), C(1, -50)});
    CHECK(r.size() == 1 && r[0] == 0);
    r = MatchCensus({E(5, 1, -50), E(6, 1, 50)}, {C(1, 0)});
    CHECK(r.size() == 2 && r[0] == 0 && r[1] == -1);
    // A wider reach is the caller's.
    r = MatchCensus({E(0, 1, 0)}, {C(1, 150)}, 200.f);
    CHECK(r.size() == 1 && r[0] == 0);
    // Nothing local: every entry misses; nothing in the census: no result.
    r = MatchCensus({E(0, 1, 0), E(1, 2, 0)}, {});
    CHECK(r.size() == 2 && r[0] == -1 && r[1] == -1);
    CHECK(MatchCensus({}, {C(1, 0)}).empty());
    // A bigger census: every entry finds its own candidate among shuffled, slightly moved ones.
    std::vector<CensusEntry> census; std::vector<CensusCandidate> locals;
    for (uint16_t i = 0; i < 200; ++i) census.push_back(E(i, 1 + i % 3, float(i % 20) * 150.f, float(i / 20) * 150.f, 0));
    for (int i = 199; i >= 0; --i) locals.push_back(C(1 + uint32_t(i) % 3, float(i % 20) * 150.f + 7.f, float(i / 20) * 150.f - 5.f, 3.f));
    r = MatchCensus(census, locals);
    bool all = r.size() == 200;
    for (size_t i = 0; all && i < r.size(); ++i) all = r[i] == int(199 - i);
    CHECK(all);
}

// The early place: a candidate with a snapshot of its own class is compared where it lay early, else where it is now.
static void TestCensusMatchPlace() {
    const CensusCandidate now = C(1, -438, -14, 194);
    CensusCandidate got = CensusMatchPlace(now, nullptr);
    CHECK(got.classHash == 1 && got.px == -438 && got.py == -14 && got.pz == 194);       // no snapshot: where it is now
    const CensusCandidate early = C(1, -344, 214, 224);
    got = CensusMatchPlace(now, &early);
    CHECK(got.classHash == 1 && got.px == -344 && got.py == 214 && got.pz == 224);       // all three axes: the snapshot
    const CensusCandidate other = C(2, -344, 214, 224);
    got = CensusMatchPlace(now, &other);
    CHECK(got.classHash == 1 && got.px == -438 && got.py == -14 && got.pz == 194);       // another class: not its place
}

// The measured case (Alley, 2026-11-05, machine c): two physics props of one class whose homes are 57 cm apart, with the
// census tolerance at 100 cm. Early in the level the copies lay 1 cm and 10 cm from their homes. At the first match one
// had rolled 250 cm away and the other had crossed to 32 cm from the first home. Matched on where they are then, home A
// takes the crossing copy and home B is left open; matched on the early places, each home takes its own copy.
static void TestCensusSnapshotMatch() {
    const std::vector<CensusEntry> census = {E(0, 7, -344, 215, 223), E(1, 7, -287, 210, 219)};   // host: _200, _202
    const CensusCandidate nowA = C(7, -438, -14, 194), nowB = C(7, -320, 199, 209);              // at the first match
    const CensusCandidate earlyA = C(7, -344, 214, 224), earlyB = C(7, -280, 205, 225);          // early in the level
    std::vector<int> r = MatchCensus(census, {CensusMatchPlace(nowA, nullptr), CensusMatchPlace(nowB, nullptr)});
    CHECK(r.size() == 2 && r[0] == 1 && r[1] == -1);          // home A takes copy B (crossed, 32 cm off it); home B is open
    r = MatchCensus(census, {CensusMatchPlace(nowA, &earlyA), CensusMatchPlace(nowB, &earlyB)});
    CHECK(r.size() == 2 && r[0] == 0 && r[1] == 1);           // each home takes its own copy
}

// ---------------------------------------------------------------- the host names its props
using Book = CensusBook<int>;
static Book::Seen S(int key, uint32_t cls, float x, float y = 0, float z = 0) { Book::Seen s; s.key = key; s.classHash = cls; s.px = x; s.py = y; s.pz = z; return s; }

static void TestCensusBook() {
    Book b;
    CHECK(b.Size() == 0 && b.Entries().empty());
    CHECK(b.Update({S(100, 1, 0), S(200, 2, 10), S(300, 1, 20)}));            // walk order gives the indices
    uint16_t i = 99;
    CHECK(b.Find(100, i) && i == 0); CHECK(b.Find(200, i) && i == 1); CHECK(b.Find(300, i) && i == 2);
    CHECK(!b.Find(400, i));
    CHECK(SameList(b.Entries(), {E(0, 1, 0), E(1, 2, 10), E(2, 1, 20)}));
    CHECK(b.Size() == 3 && b.LeftOut() == 0);

    CHECK(!b.Update({S(300, 1, 20), S(100, 1, 0), S(200, 2, 10)}));           // same props, another order: no change
    CHECK(b.Find(300, i) && i == 2);
    CHECK(!b.Update({S(100, 1, 555, 1, 2), S(200, 2, 10), S(300, 1, 20)}));   // a prop moved: home kept, no change
    CHECK(SameList(b.Entries(), {E(0, 1, 0), E(1, 2, 10), E(2, 1, 20)}));

    CHECK(b.Update({S(100, 1, 0), S(300, 1, 20)}));                           // one gone
    CHECK(!b.Find(200, i) && b.Find(300, i) && i == 2 && b.Size() == 2);
    CHECK(b.Update({S(100, 1, 0), S(300, 1, 20), S(200, 2, 10), S(500, 3, 30)}));   // back, and a new one: new indices
    CHECK(b.Find(200, i) && i == 3); CHECK(b.Find(500, i) && i == 4);
    CHECK(SameList(b.Entries(), {E(0, 1, 0), E(2, 1, 20), E(3, 2, 10), E(4, 3, 30)}));

    CHECK(!b.Update({S(100, 1, 0), S(100, 1, 0), S(300, 1, 20), S(200, 2, 10), S(500, 3, 30)}));   // a key twice: once
    CHECK(b.Size() == 4 && b.LeftOut() == 0);

    // Left out: class 0, a place no census can carry; counted, not named.
    CHECK(!b.Update({S(100, 1, 0), S(300, 1, 20), S(200, 2, 10), S(500, 3, 30), S(600, 0, 1),
                     S(700, 1, std::numeric_limits<float>::quiet_NaN()), S(800, 1, 0, kMaxPropCoord * 2)}));
    CHECK(b.Size() == 4 && b.LeftOut() == 3 && !b.Find(600, i) && !b.Find(700, i) && !b.Find(800, i));
    CHECK(b.Update({S(900, 1, 0)}) && b.LeftOut() == 0 && b.Find(900, i) && i == 5);
    Book once;                                                                 // a key twice counts once, left out too
    CHECK(!once.Update({S(1, 0, 0), S(1, 0, 0)}) && once.LeftOut() == 1 && once.Size() == 0);

    // A level change starts over.
    b.Clear();
    CHECK(b.Size() == 0 && !b.Find(900, i));
    CHECK(b.Update({S(900, 1, 0)}) && b.Find(900, i) && i == 0);
}

static void TestCensusBookCaps() {
    Book b;
    std::vector<Book::Seen> walk;
    for (int k = 0; k < int(kMaxCensusProps) + 5; ++k) walk.push_back(S(k, 1, float(k)));
    CHECK(b.Update(walk));
    CHECK(b.Size() == kMaxCensusProps && b.LeftOut() == 5);
    uint16_t i = 0;
    CHECK(b.Find(int(kMaxCensusProps) - 1, i) && i == kMaxCensusProps - 1 && !b.Find(int(kMaxCensusProps), i));
    CHECK(b.Entries().size() == kMaxCensusProps && SplitCensus(1, b.Entries()).size() == kMaxCensusParts);

    // Indices are never reused within a level: after 65536 props came and went, a new one is left out.
    Book c;
    for (int k = 0; k < 65536; ++k) c.Update({S(k, 1, 0)});
    CHECK(c.Find(65535, i) && i == 65535 && c.LeftOut() == 0);
    CHECK(c.Update({S(70000, 1, 0)}));                                         // 65535 gone; nothing new named
    CHECK(c.Size() == 0 && c.LeftOut() == 1 && !c.Find(70000, i));
}

static void TestCensusPlaceOk() {
    CHECK(CensusPlaceOk(0, 0, 0) && CensusPlaceOk(kMaxPropCoord, -kMaxPropCoord, 1.f));
    CHECK(!CensusPlaceOk(kMaxPropCoord * 1.01f, 0, 0) && !CensusPlaceOk(0, -kMaxPropCoord * 1.01f, 0));
    CHECK(!CensusPlaceOk(0, 0, std::numeric_limits<float>::infinity()) && !CensusPlaceOk(std::numeric_limits<float>::quiet_NaN(), 0, 0));
}

int main() {
    TestCensusSeqNewer();
    TestCensusRoundTrip();
    TestCensusRejects();
    TestSplitCensus();
    TestAssemblerInOrder();
    TestAssemblerOutOfOrder();
    TestAssemblerNewerAndStale();
    TestMatchCensus();
    TestCensusMatchPlace();
    TestCensusSnapshotMatch();
    TestCensusBook();
    TestCensusBookCaps();
    TestCensusPlaceOk();
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("all prop census tests passed\n");
    return 0;
}
