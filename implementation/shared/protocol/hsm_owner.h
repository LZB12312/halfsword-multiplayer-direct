// Protocol revision 9: owner authority. Every machine simulates the fighters it owns (its own player; the host also
// its bots) and streams them; everyone else shows them as stand-ins (sync/hsm_standin.h). The host relays.
//   Pose     unreliable, 60 Hz per owned fighter: capsule root and yaw plus the HSMP pose codec (sync/hsm_pose_codec.h)
//   Vitals   unreliable, 15 Hz per owned fighter: health, parts, severed and dislocated parts, dead/fallen/KO
//   HitClaim reliable: an attacker's "Deal Complex Damage" on a stand-in, for the victim's owner to replay
//   HitFx    reliable: the same claim, forwarded by the host to everyone else, cosmetic only
#pragma once
#include "combat/hsm_claim.h"
#include "hsm_protocol.h"
#include "sync/hsm_pose_codec.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace hsm {

constexpr float kMaxRootCoord = 1.0e7f;   // cm; a level is far smaller, anything beyond is garbage
constexpr size_t kPoseMsgHeaderBytes = 4 + 2 + 4 * 4 + 2;   // message header, entity and epoch, root and yaw, payload length
constexpr size_t kPoseMsgMaxBytes = kPoseMsgHeaderBytes + sync::kPoseFrameMaxBytes;
constexpr size_t kUnfragmentedBytes = 1200;   // Steam splits a larger unreliable message, and one lost piece loses it
static_assert(kPoseMsgMaxBytes <= kUnfragmentedBytes, "a pose must travel as one datagram");

struct PoseMsg {
    uint8_t entity = 0;
    uint8_t epoch = 0;                        // rev 32: the sender's hand-off epoch for entity (sync/hsm_handoff.h)
    float rootX = 0, rootY = 0, rootZ = 0;   // the owner's capsule (actor) location, cm
    float yawDeg = 0;                         // the owner's actor yaw
    std::vector<uint8_t> pose;                // sync::EncodePose bytes
};

inline std::vector<uint8_t> Encode(const PoseMsg& m) {
    Writer w; w.buf.reserve(kPoseMsgHeaderBytes + m.pose.size()); w.header(MsgType::Pose);
    w.u8(m.entity); w.u8(m.epoch); w.f32(m.rootX); w.f32(m.rootY); w.f32(m.rootZ); w.f32(m.yawDeg);
    w.u16(uint16_t(m.pose.size()));                 // kPoseFrameMaxBytes fits (static_assert below)
    w.buf.insert(w.buf.end(), m.pose.begin(), m.pose.end());
    return std::move(w.buf);
}
// Cheap transport admission only: validates framing, identity range and finite root/yaw, without decoding the
// skeleton or copying payload bytes. The session MUST call DecodePoseFrame before applying or relaying a pose.
// m receives the envelope fields only; its owning payload is cleared, including when a previous packet used it.
inline bool DecodePoseEnvelope(const uint8_t* d, size_t n, PoseMsg& m) {
    m.pose.clear();
    if (!d || n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Pose) return false;
    m.entity = r.u8(); m.epoch = r.u8(); m.rootX = r.f32(); m.rootY = r.f32(); m.rootZ = r.f32(); m.yawDeg = r.f32();
    const size_t len = r.u16();
    if (!r.ok() || m.entity == 0 || m.entity > kMaxWillies) return false;
    if (std::fabs(m.rootX) > kMaxRootCoord || std::fabs(m.rootY) > kMaxRootCoord || std::fabs(m.rootZ) > kMaxRootCoord) return false;
    if (std::fabs(m.yawDeg) > 720.f) return false;
    if (len == 0 || len > sync::kPoseFrameMaxBytes || n != kPoseMsgHeaderBytes + len) return false;   // payload = the rest
    return true;
}

// Complete validation and decoding directly from the received bytes, with no payload allocation. The same path
// is used for host relays, direct mesh traffic and detour payloads; callers still check sender/lease/epoch.
inline bool DecodePoseFrame(const uint8_t* d, size_t n, PoseMsg& m, sync::PoseFrame& frame) {
    return DecodePoseEnvelope(d, n, m) && sync::DecodePose(d + kPoseMsgHeaderBytes, n - kPoseMsgHeaderBytes, frame);
}

// Owning protocol API: retain the exact payload for callers which need to re-encode it. Never copy invalid data.
inline bool Decode(const uint8_t* d, size_t n, PoseMsg& m, sync::PoseFrame* frame = nullptr) {
    sync::PoseFrame scratch;
    if (!DecodePoseFrame(d, n, m, frame ? *frame : scratch)) return false;
    m.pose.assign(d + kPoseMsgHeaderBytes, d + n);
    return true;
}

static_assert(sync::kPoseFrameMaxBytes <= 0xFFFF, "pose payload length is a u16");

struct VitalsMsg {
    uint8_t entity = 0;
    uint8_t epoch = 0;                      // rev 32: the sender's hand-off epoch for entity (sync/hsm_handoff.h)
    uint8_t flags = 0;                      // bit0 dead, bit1 fallen, bit2 knocked out
    float health = 0, consciousness = 0, stamina = 0, bleeding = 0;
    std::array<float, 7> partHealth{};      // head, upper body, lower body, arm R, arm L, leg R, leg L
    uint32_t severed = 0;                   // bit per Enum_DismembermentPart value
    uint32_t dislocated = 0;                // bit per dislocatable joint (phase 4)
    // Rev 30: the fields other fighters' code reads on this fighter's stand-in (sync audit items 1, 9, 23, 30): the
    // attacker's parry roll, bots' target choice, sight and hearing. kVitalsState* bits, then Willie_BP values.
    uint8_t state = 0;
    float weaponSkill = 0, tonus = 0;       // Weapon Skill (Temp) 0..1, All Body Tonus 0..100
    float noise = 0, light = 0;             // Total Noise, In Light Meter
    float moveDislocation = 0;              // |Movement Dislocation|
    float exhaustion = 0;
};
constexpr uint8_t kVitalsFlagsMask = 0x07;
constexpr uint8_t kVitalsStateDowned = 1, kVitalsStateGuarding = 2, kVitalsStateGuardR = 4, kVitalsStateGuardL = 8,
                  kVitalsStateCrouched = 16, kVitalsStateThrustR = 32, kVitalsStateThreatening = 64, kVitalsStateGiveUp = 128;

inline std::vector<uint8_t> Encode(const VitalsMsg& m) {
    Writer w; w.header(MsgType::Vitals);
    w.u8(m.entity); w.u8(m.epoch); w.u8(m.flags); w.f32(m.health); w.f32(m.consciousness); w.f32(m.stamina); w.f32(m.bleeding);
    for (float h : m.partHealth) w.f32(h);
    w.u32(m.severed); w.u32(m.dislocated);
    w.u8(m.state); w.f32(m.weaponSkill); w.f32(m.tonus); w.f32(m.noise); w.f32(m.light); w.f32(m.moveDislocation); w.f32(m.exhaustion);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, VitalsMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Vitals) return false;
    m.entity = r.u8(); m.epoch = r.u8(); m.flags = r.u8(); m.health = r.f32(); m.consciousness = r.f32(); m.stamina = r.f32(); m.bleeding = r.f32();
    for (float& h : m.partHealth) h = r.f32();
    m.severed = r.u32(); m.dislocated = r.u32();
    m.state = r.u8(); m.weaponSkill = r.f32(); m.tonus = r.f32(); m.noise = r.f32(); m.light = r.f32(); m.moveDislocation = r.f32(); m.exhaustion = r.f32();
    return r.done() && m.entity >= 1 && m.entity <= kMaxWillies && (m.flags & ~kVitalsFlagsMask) == 0;
}

// HitClaim and HitFx carry the same claim; the type says what the receiver may do with it.
struct HitClaimMsg { combat::HitClaim claim; };
struct HitFxMsg { combat::HitClaim claim; };

namespace detail {
inline std::vector<uint8_t> EncodeClaim(MsgType type, const combat::HitClaim& c) {
    Writer w; w.header(type); combat::PutClaim(w, c); return w.buf;
}
inline bool DecodeClaim(const uint8_t* d, size_t n, MsgType type, combat::HitClaim& c) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != type) return false;
    if (!combat::GetClaim(r, c)) return false;
    return c.attacker >= 1 && c.attacker <= kMaxWillies && c.victim >= 1 && c.victim <= kMaxWillies && c.attacker != c.victim;
}
}  // namespace detail

inline std::vector<uint8_t> Encode(const HitClaimMsg& m) { return detail::EncodeClaim(MsgType::HitClaim, m.claim); }
inline bool Decode(const uint8_t* d, size_t n, HitClaimMsg& m) { return detail::DecodeClaim(d, n, MsgType::HitClaim, m.claim); }
inline std::vector<uint8_t> Encode(const HitFxMsg& m) { return detail::EncodeClaim(MsgType::HitFx, m.claim); }
inline bool Decode(const uint8_t* d, size_t n, HitFxMsg& m) { return detail::DecodeClaim(d, n, MsgType::HitFx, m.claim); }

}  // namespace hsm
