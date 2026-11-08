// Steam accounts kept out of every HalfSwordMP lobby, whoever hosts it. Each is listed by a hash of its Steam ID, so the
// list doesn't print the number. Every host refuses them at the hello (session.cpp); their own copy won't host or join, and
// nobody else's joins a lobby they own. The build gate keeps older builds, which lack the list, out of the same lobbies.
// Game-independent, tested in test_hsm_global_bans.cpp.
#pragma once
#include "util/hsm_sha256.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace hsm {

// SHA-256 of "hsm-ban:<SteamID64>", as lower-case hex.
inline std::string GlobalBanHash(uint64_t steamId) {
    const std::string text = "hsm-ban:" + std::to_string(steamId);
    return Sha256Hex(Sha256Of(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

inline constexpr std::string_view kGlobalBans[] = {
    "4f7991c7bdad2a7f3b92ecfb1124a0794d263ff16aca175ef61370669ae5dad1",   // 2026-11
};

template <size_t N>
inline bool GloballyBannedIn(uint64_t steamId, const std::string_view (&list)[N]) {
    if (!steamId) return false;   // no Steam ID (a direct-IP player): nothing to match
    const std::string h = GlobalBanHash(steamId);
    for (std::string_view b : list) if (b == h) return true;
    return false;
}
inline bool GloballyBanned(uint64_t steamId) { return GloballyBannedIn(steamId, kGlobalBans); }

}  // namespace hsm
