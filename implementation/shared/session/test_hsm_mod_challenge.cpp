// Unit tests for hsm_mod_challenge.h. Run via implementation/shared/run_tests.sh test_hsm_mod_challenge.
// A synthetic x64 image (headers, .text with absolute pointers, .reloc) is "loaded" at two bases: the answers must agree,
// and any changed code byte must change them.
#include "session/hsm_mod_challenge.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

namespace {
constexpr uint64_t kPreferred = 0x180000000ull;
constexpr uint32_t kTextRva = 0x1000, kTextSize = 0x3000, kRelocRva = 0x4000, kImageSize = 0x5000;

void W16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); }
void W32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i)); }
void W64(std::vector<uint8_t>& b, size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + size_t(i)] = uint8_t(v >> (8 * i)); }
uint64_t R64(const std::vector<uint8_t>& b, size_t at) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | b[at + size_t(i)]; return v; }

struct Image {
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> fixups;   // RVAs
};

// The fixups: spread over .text, one straddling a page boundary, a pair back to back, one at the very end of .text.
Image Build(bool withRelocDir = true) {
    Image im;
    std::vector<uint8_t>& b = im.bytes;
    b.assign(kImageSize, 0);
    b[0] = 'M'; b[1] = 'Z'; W32(b, 0x3C, 0x80);
    std::memcpy(&b[0x80], "PE\0\0", 4);
    const size_t coff = 0x84;
    W16(b, coff, 0x8664); W16(b, coff + 2, 2); W16(b, coff + 16, 240);
    const size_t opt = coff + 20;
    W16(b, opt, 0x20B); W64(b, opt + 24, kPreferred); W32(b, opt + 56, kImageSize); W32(b, opt + 108, 16);
    const size_t sec = opt + 240;
    std::memcpy(&b[sec], ".text\0\0\0", 8); W32(b, sec + 8, kTextSize); W32(b, sec + 12, kTextRva);
    std::memcpy(&b[sec + 40], ".reloc\0\0", 8); W32(b, sec + 48, 0x1000); W32(b, sec + 52, kRelocRva);
    uint32_t x = 12345;
    for (uint32_t i = kTextRva; i < kTextRva + kTextSize; ++i) { x = x * 1103515245u + 12345u; b[i] = uint8_t(x >> 16); }
    for (uint32_t r : {0x1010u, 0x1100u, 0x1108u, 0x1ffcu, 0x2400u, 0x2a00u, 0x3333u, 0x3ff8u}) im.fixups.push_back(r);
    for (uint32_t r : im.fixups) W64(b, r, kPreferred + 0x1000 + (r * 37) % 0x3000);   // pointers into the image
    if (withRelocDir) {
        // One block per 4 KB page; the 0x1ffc fixup's page is 0x1000. Each block padded to 4 bytes with an absolute entry.
        size_t p = kRelocRva;
        for (uint32_t page = 0x1000; page < 0x4000; page += 0x1000) {
            std::vector<uint16_t> es;
            for (uint32_t r : im.fixups) if (r >= page && r < page + 0x1000) es.push_back(uint16_t(0xA000 | (r - page)));
            if (es.size() % 2) es.push_back(0);
            W32(b, p, page); W32(b, p + 4, uint32_t(8 + 2 * es.size()));
            for (size_t i = 0; i < es.size(); ++i) W16(b, p + 8 + 2 * i, es[i]);
            p += 8 + 2 * es.size();
        }
        W32(b, opt + 112 + 5 * 8, kRelocRva); W32(b, opt + 112 + 5 * 8 + 4, uint32_t(p - kRelocRva));
    }
    return im;
}

// The loader's work: add (base - preferred) to every fixup.
std::vector<uint8_t> LoadAt(const Image& im, uint64_t base) {
    std::vector<uint8_t> b = im.bytes;
    for (uint32_t r : im.fixups) W64(b, r, R64(b, r) + (base - kPreferred));
    return b;
}

struct Side { PeLayout pe; std::vector<uint32_t> relocs; };
bool Prepare(const std::vector<uint8_t>& img, Side& s) {
    if (!ParsePeLayout(img.data(), img.size(), s.pe)) return false;
    if (s.pe.relocSize == 0) { s.relocs.clear(); return true; }
    return ParseDir64Relocs(img.data() + s.pe.relocRva, s.pe.relocSize, s.relocs);
}

ModChallengeMsg Challenge(uint32_t id, std::vector<ChallengeRange> ranges) {
    ModChallengeMsg m; m.id = id; for (size_t i = 0; i < kChallengeNonceBytes; ++i) m.nonce[i] = uint8_t(i * 13 + id); m.ranges = std::move(ranges); return m;
}
}  // namespace

static void TestLayout() {
    const Image im = Build();
    Side s;
    CHECK(Prepare(im.bytes, s));
    CHECK(s.pe.preferredBase == kPreferred && s.pe.sizeOfImage == kImageSize && s.pe.textRva == kTextRva && s.pe.textSize == kTextSize);
    CHECK(s.relocs == im.fixups);
    // Broken headers are refused, never over-read.
    for (size_t n = 0; n < 0x200; n += 7) { PeLayout pe; CHECK(!ParsePeLayout(im.bytes.data(), n, pe)); }
    { auto b = im.bytes; b[0] = 'X'; PeLayout pe; CHECK(!ParsePeLayout(b.data(), b.size(), pe)); }
    { auto b = im.bytes; W32(b, 0x3C, 0xFFFFFFF0u); PeLayout pe; CHECK(!ParsePeLayout(b.data(), b.size(), pe)); }
    { auto b = im.bytes; W16(b, 0x84, 0x14c); PeLayout pe; CHECK(!ParsePeLayout(b.data(), b.size(), pe)); }          // x86
    { auto b = im.bytes; W16(b, 0x98, 0x10B); PeLayout pe; CHECK(!ParsePeLayout(b.data(), b.size(), pe)); }          // PE32
    { auto b = im.bytes; W32(b, 0x98 + 56, kImageSize * 2); PeLayout pe; CHECK(!ParsePeLayout(b.data(), b.size(), pe)); }   // bigger than mapped
    { auto b = im.bytes; W32(b, 0x98 + 240 + 8, 0x10000); PeLayout pe; CHECK(!ParsePeLayout(b.data(), b.size(), pe)); }    // .text past the image
    { auto b = im.bytes; std::memcpy(&b[0x98 + 240], ".data\0\0\0", 8); PeLayout pe; CHECK(!ParsePeLayout(b.data(), b.size(), pe)); }   // no .text
    CHECK(!ParsePeLayout(nullptr, 100, s.pe));
}

static void TestRelocs() {
    std::vector<uint32_t> out;
    // A block: page 0x2000, a DIR64 at 0x10, an absolute pad.
    std::vector<uint8_t> d(12, 0);
    W32(d, 0, 0x2000); W32(d, 4, 12); W16(d, 8, 0xA010); W16(d, 10, 0);
    CHECK(ParseDir64Relocs(d.data(), d.size(), out) && out.size() == 1 && out[0] == 0x2010);
    CHECK(ParseDir64Relocs(d.data(), 0, out) && out.empty());
    for (size_t n = 1; n < d.size(); ++n) CHECK(!ParseDir64Relocs(d.data(), n, out));
    { auto b = d; W16(b, 8, 0x3010); CHECK(!ParseDir64Relocs(b.data(), b.size(), out)); }   // HIGHLOW in an x64 image
    { auto b = d; W32(b, 4, 7); CHECK(!ParseDir64Relocs(b.data(), b.size(), out)); }        // block smaller than its header
    { auto b = d; W32(b, 4, 13); CHECK(!ParseDir64Relocs(b.data(), b.size(), out)); }       // odd
    { auto b = d; W32(b, 4, 400); CHECK(!ParseDir64Relocs(b.data(), b.size(), out)); }      // past the directory
    { auto b = d; W16(b, 10, 0xA010); CHECK(!ParseDir64Relocs(b.data(), b.size(), out)); }  // the same fixup twice
    // Unsorted entries come out sorted.
    { auto b = d; W16(b, 8, 0xA020); W16(b, 10, 0xA008); CHECK(ParseDir64Relocs(b.data(), b.size(), out) && out.size() == 2 && out[0] == 0x2008 && out[1] == 0x2020); }
}

static void TestAnswersAgree() {
    const Image im = Build();
    const uint64_t baseA = 0x7FF612340000ull, baseB = 0x7FFA00000000ull;
    const std::vector<uint8_t> a = LoadAt(im, baseA), b = LoadAt(im, baseB);
    Side sa, sb;
    CHECK(Prepare(a, sa) && Prepare(b, sb));
    // Ranges cut through fixups at both edges, cover back-to-back ones and the very end of .text.
    const std::vector<std::vector<ChallengeRange>> sets = {
        {{0, 4096}}, {{0x0ffd, 8}}, {{0x1100 - kTextRva + 3, 4}}, {{0x0ffc + 4, 2}}, {{kTextSize - 8, 8}}, {{kTextSize - 6, 6}}, {{kTextSize - 3, 3}},
        {{0x100, 16}, {0x1400, 4096}, {0x2333, 200}},
    };
    uint32_t id = 1;
    for (const auto& ranges : sets) {
        const ModChallengeMsg m = Challenge(id++, ranges);
        Sha256Digest da{}, db{};
        CHECK(ComputeAnswer(a.data(), sa.pe, sa.relocs, baseA, m, da));
        CHECK(ComputeAnswer(b.data(), sb.pe, sb.relocs, baseB, m, db));
        CHECK(da == db);
        // Without normalising, a range with a fixup differs between the two loads.
        Sha256Digest ra{}, rb{};
        CHECK(ComputeAnswer(a.data(), sa.pe, {}, baseA, m, ra) && ComputeAnswer(b.data(), sb.pe, {}, baseB, m, rb));
        bool touchesFixup = false;
        for (const ChallengeRange& r : ranges)
            for (uint32_t fx : im.fixups) if (fx + 8 > kTextRva + r.offset && fx < kTextRva + r.offset + r.length) touchesFixup = true;
        // Bytes 5-7 of a pointer are equal at both bases (0x00007F..), so only ranges reaching the low bytes must differ.
        if (touchesFixup && !(ranges.size() == 1 && ranges[0].offset == kTextSize - 3)) CHECK(ra != rb);
    }
    // Another nonce, another id: another answer.
    ModChallengeMsg m1 = Challenge(5, {{0, 4096}}), m2 = m1;
    m2.nonce[0] ^= 1;
    Sha256Digest d1{}, d2{}, d3{};
    CHECK(ComputeAnswer(a.data(), sa.pe, sa.relocs, baseA, m1, d1) && ComputeAnswer(a.data(), sa.pe, sa.relocs, baseA, m2, d2) && d1 != d2);
    m2 = m1; m2.id = 6;
    CHECK(ComputeAnswer(a.data(), sa.pe, sa.relocs, baseA, m2, d3) && d1 != d3);
    // A patched code byte (a hook in the range) changes the answer; one outside the ranges does not.
    std::vector<uint8_t> patched = b;
    patched[kTextRva + 0x200] ^= 0xFF;
    Sha256Digest dp{}, dq{};
    CHECK(ComputeAnswer(patched.data(), sb.pe, sb.relocs, baseB, m1, dp) && dp != d1);
    const ModChallengeMsg far = Challenge(9, {{0x2000, 16}});
    CHECK(ComputeAnswer(patched.data(), sb.pe, sb.relocs, baseB, far, dp) && ComputeAnswer(a.data(), sa.pe, sa.relocs, baseA, far, dq) && dp == dq);
    // A patched pointer (a redirected call target) changes it too.
    patched = b; W64(patched, 0x1100, R64(patched, 0x1100) + 0x40);
    CHECK(ComputeAnswer(patched.data(), sb.pe, sb.relocs, baseB, Challenge(11, {{0x100, 16}}), dp) &&
          ComputeAnswer(a.data(), sa.pe, sa.relocs, baseA, Challenge(11, {{0x100, 16}}), dq) && dp != dq);
    // An image without a relocation directory, loaded at its preferred base, answers too.
    const Image fixed = Build(false);
    Side sf; CHECK(Prepare(fixed.bytes, sf) && sf.relocs.empty());
    CHECK(ComputeAnswer(fixed.bytes.data(), sf.pe, sf.relocs, kPreferred, m1, dp));
}

static void TestBounds() {
    const Image im = Build();
    Side s; CHECK(Prepare(im.bytes, s));
    Sha256Digest d{};
    CHECK(!ComputeAnswer(im.bytes.data(), s.pe, s.relocs, kPreferred, Challenge(1, {{kTextSize - 4, 8}}), d));    // past .text
    CHECK(!ComputeAnswer(im.bytes.data(), s.pe, s.relocs, kPreferred, Challenge(1, {{0xFFFFFFF0u, 100}}), d));   // overflow
    CHECK(!ComputeAnswer(im.bytes.data(), s.pe, s.relocs, kPreferred, Challenge(1, {}), d));
    CHECK(!ComputeAnswer(im.bytes.data(), s.pe, s.relocs, kPreferred, Challenge(1, {{0, 0}}), d));
    CHECK(!ComputeAnswer(im.bytes.data(), s.pe, s.relocs, kPreferred, Challenge(1, {{0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}}), d));
    std::vector<uint32_t> badRelocs = {kImageSize - 4};   // a fixup whose 8 bytes leave the image
    PeLayout pe = s.pe; pe.textSize = kImageSize - kTextRva;
    CHECK(!ComputeAnswer(im.bytes.data(), pe, badRelocs, kPreferred, Challenge(1, {{kImageSize - kTextRva - 8, 8}}), d));
    // PickRanges stays inside .text, also for a section shorter than one range.
    uint64_t seed = 42;
    for (int i = 0; i < 200; ++i) {
        for (const ChallengeRange& r : PickRanges(ChallengeMix(seed), kTextSize)) CHECK(r.length == kChallengeRangeBytes && r.offset + r.length <= kTextSize);
        for (const ChallengeRange& r : PickRanges(ChallengeMix(seed), 100)) CHECK(r.length == 100 && r.offset == 0);
    }
    CHECK(PickRanges(1, 0).empty() && PickRanges(1, kTextSize, 9).size() == kMaxChallengeRanges);
    CHECK(PickRanges(1, kTextSize) == PickRanges(1, kTextSize) && !(PickRanges(1, kTextSize) == PickRanges(2, kTextSize)));
}

static void TestWire() {
    ModChallengeMsg m = Challenge(77, {{1, 2}, {3000, 4096}});
    auto b = Encode(m);
    ModChallengeMsg o;
    CHECK(Decode(b.data(), b.size(), o) && o.id == 77 && o.ranges == m.ranges && std::memcmp(o.nonce, m.nonce, kChallengeNonceBytes) == 0);
    for (size_t n = 0; n < b.size(); ++n) CHECK(!Decode(b.data(), n, o));
    b.push_back(0); CHECK(!Decode(b.data(), b.size(), o)); b.pop_back();
    { auto c = b; c[4 + 4 + 16] = 0; CHECK(!Decode(c.data(), c.size(), o)); }       // no ranges
    { auto c = b; c[4 + 4 + 16] = 5; CHECK(!Decode(c.data(), c.size(), o)); }       // too many
    { auto c = b; c[4 + 4 + 16 + 1 + 4] = 0; c[4 + 4 + 16 + 1 + 5] = 0; CHECK(!Decode(c.data(), c.size(), o)); }   // length 0
    { ModChallengeMsg big = Challenge(1, {{0, 4097}}); auto c = Encode(big); CHECK(!Decode(c.data(), c.size(), o)); }
    ModAnswerMsg a; a.id = 77; a.digest[0] = 0xAB;
    b = Encode(a);
    ModAnswerMsg ao;
    CHECK(Decode(b.data(), b.size(), ao) && ao.id == 77 && ao.status == AnswerStatus::Ok && ao.digest == a.digest);
    for (size_t n = 0; n < b.size(); ++n) CHECK(!Decode(b.data(), n, ao));
    a.status = AnswerStatus::Unable; b = Encode(a);
    CHECK(b.size() == 4 + 5 && Decode(b.data(), b.size(), ao) && ao.status == AnswerStatus::Unable);
    { auto c = b; c[8] = 2; CHECK(!Decode(c.data(), c.size(), ao)); }
    { ModChallengeMsg x; auto c = Encode(a); CHECK(!Decode(c.data(), c.size(), x)); }   // wrong type
}

static void TestTracker() {
    ChallengeTracker t;
    CHECK(!t.Due(1000000) && !t.active());
    t.Start(1000);
    CHECK(!t.Due(1000 + kFirstChallengeMs - 1) && t.Due(1000 + kFirstChallengeMs));
    Sha256Digest want{}; want[0] = 1;
    uint64_t now = 1000 + kFirstChallengeMs;
    t.Sent(1, want, now, 0);
    CHECK(t.pending() && !t.Due(now + kChallengeMinGapMs));
    ModAnswerMsg a; a.id = 1; a.digest = want;
    ModAnswerMsg stale = a; stale.id = 2;
    CHECK(t.OnAnswer(stale, now + 10, 50) == ChallengeOutcome::Stale && t.pending());
    CHECK(t.OnAnswer(a, now + 100, 50) == ChallengeOutcome::Ok && !t.pending() && t.passes() == 1 && t.fails() == 0);
    CHECK(t.OnAnswer(a, now + 120, 50) == ChallengeOutcome::Stale);                         // answered already
    CHECK(!t.Due(now + kChallengeMinGapMs - 1) && t.Due(now + kChallengeMinGapMs));         // rand 0: the shortest gap
    now += kChallengeMinGapMs;
    t.Sent(2, want, now, kChallengeGapSpanMs - 1);
    a.id = 2;
    CHECK(t.OnAnswer(a, now + 2 * 50 + kChallengeLateSlackMs + 1, 50) == ChallengeOutcome::Late && t.fails() == 0);
    CHECK(!t.Due(now + kChallengeMinGapMs + kChallengeGapSpanMs - 2) && t.Due(now + kChallengeMinGapMs + kChallengeGapSpanMs - 1));
    // Wrong, Unable and Timeout each count; at kMaxChallengeFails the host removes the player.
    t.Sent(3, want, now, 0); a.id = 3; a.digest[0] = 2;
    CHECK(t.OnAnswer(a, now + 10, 50) == ChallengeOutcome::Wrong && t.fails() == 1 && !t.ShouldRemove());
    t.Sent(4, want, now, 0); a.id = 4; a.status = AnswerStatus::Unable; a.digest = want;
    CHECK(t.OnAnswer(a, now + 10, 50) == ChallengeOutcome::Wrong && t.fails() == 2 && !t.ShouldRemove());
    t.Sent(5, want, now, 0);
    CHECK(t.Tick(now + kChallengeTimeoutMs - 1) == ChallengeOutcome::None);
    CHECK(t.Tick(now + kChallengeTimeoutMs) == ChallengeOutcome::Timeout && t.fails() == 3 && t.ShouldRemove());
    CHECK(t.Tick(now + kChallengeTimeoutMs + 1) == ChallengeOutcome::None);                // once
    t.Stop();
    CHECK(!t.Due(now + 10000000) && t.Tick(now + 10000000) == ChallengeOutcome::None);
    t.Start(0);
    CHECK(t.fails() == 0 && t.passes() == 0);                                              // a fresh start forgets
    for (uint8_t o = 0; o <= uint8_t(ChallengeOutcome::Stale); ++o) CHECK(std::strlen(ChallengeOutcomeName(ChallengeOutcome(o))) > 1);
}

int main() {
    TestLayout();
    TestRelocs();
    TestAnswersAgree();
    TestBounds();
    TestWire();
    TestTracker();
    if (g_fail) { std::printf("test_hsm_mod_challenge: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_mod_challenge: PASS\n");
    return 0;
}
