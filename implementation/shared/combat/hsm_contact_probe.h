// What a raw contact with a stand-in touched, for the log (HELP IMPROVE THE MOD only; gameplay/hit_probe.h reads the
// game). Deal Complex Damage only sees the Mesh, so a contact elsewhere never reached the hit counters. Each raw contact
// is sorted here:
//   body     on the Mesh, within kContactOffBodyCm of the body as it is drawn
//   off-body on the Mesh, but farther from the drawn body than that: the physics body is not where the bones are
//   part     another component of the fighter
//   gear     a component of an actor the fighter carries or wears
//   unread   on the Mesh, with no distance to go by
// Nothing here changes a hit. A contact is an event: both sides of one touch can report it, so counts run high.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hsm::combat {

constexpr double kContactOffBodyCm = 8.0;     // a contact point sits on or between two shapes: more than that is not the drawn body
constexpr int kProbeEventsPerSec = 128;       // the rest are only counted as dropped: hit events can flood
constexpr int kProbeDetailsPerWindow = 16;    // lines of their own between two health reports, part, gear and off-body
constexpr int kProbeBodyDetailsPerWindow = 4; // the same for plain body contacts: a baseline to compare with
constexpr size_t kProbeTallyRows = 64;        // different (part, bone) pairs a window keeps apart
constexpr size_t kProbeTopRows = 5;           // of those, named in the health line

enum class ContactPlace : uint8_t { Body, OffBody, Part, Gear, Unread };

inline ContactPlace PlaceOfContact(bool onMesh, bool onFighter, bool measured, double offCm) {
    if (!onMesh) return onFighter ? ContactPlace::Part : ContactPlace::Gear;
    if (!measured) return ContactPlace::Unread;
    return offCm > kContactOffBodyCm ? ContactPlace::OffBody : ContactPlace::Body;
}

inline const char* ContactPlaceName(ContactPlace p) {
    switch (p) {
        case ContactPlace::Body: return "body";
        case ContactPlace::OffBody: return "off-body";
        case ContactPlace::Part: return "part";
        case ContactPlace::Gear: return "gear";
        case ContactPlace::Unread: return "unread";
    }
    return "?";
}

// At most kProbeEventsPerSec a second, counted from the first event of each second.
class ProbeBudget {
public:
    bool Take(double nowMs) {
        if (!started_ || nowMs - windowMs_ >= 1000.0 || nowMs < windowMs_) { started_ = true; windowMs_ = nowMs; used_ = 0; }
        if (used_ >= kProbeEventsPerSec) return false;
        ++used_;
        return true;
    }
private:
    bool started_ = false;
    double windowMs_ = 0;
    int used_ = 0;
};

struct ProbeVec { double x = 0, y = 0, z = 0; };
struct BodyOffset { double forward = 0, right = 0, up = 0; };

// A point in the fighter's own frame, from the pelvis: the frame comes from the bones, so "behind" means the back of the
// body whether it stands or lies face down (the actor's rotation says neither on a stand-in). False when the bones
// are too close together to give one. Unreal's axes: forward = right x up.
inline bool BodyFrameOffset(const ProbeVec& pelvis, const ProbeVec& head, const ProbeVec& shoulderL, const ProbeVec& shoulderR,
                            const ProbeVec& point, BodyOffset& out) {
    auto sub = [](const ProbeVec& a, const ProbeVec& b) { return ProbeVec{a.x - b.x, a.y - b.y, a.z - b.z}; };
    auto dot = [](const ProbeVec& a, const ProbeVec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    auto unit = [&](const ProbeVec& v, ProbeVec& u) {
        const double n = std::sqrt(dot(v, v));
        if (!(n > 1e-3)) return false;
        u = {v.x / n, v.y / n, v.z / n};
        return true;
    };
    ProbeVec right, up;
    if (!unit(sub(shoulderR, shoulderL), right)) return false;
    const ProbeVec spine = sub(head, pelvis);
    const double along = dot(spine, right);
    if (!unit({spine.x - right.x * along, spine.y - right.y * along, spine.z - right.z * along}, up)) return false;
    const ProbeVec forward{right.y * up.z - right.z * up.y, right.z * up.x - right.x * up.z, right.x * up.y - right.y * up.x};
    const ProbeVec d = sub(point, pelvis);
    out = {dot(d, forward), dot(d, right), dot(d, up)};
    return std::isfinite(out.forward) && std::isfinite(out.right) && std::isfinite(out.up);
}

// One health report's contacts.
class ContactTally {
public:
    // True when this contact gets a line of its own: the first of its place and (part, bone) this window, while lines
    // remain. The place is part of the row: an off-body contact is not hidden by a body contact on the same bone.
    bool Add(ContactPlace place, const std::string& key, bool measured, double offCm) {
        ++count_[size_t(place)];
        if (measured && offCm > maxOffCm_) maxOffCm_ = offCm;
        const RowKey rowKey{place, key};
        auto it = rows_.find(rowKey);
        const bool first = it == rows_.end();
        if (first) {
            if (rows_.size() >= kProbeTallyRows) { ++overflow_; return false; }
            it = rows_.emplace(rowKey, Row{}).first;
        }
        ++it->second.count;
        if (measured && offCm > it->second.maxOffCm) it->second.maxOffCm = offCm;
        if (!first || place == ContactPlace::Unread) return false;
        const bool body = place == ContactPlace::Body;
        int& lines = body ? bodyLines_ : lines_;
        if (lines >= (body ? kProbeBodyDetailsPerWindow : kProbeDetailsPerWindow)) return false;
        ++lines;
        return true;
    }
    void Dropped() { ++dropped_; }

    // The health line, and a fresh window; empty when nothing was seen.
    std::string DrainLine() {
        uint32_t total = dropped_;
        for (uint32_t c : count_) total += c;
        std::string line;
        if (total) {
            char buf[192];
            std::snprintf(buf, sizeof buf, "probe: contacts on stand-ins: body %u off-body %u part %u gear %u unread %u dropped %u, farthest %.1f cm",
                          count_[0], count_[1], count_[2], count_[3], count_[4], dropped_, maxOffCm_);
            line = buf;
            std::vector<std::pair<RowKey, Row>> top;
            for (const auto& [key, row] : rows_) if (key.first != ContactPlace::Body) top.emplace_back(key, row);
            std::stable_sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
            if (top.size() > kProbeTopRows) top.resize(kProbeTopRows);
            for (size_t i = 0; i < top.size(); ++i) {
                std::snprintf(buf, sizeof buf, " x%u %s, up to %.1f cm", top[i].second.count, ContactPlaceName(top[i].first.first), top[i].second.maxOffCm);
                line += (i ? "; " : "; top: ") + top[i].first.second + buf;
            }
            if (overflow_) line += "; " + std::to_string(overflow_) + " more on other parts";
        }
        *this = ContactTally();
        return line;
    }

private:
    using RowKey = std::pair<ContactPlace, std::string>;
    struct Row { uint32_t count = 0; double maxOffCm = 0; };
    std::map<RowKey, Row> rows_;
    uint32_t count_[5] = {};
    uint32_t dropped_ = 0, overflow_ = 0;
    int lines_ = 0, bodyLines_ = 0;
    double maxOffCm_ = 0;
};

}  // namespace hsm::combat
