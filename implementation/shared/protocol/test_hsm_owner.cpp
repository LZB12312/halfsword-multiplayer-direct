// Protocol rev 9 owner-authority messages: Pose (root + yaw + the pose codec payload), Vitals, HitClaim, HitFx.
// Each round-trips exactly (the pose payload byte for byte, and to the same decoded frame as DecodePose); every
// malformed form is rejected: entity out of range, non-finite or absurd root/yaw, a payload that is empty, too long,
// not exactly the rest of the message or not a valid pose, unknown flag bits, trailing or missing bytes, a claim
// whose attacker is its victim, and one message type decoded as another.
#include "hsm_owner.h"
#include "session/hsm_lease.h"
#include <cmath>
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using hsm::sync::PoseFrame;
using hsm::sync::V3;
using hsm::sync::Q4;

static PoseFrame SomeFrame() {
    PoseFrame f;
    f.tsMs = 123456;
    f.stepUs = 16667;
    for (int b = 0; b < hsm::sync::kPoseBones; ++b) {
        f.bones[size_t(b)].pos = V3{ 100.0 + b * 3.0, -50.0 + b, 90.0 + b * 2.0 };
        const double a = 0.05 * b;
        f.bones[size_t(b)].rot = Q4{ 0, std::sin(a), 0, std::cos(a) };
        f.bones[size_t(b)].vel = V3{ 10.0 * b, -5.0, 2.0 };
        f.bones[size_t(b)].angVelDeg = V3{ 0, 30.0, -b * 1.0 };
    }
    f.weapons[0].present = true;
    f.weapons[0].hand = 0;
    f.weapons[0].classHash = 0xABCDEF01u;
    f.weapons[0].pos = V3{ 130, -30, 120 };
    f.weapons[0].rot = Q4{ 0, 0, std::sin(0.3), std::cos(0.3) };
    f.weapons[0].vel = V3{ 300, 0, -40 };
    f.weapons[0].angVelDeg = V3{ 0, 0, 200 };
    f.weapons[0].base = V3{ 135, -30, 125 };
    f.weapons[0].tip = V3{ 200, -30, 160 };
    return f;
}

static PoseMsg SomePose() {
    PoseMsg m;
    m.entity = 3;
    m.rootX = 101.5f; m.rootY = -48.25f; m.rootZ = 92.f; m.yawDeg = -135.f;
    m.pose = sync::EncodePose(SomeFrame());
    return m;
}

static bool SameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { return a == b; }

static void TestPoseRoundTrip() {
    const PoseMsg m = SomePose();
    const std::vector<uint8_t> bytes = Encode(m);
    CHECK(bytes.size() == kPoseMsgHeaderBytes + m.pose.size());
    PoseMsg out;
    PoseFrame frame, direct;
    CHECK(Decode(bytes.data(), bytes.size(), out, &frame));
    CHECK(out.entity == 3 && out.rootX == 101.5f && out.rootY == -48.25f && out.rootZ == 92.f && out.yawDeg == -135.f);
    CHECK(SameBytes(out.pose, m.pose));
    CHECK(sync::DecodePose(m.pose.data(), m.pose.size(), direct));
    CHECK(frame.tsMs == direct.tsMs && frame.stepUs == direct.stepUs);
    for (int b = 0; b < hsm::sync::kPoseBones; ++b)
        CHECK(frame.bones[size_t(b)].pos.x == direct.bones[size_t(b)].pos.x && frame.bones[size_t(b)].vel.z == direct.bones[size_t(b)].vel.z);
    CHECK(frame.weapons[0].present && frame.weapons[0].classHash == 0xABCDEF01u && !frame.weapons[1].present);
    PoseMsg noFrame;
    CHECK(Decode(bytes.data(), bytes.size(), noFrame));   // the frame is optional
    // Even the largest pose (every bone overridden, both weapons) is one unfragmented datagram: Steam splits an
    // unreliable message above ~1200 B, and losing any piece loses the pose. (The plan's 400 B estimate was wrong
    // for the HSMP codec: a full frame with one weapon is ~440 B.)
    std::printf("pose message: %zu bytes (payload %zu, max message %zu)\n", bytes.size(), m.pose.size(), kPoseMsgMaxBytes);
    CHECK(kPoseMsgMaxBytes == kPoseMsgHeaderBytes + sync::kPoseFrameMaxBytes);
    CHECK(bytes.size() <= kPoseMsgMaxBytes && kPoseMsgMaxBytes <= kUnfragmentedBytes && kUnfragmentedBytes == 1200);
}

static void TestPoseRejects() {
    auto bad = [](PoseMsg m) { const auto b = Encode(m); PoseMsg o; return !Decode(b.data(), b.size(), o); };
    PoseMsg m = SomePose();
    m.entity = 0; CHECK(bad(m));
    m = SomePose(); m.entity = uint8_t(kMaxWillies + 1); CHECK(bad(m));
    m = SomePose(); m.entity = uint8_t(kMaxWillies); CHECK(!bad(m));
    m = SomePose(); m.rootX = 1.0e7f; CHECK(!bad(m));                     // the bound itself is allowed
    m = SomePose(); m.rootX = 1.1e7f; CHECK(bad(m));
    m = SomePose(); m.rootY = -1.1e7f; CHECK(bad(m));
    m = SomePose(); m.rootZ = 1.1e7f; CHECK(bad(m));
    m = SomePose(); m.rootZ = std::numeric_limits<float>::quiet_NaN(); CHECK(bad(m));
    m = SomePose(); m.yawDeg = std::numeric_limits<float>::infinity(); CHECK(bad(m));
    m = SomePose(); m.yawDeg = 720.f; CHECK(!bad(m));
    m = SomePose(); m.yawDeg = 721.f; CHECK(bad(m));
    m = SomePose(); m.yawDeg = -721.f; CHECK(bad(m));
    m = SomePose(); m.pose.clear(); CHECK(bad(m));
    m = SomePose(); m.pose.assign(sync::kPoseFrameMaxBytes + 1, 0); CHECK(bad(m));
    m = SomePose(); m.pose[0] ^= 0xFF; CHECK(bad(m));                     // codec version byte
    m = SomePose(); m.pose.pop_back(); CHECK(bad(m));                     // a truncated pose is no pose

    const std::vector<uint8_t> good = Encode(SomePose());
    PoseMsg o;
    std::vector<uint8_t> b = good; b.push_back(0);                        // trailing byte: length no longer the rest
    CHECK(!Decode(b.data(), b.size(), o));
    b = good; b.pop_back();                                               // missing byte
    CHECK(!Decode(b.data(), b.size(), o));
    b = good; b[kPoseMsgHeaderBytes - 2] = uint8_t(b[kPoseMsgHeaderBytes - 2] + 1);   // length field one too big
    CHECK(!Decode(b.data(), b.size(), o));
    b = good; b[kPoseMsgHeaderBytes - 2] = uint8_t(b[kPoseMsgHeaderBytes - 2] - 1);   // one too small
    CHECK(!Decode(b.data(), b.size(), o));
    for (size_t n = 0; n < kPoseMsgHeaderBytes; ++n) CHECK(!Decode(good.data(), n, o));   // cut inside the header
    b = good; b[3] = uint8_t(MsgType::Vitals);                            // another type's header
    CHECK(!Decode(b.data(), b.size(), o));
    b = good; b[2] = uint8_t(kProtocolRevision - 1);                      // another revision
    CHECK(!Decode(b.data(), b.size(), o));
}

static void TestPoseAdmissionAndFrameDecode() {
    const PoseMsg original = SomePose();
    const auto good = Encode(original);
    PoseMsg envelope = original;   // reuse must never expose a previous packet's owning payload
    CHECK(DecodePoseEnvelope(good.data(), good.size(), envelope));
    CHECK(envelope.pose.empty() && envelope.entity == original.entity && envelope.epoch == original.epoch);
    CHECK(envelope.rootX == original.rootX && envelope.rootY == original.rootY && envelope.rootZ == original.rootZ);
    CHECK(envelope.yawDeg == original.yawDeg);
    PoseFrame actual, expected;
    CHECK(DecodePoseFrame(good.data(), good.size(), envelope, actual));
    CHECK(envelope.pose.empty());
    CHECK(sync::DecodePose(original.pose.data(), original.pose.size(), expected));
    CHECK(sync::EncodePose(actual) == sync::EncodePose(expected));

    // An intact outer envelope is not permission to use/relay its payload. Bad codec/version/physics data must
    // survive admission only to fail the full session decode; the owning Decode API must still reject it too.
    auto refusesPayload = [&](const std::vector<uint8_t>& b) {
        CHECK(DecodePoseEnvelope(b.data(), b.size(), envelope));
        CHECK(!DecodePoseFrame(b.data(), b.size(), envelope, actual));
        CHECK(envelope.pose.empty());
        PoseMsg owned;
        CHECK(!Decode(b.data(), b.size(), owned) && owned.pose.empty());
    };
    auto b = good; b[kPoseMsgHeaderBytes] ^= 0xFF; refusesPayload(b);   // wrong codec
    b = good; b[kPoseMsgHeaderBytes + 7] = 8; refusesPayload(b);        // flail head without its weapon
    b = good;                                                        // non-finite pelvis inside an otherwise valid envelope
    const uint32_t nan = 0x7FC00000;
    for (size_t i = 0; i < 4; ++i) b[kPoseMsgHeaderBytes + 10 + i] = uint8_t(nan >> (8 * i));
    refusesPayload(b);
    PoseMsg shortPose = original; shortPose.pose.resize(1); refusesPayload(Encode(shortPose));

    // Every truncation, length mismatch, invalid root and invalid message identity fails the cheap gate too.
    for (size_t n = 0; n < good.size(); ++n) {
        CHECK(!DecodePoseEnvelope(good.data(), n, envelope));
        CHECK(!DecodePoseFrame(good.data(), n, envelope, actual));
    }
    b = good; b.push_back(0); CHECK(!DecodePoseEnvelope(b.data(), b.size(), envelope));
    b = good; b[4] = 0; CHECK(!DecodePoseEnvelope(b.data(), b.size(), envelope));
    b = good; b[3] = uint8_t(MsgType::Vitals); CHECK(!DecodePoseEnvelope(b.data(), b.size(), envelope));
    b = good; b[2] ^= 1; CHECK(!DecodePoseEnvelope(b.data(), b.size(), envelope));
    PoseMsg invalid = original; invalid.rootX = std::numeric_limits<float>::infinity();
    b = Encode(invalid); CHECK(!DecodePoseEnvelope(b.data(), b.size(), envelope));
    invalid = original; invalid.yawDeg = 721.f;
    b = Encode(invalid); CHECK(!DecodePoseEnvelope(b.data(), b.size(), envelope));
    CHECK(!DecodePoseFrame(nullptr, good.size(), envelope, actual));
    CHECK(!DecodePoseEnvelope(good.data(), kMaxPacket + 1, envelope));
}

static VitalsMsg SomeVitals() {
    VitalsMsg v;
    v.entity = 9; v.flags = 0x5; v.health = 77.5f; v.consciousness = 0.25f; v.stamina = 40.f; v.bleeding = 1.5f;
    for (size_t i = 0; i < v.partHealth.size(); ++i) v.partHealth[i] = float(10 * i) + 0.5f;
    v.severed = 0x81; v.dislocated = 0x3;
    v.state = kVitalsStateDowned | kVitalsStateGuarding | kVitalsStateGiveUp; v.weaponSkill = 0.75f; v.tonus = 62.f;
    v.noise = 0.4f; v.light = 0.9f; v.moveDislocation = 35.f; v.exhaustion = 12.f;
    return v;
}

static void TestVitals() {
    const VitalsMsg v = SomeVitals();
    const auto bytes = Encode(v);
    VitalsMsg o;
    CHECK(Decode(bytes.data(), bytes.size(), o));
    CHECK(o.entity == 9 && o.flags == 0x5 && o.health == 77.5f && o.consciousness == 0.25f && o.stamina == 40.f && o.bleeding == 1.5f);
    for (size_t i = 0; i < o.partHealth.size(); ++i) CHECK(o.partHealth[i] == v.partHealth[i]);
    CHECK(o.severed == 0x81 && o.dislocated == 0x3);
    CHECK(o.state == (kVitalsStateDowned | kVitalsStateGuarding | kVitalsStateGiveUp) && o.weaponSkill == 0.75f && o.tonus == 62.f);
    CHECK(o.noise == 0.4f && o.light == 0.9f && o.moveDislocation == 35.f && o.exhaustion == 12.f);
    auto bad = [](VitalsMsg m) { const auto b = Encode(m); VitalsMsg x; return !Decode(b.data(), b.size(), x); };
    VitalsMsg m = v; m.entity = 0; CHECK(bad(m));
    m = v; m.entity = uint8_t(kMaxWillies + 1); CHECK(bad(m));
    m = v; m.flags = 0x8; CHECK(bad(m));
    m = v; m.flags = 0x7; CHECK(!bad(m));
    m = v; m.health = std::numeric_limits<float>::quiet_NaN(); CHECK(bad(m));
    m = v; m.partHealth[6] = std::numeric_limits<float>::infinity(); CHECK(bad(m));
    m = v; m.tonus = std::numeric_limits<float>::quiet_NaN(); CHECK(bad(m));
    m = v; m.exhaustion = std::numeric_limits<float>::infinity(); CHECK(bad(m));
    std::vector<uint8_t> b = bytes; b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));
    b = bytes; b.pop_back(); CHECK(!Decode(b.data(), b.size(), o));
}

static combat::HitClaim SomeClaim() {
    combat::HitClaim c;
    c.claimId = 77; c.attacker = 2; c.victim = 1; c.attackerTsMs = 99999;
    c.hitBone = "head"; c.location = V3{ 1, 2, 3 }; c.normal = V3{ 0, 0, 1 }; c.hitVelocity = V3{ 500, 0, 0 };
    c.hitImpulse = V3{ 40, 0, 0 }; c.cuttingPower = 1.5; c.rigidity = 0.4; c.source = combat::StrikeSource::WeaponRight;
    c.strikeComponent = "Blade";
    return c;
}

static void TestClaims() {
    const combat::HitClaim c = SomeClaim();
    const auto claimBytes = Encode(HitClaimMsg{ c });
    const auto fxBytes = Encode(HitFxMsg{ c });
    HitClaimMsg hc;
    HitFxMsg fx;
    CHECK(Decode(claimBytes.data(), claimBytes.size(), hc));
    CHECK(hc.claim.claimId == 77 && hc.claim.attacker == 2 && hc.claim.victim == 1 && hc.claim.hitBone == "head");
    CHECK(hc.claim.cuttingPower == 1.5 && hc.claim.source == combat::StrikeSource::WeaponRight && hc.claim.strikeComponent == "Blade");
    CHECK(Decode(fxBytes.data(), fxBytes.size(), fx));
    CHECK(fx.claim.claimId == 77 && fx.claim.hitImpulse.x == 40);
    CHECK(!Decode(claimBytes.data(), claimBytes.size(), fx));             // a claim is not an effect, nor the reverse
    CHECK(!Decode(fxBytes.data(), fxBytes.size(), hc));
    auto bad = [](combat::HitClaim x) { const auto b = Encode(HitClaimMsg{ x }); HitClaimMsg o; return !Decode(b.data(), b.size(), o); };
    combat::HitClaim m = c; m.victim = m.attacker; CHECK(bad(m));
    m = c; m.attacker = 0; CHECK(bad(m));
    m = c; m.victim = uint8_t(kMaxWillies + 1); CHECK(bad(m));
    m = c; m.attacker = uint8_t(kMaxWillies); CHECK(!bad(m));
    std::vector<uint8_t> b = claimBytes; b.push_back(0); CHECK(!Decode(b.data(), b.size(), hc));
    b = claimBytes; b.pop_back(); CHECK(!Decode(b.data(), b.size(), hc));
    // Test 5: a rock thrown at a stand-in is a claim struck by that prop, with its id and its velocity at the contact.
    combat::HitClaim rock = c;
    combat::Xform bone;
    bone.rot = sync::Q4{ 0, 0, 0, 1 };
    combat::StruckByProp(rock, MakeLevelPropId(14), V3{ 300, 0, -50 }, bone);
    const auto rockBytes = Encode(HitClaimMsg{ rock });
    CHECK(Decode(rockBytes.data(), rockBytes.size(), hc));
    CHECK(hc.claim.striker == combat::StrikerKind::Prop && hc.claim.propId == MakeLevelPropId(14));
    CHECK(hc.claim.propVelocity.x == 300 && hc.claim.propVelocity.z == -50 && hc.claim.source == combat::StrikeSource::Body);
    const auto rockFx = Encode(HitFxMsg{ rock });
    CHECK(Decode(rockFx.data(), rockFx.size(), fx) && fx.claim.propId == MakeLevelPropId(14));
    m = rock; m.source = combat::StrikeSource::WeaponRight; CHECK(bad(m));
    m = rock; m.propId = 0; CHECK(bad(m));
    // Rev 25: a lodged blade's damage (stabs) is a claim of striker kind Stuck, carrying Get Damage's Raw Damage.
    combat::HitClaim stab = c;
    stab.striker = combat::StrikerKind::Stuck; stab.rawDamage = 3500; stab.hitBone = "spine_03";
    const auto stabBytes = Encode(HitClaimMsg{ stab });
    CHECK(Decode(stabBytes.data(), stabBytes.size(), hc));
    CHECK(hc.claim.striker == combat::StrikerKind::Stuck && hc.claim.rawDamage == 3500 && hc.claim.hitBone == "spine_03");
    CHECK(hc.claim.source == combat::StrikeSource::WeaponRight && hc.claim.cuttingPower == 1.5);
    CHECK(!bad(stab));
    for (size_t n = 0; n < stabBytes.size(); ++n) CHECK(!Decode(stabBytes.data(), n, hc));   // every truncation fails
    m = stab; m.rawDamage = 0; CHECK(bad(m));                                   // nothing to apply
    m = stab; m.rawDamage = combat::kMaxStuckRawDamage * 2; CHECK(bad(m));     // out of range
    m = stab; m.source = combat::StrikeSource::Body; CHECK(bad(m));             // no blade
    m = c; m.rawDamage = 100; CHECK(bad(m));                                    // only a Stuck claim has a damage
}

int main() {
    TestPoseRoundTrip();
    TestPoseRejects();
    TestPoseAdmissionAndFrameDecode();
    TestVitals();
    TestClaims();
    std::printf(g_fail ? "%d FAILED\n" : "all owner protocol tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
