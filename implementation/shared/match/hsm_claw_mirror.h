// Half Wolverine's claws on other players' screens. Its visible blades are scenery on the local fighter, so nothing draws
// them on anyone else's copy of that fighter. Each player publishes where their blades sit as lobby member data, and every
// other player draws matching blades. This is the wire format: pure, and every field from a peer is bounded. Peers never
// name an asset: they pick bone or adamantium, and the mesh paths are this mod's own.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

inline constexpr const char* kMemberClawKey = "hsm_claw";
inline constexpr size_t kMaxClawBlades = 6;       // Half Wolverine: three on each hand
inline constexpr size_t kMaxClawValue = 512;
inline constexpr double kMaxClawOffset = 200.0;   // cm from the hand bone
inline constexpr size_t kMaxClawBone = 40;

struct ClawBlade { std::string bone; double x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0; };
struct ClawState {
    bool out = false;
    bool bone = true;   // bone or adamantium blades
    std::vector<ClawBlade> blades;
};

inline bool ClawBoneOk(std::string_view s) {
    if (s.empty() || s.size() > kMaxClawBone) return false;
    for (char c : s) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

// "" when sheathed. Out: "B" or "A", then one "bone,x,y,z,pitch,yaw,roll" per blade, ';' between.
inline std::string EncodeClaws(const ClawState& s) {
    if (!s.out) return {};
    std::string out = s.bone ? "B" : "A";
    size_t n = 0;
    for (const ClawBlade& b : s.blades) {
        if (n++ >= kMaxClawBlades || !ClawBoneOk(b.bone)) break;
        char line[128];
        std::snprintf(line, sizeof line, ";%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f", b.bone.c_str(), b.x, b.y, b.z, b.pitch, b.yaw, b.roll);
        out += line;
    }
    return out;
}

// Peer-written: anything malformed means no claws drawn.
inline ClawState DecodeClaws(std::string_view v) {
    ClawState s;
    if (v.size() < 2 || v.size() > kMaxClawValue || (v[0] != 'B' && v[0] != 'A')) return s;
    ClawState parsed;
    parsed.bone = v[0] == 'B';
    size_t at = 1;
    while (at < v.size()) {
        if (v[at] != ';' || parsed.blades.size() >= kMaxClawBlades) return s;
        size_t end = v.find(';', at + 1);
        if (end == std::string_view::npos) end = v.size();
        const std::string entry(v.substr(at + 1, end - at - 1));
        const size_t comma = entry.find(',');
        if (comma == std::string::npos || !ClawBoneOk(std::string_view(entry).substr(0, comma))) return s;
        ClawBlade b;
        b.bone = entry.substr(0, comma);
        double* const f[6] = {&b.x, &b.y, &b.z, &b.pitch, &b.yaw, &b.roll};
        const char* p = entry.c_str() + comma;
        for (int i = 0; i < 6; ++i) {
            if (*p != ',') return s;
            char* stop = nullptr;
            const double d = std::strtod(p + 1, &stop);
            if (stop == p + 1 || !std::isfinite(d)) return s;
            if (std::fabs(d) > (i < 3 ? kMaxClawOffset : 360.0)) return s;
            *f[i] = d;
            p = stop;
        }
        if (*p != '\0') return s;
        parsed.blades.push_back(std::move(b));
        at = end;
    }
    if (parsed.blades.empty()) return s;
    parsed.out = true;
    return parsed;
}

}  // namespace hsm
