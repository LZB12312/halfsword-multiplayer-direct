// The mod build every player compares before a match (pure, no engine types). The protocol revision only says that two
// mods can talk; two-player tests ran different builds of the same revision and chased bugs one side had already fixed.
// Each machine sends its build string (Hello, Welcome, the Steam lobby's member data); the lobby warns about every other
// player on a different build, and the host does not start a match while one is in, unless allow_build_mismatch=1.
// From 0.8.0-pre26 the string ends with the SHA-256 of the mod's own DLL file, so two players match only on identical
// binaries: a self-built or edited DLL no longer passes for a release by its version and commit alone. Each machine
// reports its own hash, so this keeps honest players on one build; it is not proof against a client written to lie.
#pragma once
#include "hsm_protocol.h"
#include "util/hsm_sha256.h"
#include <cstddef>
#include <string>
#include <vector>

namespace hsm {

constexpr size_t kBuildHashHex = 12;   // hex digits of the DLL's SHA-256 in the build string (48 bits)

// The first kBuildHashHex hex digits of a SHA-256: "1a2b3c4d5e6f".
inline std::string BuildHash(const Sha256Digest& d) { return Sha256Hex(d).substr(0, kBuildHashHex); }

// "0.6.4", or "0.6.4 (db82570)" when build.sh knew its commit ("db82570-dirty" with local changes), then " #<hash>" when
// the DLL could hash itself. At most kMaxModBuild bytes: a long describe is cut first, so the hash is always whole.
inline std::string ModBuildString(const std::string& version, const std::string& describe, const std::string& hash = {}) {
    const std::string tail = hash.empty() ? std::string{} : " #" + hash;
    std::string s = describe.empty() ? version : version + " (" + describe + ")";
    if (s.size() + tail.size() > kMaxModBuild) s.resize(kMaxModBuild > tail.size() ? kMaxModBuild - tail.size() : 0);
    s += tail;
    if (s.size() > kMaxModBuild) s.resize(kMaxModBuild);
    return s;
}

struct PeerBuild {
    std::string name;
    std::string build;   // empty: not known yet (the peer's Hello or member data has not come)
};

// The other players whose known build differs from mine, as "NAME on BUILD".
inline std::vector<std::string> BuildMismatches(const std::string& mine, const std::vector<PeerBuild>& others) {
    std::vector<std::string> out;
    for (const PeerBuild& p : others)
        if (!p.build.empty() && p.build != mine) out.push_back((p.name.empty() ? std::string("another player") : p.name) + " on " + p.build);
    return out;
}

// The lobby's line: "Bob on 0.6.3 - update" (several joined with "; "), or empty when every known build matches.
inline std::string BuildWarning(const std::string& mine, const std::vector<PeerBuild>& others) {
    const std::vector<std::string> m = BuildMismatches(mine, others);
    if (m.empty()) return {};
    std::string s;
    for (const std::string& x : m) s += (s.empty() ? "" : "; ") + x;
    return s + " - update";
}

// The host starts a match only when every known build matches mine, or the override is on.
inline bool MayStartMatch(const std::string& mine, const std::vector<PeerBuild>& others, bool allowMismatch) {
    return allowMismatch || BuildMismatches(mine, others).empty();
}

}  // namespace hsm
