// HSMMod challenge-response (docs/PLAN_MOD_RULES.md 4c): the host asks a client for SHA-256(nonce || id || a few byte
// ranges of HSMMod's loaded code section) and computes the same from its own loaded copy. Two machines on the same build
// (the build string's hash matches, session/hsm_mod_rules.h CheckJoinBuild) have the same code bytes once each side undoes
// its base relocations, so ASLR does not change the answer. Pure, no Win32 types: the caller passes its module's mapped
// image (base pointer and SizeOfImage); gameplay/mod_challenge.cpp does that.
//
// Relocation normalisation: an IMAGE_REL_BASED_DIR64 fixup holds (preferred base + target RVA) + (actual base - preferred
// base) after loading, so (value - actual base) is the target RVA on every machine. Each fixup that overlaps a range has
// its 8 bytes replaced by that value (little-endian) before hashing; a fixup that straddles the range's edge contributes
// only the bytes inside the range.
//
// What this proves: the client can produce the bytes of an unmodified HSMMod quickly. A patched client can keep a clean
// copy of the original code in memory and answer from it, so this raises the cost of a modified client; it is not proof.
// One bad answer is not enough to act on: the host removes a player after kMaxChallengeFails failures.
#pragma once
#include "hsm_protocol.h"
#include "util/hsm_sha256.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hsm {

inline constexpr size_t kChallengeNonceBytes = 16;
inline constexpr size_t kMaxChallengeRanges = 4;
inline constexpr uint16_t kChallengeRangeBytes = 4096;     // each range the host picks (shorter at the section's end)
inline constexpr uint64_t kFirstChallengeMs = 5000;         // after the player's ModList passed
inline constexpr uint64_t kChallengeMinGapMs = 60000;       // then every 60..180 s at random
inline constexpr uint64_t kChallengeGapSpanMs = 120000;
inline constexpr uint64_t kChallengeTimeoutMs = 10000;      // no answer by then: a failure
inline constexpr uint64_t kChallengeLateSlackMs = 1500;     // later than 2 x RTT + this: logged as late, still counted ok
inline constexpr uint8_t kMaxChallengeFails = 3;            // the host removes the player at this many failures
inline constexpr size_t kMaxRelocs = 1u << 20;              // DIR64 fixups read from one image

// ------------------------------------------------------------------ the image

struct PeLayout {
    uint64_t preferredBase = 0;
    uint32_t sizeOfImage = 0;
    uint32_t textRva = 0, textSize = 0;     // the .text section (its VirtualSize)
    uint32_t relocRva = 0, relocSize = 0;   // the base relocation directory (0 when the image has none)
};

namespace challenge_detail {
inline uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t Rd64(const uint8_t* p) { return uint64_t(Rd32(p)) | uint64_t(Rd32(p + 4)) << 32; }
inline bool Fits(uint64_t off, uint64_t len, uint64_t n) { return off <= n && len <= n - off; }
}  // namespace challenge_detail

// Reads the PE32+ headers of a mapped image (`n`: bytes readable from `img`). False for anything that is not a sane x64
// image: bad signatures, sections or directories outside SizeOfImage, no .text.
inline bool ParsePeLayout(const uint8_t* img, size_t n, PeLayout& out) {
    using namespace challenge_detail;
    out = PeLayout{};
    if (!img || n < 0x40 || img[0] != 'M' || img[1] != 'Z') return false;
    const uint32_t pe = Rd32(img + 0x3C);
    if (!Fits(pe, 24, n) || std::memcmp(img + pe, "PE\0\0", 4) != 0) return false;
    const uint8_t* coff = img + pe + 4;
    if (Rd16(coff) != 0x8664) return false;
    const uint16_t sections = Rd16(coff + 2), optSize = Rd16(coff + 16);
    const uint64_t opt = uint64_t(pe) + 24;
    if (optSize < 112 + 16 * 8 || !Fits(opt, optSize, n)) return false;
    const uint8_t* o = img + opt;
    if (Rd16(o) != 0x20B) return false;                       // PE32+
    out.preferredBase = Rd64(o + 24);
    out.sizeOfImage = Rd32(o + 56);
    const uint32_t dirs = Rd32(o + 108);
    if (out.sizeOfImage == 0 || out.sizeOfImage > n) return false;
    if (dirs > 5) {
        out.relocRva = Rd32(o + 112 + 5 * 8);
        out.relocSize = Rd32(o + 112 + 5 * 8 + 4);
        if (!Fits(out.relocRva, out.relocSize, out.sizeOfImage)) return false;
    }
    const uint64_t sec = opt + optSize;
    if (sections == 0 || sections > 96 || !Fits(sec, uint64_t(sections) * 40, n)) return false;
    for (uint16_t i = 0; i < sections; ++i) {
        const uint8_t* s = img + sec + size_t(i) * 40;
        if (std::memcmp(s, ".text\0\0\0", 8) != 0) continue;
        out.textSize = Rd32(s + 8);
        out.textRva = Rd32(s + 12);
        if (out.textSize == 0 || !Fits(out.textRva, out.textSize, out.sizeOfImage)) return false;
        return true;
    }
    return false;
}

// The DIR64 fixups of a base relocation directory, as RVAs, sorted. Absolute (padding) entries are skipped; any other
// type, a block outside the directory, a fixup listed twice or more than kMaxRelocs refuses the image.
inline bool ParseDir64Relocs(const uint8_t* dir, size_t n, std::vector<uint32_t>& out) {
    using namespace challenge_detail;
    out.clear();
    size_t p = 0;
    while (p < n) {
        if (n - p < 8) return false;
        const uint32_t page = Rd32(dir + p), size = Rd32(dir + p + 4);
        if (size < 8 || (size & 1) || size > n - p) return false;
        for (size_t k = p + 8; k + 2 <= p + size; k += 2) {
            const uint16_t e = Rd16(dir + k);
            const unsigned type = e >> 12;
            if (type == 0) continue;
            if (type != 10) return false;                     // IMAGE_REL_BASED_DIR64 only, in an x64 image
            const uint64_t rva = uint64_t(page) + (e & 0x0FFF);
            if (rva > 0xFFFFFFFFull || out.size() >= kMaxRelocs) return false;
            out.push_back(uint32_t(rva));
        }
        p += size;
    }
    std::sort(out.begin(), out.end());
    for (size_t i = 1; i < out.size(); ++i) if (out[i] == out[i - 1]) return false;
    return true;
}

// ------------------------------------------------------------------ the messages

struct ChallengeRange {
    uint32_t offset = 0;   // from the start of .text
    uint16_t length = 0;
    bool operator==(const ChallengeRange& o) const { return offset == o.offset && length == o.length; }
};

// ModChallenge (67), host -> client: [id u32][nonce 16][count u8 1..4] then per range [offset u32][length u16 1..4096].
struct ModChallengeMsg {
    uint32_t id = 0;
    uint8_t nonce[kChallengeNonceBytes] = {};
    std::vector<ChallengeRange> ranges;
};
// ModAnswer (68), client -> host: [id u32][status u8] then the digest when status is Ok.
enum class AnswerStatus : uint8_t { Ok = 0, Unable = 1 };
struct ModAnswerMsg {
    uint32_t id = 0;
    AnswerStatus status = AnswerStatus::Ok;
    Sha256Digest digest{};
};

inline std::vector<uint8_t> Encode(const ModChallengeMsg& m) {
    Writer w; w.header(MsgType::ModChallenge);
    w.u32(m.id);
    w.buf.insert(w.buf.end(), m.nonce, m.nonce + kChallengeNonceBytes);
    const size_t n = m.ranges.size() < kMaxChallengeRanges ? m.ranges.size() : kMaxChallengeRanges;
    w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) { w.u32(m.ranges[i].offset); w.u16(m.ranges[i].length); }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ModChallengeMsg& m) {
    Reader r(d, n); MsgType t;
    if (!r.header(t) || t != MsgType::ModChallenge) return false;
    m = ModChallengeMsg{};
    m.id = r.u32();
    for (uint8_t& b : m.nonce) b = r.u8();
    const uint8_t count = r.u8();
    if (!r.ok() || count == 0 || count > kMaxChallengeRanges) return false;
    for (uint8_t i = 0; i < count; ++i) {
        ChallengeRange cr; cr.offset = r.u32(); cr.length = r.u16();
        if (!r.ok() || cr.length == 0 || cr.length > kChallengeRangeBytes) return false;
        m.ranges.push_back(cr);
    }
    return r.done();
}
inline std::vector<uint8_t> Encode(const ModAnswerMsg& m) {
    Writer w; w.header(MsgType::ModAnswer);
    w.u32(m.id); w.u8(uint8_t(m.status));
    if (m.status == AnswerStatus::Ok) w.buf.insert(w.buf.end(), m.digest.begin(), m.digest.end());
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ModAnswerMsg& m) {
    Reader r(d, n); MsgType t;
    if (!r.header(t) || t != MsgType::ModAnswer) return false;
    m = ModAnswerMsg{};
    m.id = r.u32();
    const uint8_t s = r.u8();
    if (!r.ok() || s > 1) return false;
    m.status = AnswerStatus(s);
    if (m.status == AnswerStatus::Ok) for (uint8_t& b : m.digest) b = r.u8();
    return r.done();
}

// ------------------------------------------------------------------ picking and answering

// splitmix64: the host's ranges from one random 64-bit value (it sends them, so the client never needs the rule).
inline uint64_t ChallengeMix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline std::vector<ChallengeRange> PickRanges(uint64_t seed, uint32_t textSize, size_t count = 3) {
    std::vector<ChallengeRange> out;
    if (textSize == 0) return out;
    if (count > kMaxChallengeRanges) count = kMaxChallengeRanges;
    for (size_t i = 0; i < count; ++i) {
        ChallengeRange r;
        r.length = uint16_t(textSize < kChallengeRangeBytes ? textSize : kChallengeRangeBytes);
        const uint32_t span = textSize - r.length + 1;
        r.offset = uint32_t(ChallengeMix(seed) % span);
        out.push_back(r);
    }
    return out;
}

// One range's bytes with every overlapping DIR64 fixup replaced by (value - actualBase). False when the range or a fixup
// it touches lies outside .text or the image.
inline bool NormalizedRange(const uint8_t* img, const PeLayout& pe, const std::vector<uint32_t>& relocs, uint64_t actualBase,
                            const ChallengeRange& r, std::vector<uint8_t>& out) {
    using namespace challenge_detail;
    if (r.length == 0 || !Fits(r.offset, r.length, pe.textSize)) return false;
    const uint32_t start = pe.textRva + r.offset, end = start + r.length;
    out.assign(img + start, img + end);
    // The first fixup whose 8 bytes can reach `start`.
    auto it = std::lower_bound(relocs.begin(), relocs.end(), start >= 7 ? start - 7 : 0u);
    for (; it != relocs.end() && *it < end; ++it) {
        const uint32_t fx = *it;
        if (!Fits(fx, 8, pe.sizeOfImage)) return false;
        const uint64_t v = Rd64(img + fx) - actualBase;
        for (uint32_t k = 0; k < 8; ++k) {
            const uint64_t at = uint64_t(fx) + k;
            if (at >= start && at < end) out[size_t(at - start)] = uint8_t(v >> (8 * k));
        }
    }
    return true;
}

// The answer: SHA-256(nonce || id (u32 LE) || each normalised range in order). False when any range is out of bounds.
inline bool ComputeAnswer(const uint8_t* img, const PeLayout& pe, const std::vector<uint32_t>& relocs, uint64_t actualBase,
                          const ModChallengeMsg& m, Sha256Digest& out) {
    if (m.ranges.empty() || m.ranges.size() > kMaxChallengeRanges) return false;
    Sha256 h;
    h.Update(m.nonce, kChallengeNonceBytes);
    const uint8_t id[4] = {uint8_t(m.id), uint8_t(m.id >> 8), uint8_t(m.id >> 16), uint8_t(m.id >> 24)};
    h.Update(id, 4);
    std::vector<uint8_t> bytes;
    for (const ChallengeRange& r : m.ranges) {
        if (!NormalizedRange(img, pe, relocs, actualBase, r, bytes)) return false;
        h.Update(bytes.data(), bytes.size());
    }
    out = h.Final();
    return true;
}

// ------------------------------------------------------------------ the host's side, per player

enum class ChallengeOutcome : uint8_t {
    None,       // nothing happened
    Ok,         // the right answer in time
    Late,       // the right answer, later than 2 x RTT + slack (logged; counts as ok)
    Wrong,      // another digest, or Unable: a failure
    Timeout,    // no answer within kChallengeTimeoutMs: a failure
    Stale,      // an answer to no pending challenge: ignored
};
inline const char* ChallengeOutcomeName(ChallengeOutcome o) {
    switch (o) {
        case ChallengeOutcome::None: return "none";
        case ChallengeOutcome::Ok: return "ok";
        case ChallengeOutcome::Late: return "late";
        case ChallengeOutcome::Wrong: return "wrong";
        case ChallengeOutcome::Timeout: return "timeout";
        case ChallengeOutcome::Stale: return "stale";
    }
    return "?";
}

class ChallengeTracker {
public:
    // The player passed its ModList at nowMs: the first challenge comes kFirstChallengeMs later.
    void Start(uint64_t nowMs) { *this = ChallengeTracker{}; active_ = true; nextMs_ = nowMs + kFirstChallengeMs; }
    void Stop() { active_ = false; pending_ = false; }
    bool active() const { return active_; }
    bool Due(uint64_t nowMs) const { return active_ && !pending_ && nowMs >= nextMs_; }
    // A challenge went out; `rand` picks the gap to the next one.
    void Sent(uint32_t id, const Sha256Digest& expect, uint64_t nowMs, uint64_t rand) {
        pending_ = true; id_ = id; expect_ = expect; sentMs_ = nowMs;
        nextMs_ = nowMs + kChallengeMinGapMs + rand % kChallengeGapSpanMs;
    }
    ChallengeOutcome OnAnswer(const ModAnswerMsg& a, uint64_t nowMs, uint64_t rttMs) {
        if (!active_ || !pending_ || a.id != id_) return ChallengeOutcome::Stale;
        pending_ = false;
        if (a.status != AnswerStatus::Ok || a.digest != expect_) { ++fails_; return ChallengeOutcome::Wrong; }
        ++passes_;
        return nowMs - sentMs_ > 2 * rttMs + kChallengeLateSlackMs ? ChallengeOutcome::Late : ChallengeOutcome::Ok;
    }
    ChallengeOutcome Tick(uint64_t nowMs) {
        if (!active_ || !pending_ || nowMs - sentMs_ < kChallengeTimeoutMs) return ChallengeOutcome::None;
        pending_ = false; ++fails_;
        return ChallengeOutcome::Timeout;
    }
    bool ShouldRemove() const { return fails_ >= kMaxChallengeFails; }
    uint8_t fails() const { return fails_; }
    uint32_t passes() const { return passes_; }
    bool pending() const { return pending_; }
    uint32_t id() const { return id_; }
private:
    bool active_ = false, pending_ = false;
    uint32_t id_ = 0, passes_ = 0;
    uint8_t fails_ = 0;
    uint64_t sentMs_ = 0, nextMs_ = 0;
    Sha256Digest expect_{};
};

}  // namespace hsm
