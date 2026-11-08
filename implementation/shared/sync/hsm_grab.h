// Protocol revision 9: grabs across machines. A grab is native on the grabber's machine (Willie_BP ReceiveTick binds
// the held body to its hand with "Grab R/L Constraint"), so a stand-in held that way moves only on the grabber's
// screen. The grabber's owner therefore streams what each hand holds (whose fighter, which bone, and the grip: its
// hand in that bone's frame), and the held fighter's owner pulls that bone toward the grabber's hand as its stand-in
// shows it, through a soft, force-limited handle: the victim can struggle, and past kGrabBreakUu for kGrabBreakMs
// breaks free. A hold ends when the grabber lets go, its stream is silent for kGrabLeaseMs, or the grabber dies.
//   GrabState  grabber's owner -> host -> others: reliable on a change, unreliable on the kGrabRefreshMs refresh
//
// Design and constants from Cyrex0/HalfSword-MP mods/HSMPInteract/Scripts/main.lua (cfg, drive_held), Copyright (c)
// 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md). HSMP moves a PhysicsHandle's
// target to grip + clamp(hand - grip, lead); here the same spring-damper is a per-step velocity change on the bone
// (no component spawned), integrated implicitly like the handle's acceleration drive.
#pragma once
#include "hsm_protocol.h"
#include "sync/hsm_pose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hsm {

constexpr size_t kGrabHands = 2;        // 0 right (hand_r), 1 left (hand_l)
constexpr size_t kMaxGrabBone = 64;     // bone names are short ("lowerarm_l"); longer is garbage
constexpr double kMaxGripUu = 60.0;     // cm from the bone's origin: no hand holds farther out (HSMP MAX_POINT)

struct GrabHand {
    bool held = false;
    uint8_t target = 0;                 // the held fighter
    uint32_t grabId = 0;                // nonzero, the grabber's; a new one per new hold
    std::string bone;                   // the held body
    sync::V3 grip;                      // the grabber's hand in that bone's frame, cm
};

struct GrabStateMsg {
    uint8_t grabber = 0;
    uint32_t seq = 0;                   // +1 per message from this grabber; receivers keep the newest
    std::array<GrabHand, kGrabHands> hands{};
};

namespace grabdetail {
using sync::V3;
inline V3 Add(const V3& a, const V3& b) { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(const V3& a, const V3& b) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Scale(const V3& a, double s) { return V3{ a.x * s, a.y * s, a.z * s }; }
inline double Len(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline bool Finite(const V3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline V3 ClampLen(const V3& a, double lim) { const double l = Len(a); return l > lim ? Scale(a, lim / l) : a; }
}  // namespace grabdetail

// A bone name: letters, digits and underscores ("lowerarm_l", "spine_03"), at most kMaxGrabBone. The receiver interns it
// as an FName, so nothing else may travel.
inline bool ValidBoneName(const std::string& s) {
    if (s.empty() || s.size() > kMaxGrabBone) return false;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}


// A hold that may travel: a fighter other than the grabber, a grab id, a named bone, a finite grip within reach.
inline bool ValidHold(uint8_t grabber, const GrabHand& h) {
    return h.held && h.target != 0 && h.target <= kMaxWillies && h.target != grabber && h.grabId != 0
        && ValidBoneName(h.bone) && grabdetail::Finite(h.grip)
        && grabdetail::Len(h.grip) <= kMaxGripUu;
}

// Per hand: u8 held, and if held: u8 target, u32 grab id, bone, f32 grip x y z.
inline std::vector<uint8_t> Encode(const GrabStateMsg& m) {
    Writer w; w.header(MsgType::GrabState);
    w.u8(m.grabber); w.u32(m.seq);
    for (const GrabHand& h : m.hands) {
        w.u8(h.held ? 1 : 0);
        if (!h.held) continue;
        w.u8(h.target); w.u32(h.grabId); w.str(h.bone, kMaxGrabBone);
        w.f32(float(h.grip.x)); w.f32(float(h.grip.y)); w.f32(float(h.grip.z));
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, GrabStateMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::GrabState) return false;
    m.grabber = r.u8(); m.seq = r.u32();
    if (!r.ok() || m.grabber == 0 || m.grabber > kMaxWillies) return false;
    for (GrabHand& h : m.hands) {
        h = GrabHand{};
        const uint8_t held = r.u8();
        if (!r.ok() || held > 1) return false;
        if (!held) continue;
        h.held = true; h.target = r.u8(); h.grabId = r.u32(); h.bone = r.str(kMaxGrabBone);
        h.grip.x = r.f32(); h.grip.y = r.f32(); h.grip.z = r.f32();
        if (!r.ok() || !ValidHold(m.grabber, h)) return false;
    }
    return r.done();
}

}  // namespace hsm

namespace hsm::sync {

constexpr double kGrabRefreshMs = 100.0;   // a hold is resent this often: the lease refresh (HSMP grab_update_ms)
constexpr double kGrabLeaseMs = 1500.0;    // nothing newer heard for this long: released (grab_lease_ms)
constexpr double kGripMaxUu = 55.0;        // the sender clamps the grip to this, inside kMaxGripUu (grip_max)
// 0.7.3: 80 uu for 300 ms (was HSMP's 130 / 400): a fighter on its feet that pulls away gets free, as from a stock grab
// whose hand cannot hold the weight (Willie_BP.cpp:16797, Grab R Release Rate).
constexpr double kGrabBreakUu = 80.0;      // the grip farther than this from the hand ... (grab_break)
constexpr double kGrabBreakMs = 300.0;     // ... for this long: broke free until a new grab (grab_break_ms)
// A body that cannot struggle (dead, fallen, knocked out) breaks free only on a glitch: farther and longer than that.
constexpr double kGrabDownBreakUu = 250.0, kGrabDownBreakMs = 1000.0;
// The grabber's stand-in shows its hand a playout delay late; the pull aims this far ahead along the hand's velocity.
constexpr double kGrabAnchorLeadS = 0.08, kGrabAnchorLeadCapUu = 40.0;

// The same holds (the same grabs of the same bones); grips and seq may differ.
inline bool SameHolds(const GrabStateMsg& a, const GrabStateMsg& b) {
    if (a.grabber != b.grabber) return false;
    for (size_t i = 0; i < kGrabHands; ++i) {
        const GrabHand& x = a.hands[i]; const GrabHand& y = b.hands[i];
        if (x.held != y.held || x.target != y.target || x.grabId != y.grabId || x.bone != y.bone) return false;
    }
    return true;
}

enum class GrabSend { None, Change, Refresh };   // Change goes reliable, Refresh unreliable

// The grabber's side. Update every tick with what the owned fighter's hands hold (grab ids are assigned here: a new
// one when a hand takes another fighter or bone); it fills `out` and says when to send it. An invalid hold counts as
// a free hand, and nothing is sent while nothing is or was held. Reset when the owned fighter is replaced.
class GrabSender {
public:
    GrabSend Update(uint8_t grabber, const std::array<GrabHand, kGrabHands>& reads, double nowMs, GrabStateMsg& out) {
        GrabStateMsg next; next.grabber = grabber;
        bool any = false;
        for (size_t i = 0; i < kGrabHands; ++i) {
            GrabHand h = reads[i];
            const GrabHand& was = last_.hands[i];
            const bool same = last_.grabber == grabber && was.held && was.target == h.target && was.bone == h.bone;
            h.grabId = same ? was.grabId : (nextId_ + 1 == 0 ? 1 : nextId_ + 1);
            if (grabdetail::Finite(h.grip)) h.grip = grabdetail::ClampLen(h.grip, kGripMaxUu);
            if (!ValidHold(grabber, h)) continue;
            if (!same) nextId_ = h.grabId;
            next.hands[i] = h; any = true;
        }
        bool wasAny = false;
        for (const GrabHand& h : last_.hands) wasAny = wasAny || h.held;
        GrabSend send = GrabSend::None;
        if (!SameHolds(next, last_) && (any || wasAny)) send = GrabSend::Change;
        else if (any && nowMs - sentMs_ >= kGrabRefreshMs) send = GrabSend::Refresh;
        if (send == GrabSend::None) { if (!any && !wasAny) last_.grabber = grabber; return send; }
        next.seq = ++seq_;
        last_ = next; sentMs_ = nowMs; out = next;
        return send;
    }
    void Reset() { last_ = GrabStateMsg{}; }   // seq and grab ids keep counting, so receivers take what follows

private:
    GrabStateMsg last_;                        // the last sent
    double sentMs_ = 0;
    uint32_t seq_ = 0, nextId_ = 0;
};

struct GrabPull {                              // one hold on a fighter of mine, to pull
    uint8_t grabber = 0, hand = 0, target = 0;
    uint32_t grabId = 0;
    std::string bone;
    V3 grip;                                   // in the bone's frame, cm
};

// The held side: the newest state of every grabber, and which holds broke free.
class GrabInbox {
public:
    // True if m is its grabber's newest state. An older one is never taken back, however long the stream was silent:
    // silence is the normal state after a let-go, so a delayed refresh must not revive the hold. A grabber whose owner
    // restarted its counter has left first, and Forget lets its stream start afresh.
    bool Accept(const GrabStateMsg& m, double nowMs) {
        if (m.grabber == 0 || m.grabber > kMaxWillies) return false;
        Slot& s = slots_[m.grabber - 1];
        if (s.heard && !SeqNewer(m.seq, s.state.seq)) return false;
        s.state = m; s.heard = true; s.heardMs = nowMs;
        for (const GrabHand& h : m.hands) if (h.held) s.topId = std::max(s.topId, h.grabId);
        return true;
    }
    // The holds to pull now: fresh, valid, on a fighter of mine, by a living grabber, not broken free.
    std::vector<GrabPull> Pulls(double nowMs, const std::function<bool(uint8_t)>& mine,
                                const std::function<bool(uint8_t)>& grabberDead) const {
        std::vector<GrabPull> out;
        for (const Slot& s : slots_) {
            if (!s.heard || nowMs - s.heardMs > kGrabLeaseMs || grabberDead(s.state.grabber)) continue;
            for (size_t i = 0; i < kGrabHands; ++i) {
                const GrabHand& h = s.state.hands[i];
                if (!ValidHold(s.state.grabber, h) || !mine(h.target)) continue;
                if (s.brk[i].broken && s.brk[i].grabId == h.grabId) continue;
                if (h.grabId <= s.retiredUpTo && nowMs < s.retiredUntilMs) continue;
                out.push_back(GrabPull{ s.state.grabber, uint8_t(i), h.target, h.grabId, h.bone, h.grip });
            }
        }
        return out;
    }
    // This tick's grip-to-hand distance of a pulled hold; true once it broke free (it stays broken for that grab): farther
    // than breakUu for breakMs (kGrabDownBreakUu/Ms for a body that cannot struggle).
    bool NoteDistance(const GrabPull& p, double distUu, double nowMs, double breakUu = kGrabBreakUu, double breakMs = kGrabBreakMs) {
        if (p.grabber == 0 || p.grabber > kMaxWillies || p.hand >= kGrabHands) return false;
        Break& b = slots_[p.grabber - 1].brk[p.hand];
        if (b.grabId != p.grabId) b = Break{ p.grabId };
        if (b.broken) return true;
        if (distUu > breakUu) {
            if (!b.far) { b.far = true; b.farSinceMs = nowMs; }
            b.broken = nowMs - b.farSinceMs >= breakMs;
        } else {
            b.far = false;
        }
        return b.broken;
    }
    // A fighter of mine was replaced (it respawned, or appeared): every hold on it so far was on an earlier body, and the
    // grabber's machine only learns of that a round trip later. They count as broken free, so the refreshes of those
    // grabs do not bring them back; a new grab pulls the new body.
    void DropTarget(uint8_t target) {
        for (Slot& s : slots_)
            for (size_t i = 0; i < kGrabHands; ++i)
                if (s.state.hands[i].held && s.state.hands[i].target == target) s.brk[i] = Break{ s.state.hands[i].grabId, false, true, 0 };
    }
    // The grabber left, restarted or respawned. Its stream starts afresh (a restarted counter is taken), but every grab it
    // made so far -- any id up to the highest heard, held now or let go of -- is retired for kGrabLeaseMs: refreshes its
    // old body sent before its machine saw the respawn are still on the way, and a reordered one may predate its let-go.
    // A new grab (a higher id: the sender keeps counting across a respawn) pulls at once; a restarted machine reusing
    // low ids waits out the retirement at most, and a rejoin takes longer than that anyway. A retirement still running
    // is carried over: a second Forget within it keeps the old grabs retired and adds the ones heard since.
    void Forget(uint8_t grabber, double nowMs) {
        if (grabber == 0 || grabber > kMaxWillies) return;
        Slot& s = slots_[grabber - 1];
        Slot fresh;
        fresh.retiredUpTo = nowMs < s.retiredUntilMs ? std::max(s.topId, s.retiredUpTo) : s.topId;
        fresh.retiredUntilMs = nowMs + kGrabLeaseMs;
        s = fresh;
    }
    // Every hold is dropped, but what was retired stays retired and the highest ids heard are remembered: a refresh of a
    // grab made before still on its way (or a stream replayed after a pause) must not bring it back.
    void Clear() {
        for (Slot& s : slots_) {
            Slot fresh;
            fresh.topId = s.topId;
            fresh.retiredUpTo = s.retiredUpTo;
            fresh.retiredUntilMs = s.retiredUntilMs;
            s = fresh;
        }
    }

private:
    struct Break { uint32_t grabId = 0; bool far = false, broken = false; double farSinceMs = 0; };
    struct Slot {
        bool heard = false; double heardMs = 0; GrabStateMsg state; std::array<Break, kGrabHands> brk{};
        uint32_t topId = 0;                                        // the highest grab id heard since the last Forget
        uint32_t retiredUpTo = 0; double retiredUntilMs = 0;       // grabs up to this id are dead until then
    };
    std::array<Slot, kMaxWillies> slots_{};
};

// A fighter on its feet. 0.7.3: HSMP's handle (1500, 150, 40, 800) pulled up to 60 000 cm/s^2 (about 60 g) on one bone,
// whatever the body weighed, so a held player could not pull away (reports: grappling "unreasonably overpowering").
// Now at most 600 x 30 = 18 000 cm/s^2 and 300 cm/s a step: the hold still leads the body, the body can still resist.
struct GrabPullParams {
    double stiffness = 600.0;    // 1/s^2: a soft handle the victim can struggle against
    double damping = 75.0;       // 1/s; zeta = 75 / (2 sqrt 600) ~ 1.5, overdamped: never past the hand
    double leadUu = 30.0;        // the spring's stretch is capped here: pull <= stiffness x lead (grab_lead)
    double maxDvUu = 300.0;      // cm/s per 60 Hz step at most (kGrabCapRefDtS): a glitch never launches a body
    double minDtS = 0.004, maxDtS = 0.080;   // the stand-in servo's frame-time clamp
    // The held bone is never driven upward faster than this (cm/s); negative: no cap. 0.8.0-pre7 (#83, #84): a carried
    // body followed the grabber's hand straight up through a jump, and the gravity-free stand-in of that body, seen a
    // round trip later on the grabber's screen, held the hand up in turn: the two machines lifted each other.
    double maxUpUu = -1.0;
};

// A held body that cannot struggle (dead, fallen, knocked out): carried the way the game carries a held NPC, not
// dragged on a soft spring (test 2026-10-25: grabbing another player's body "worked" but did not feel like the game's).
// Stiffer, a longer lead, still overdamped (zeta = 200 / (2 sqrt 4500) ~ 1.5), and a higher per-step cap so a body keeps
// up with a walking grabber (unchanged in 0.7.3: carrying a body was never the problem).
// A carried body is lifted no faster than this (cm/s): dragged along the ground at any speed, hoisted at a slow lift,
// never flown up with a jumping grabber. And the pull aims at the hand itself, not ahead of it (GrabAnchor lead 0): the
// lead was longer than the round trip, so the body arrived ahead of the hand and pushed it on.
constexpr double kGrabDownMaxUpUu = 120.0;
inline GrabPullParams DownPullParams() {
    GrabPullParams p;
    p.stiffness = 4500.0; p.damping = 200.0; p.leadUu = 80.0; p.maxDvUu = 1500.0;
    p.maxUpUu = kGrabDownMaxUpUu;
    return p;
}
// Share of a down body's pull also given to its pelvis and chest, so the whole body comes along, not just the held limb.
constexpr double kGrabDownSpread = 0.5;

// Where the pull aims: the grabber's stand-in hand, advanced along its velocity by leadS (at most capUu), since the
// stand-in shows the grabber a playout delay late. Non-finite velocity: the hand itself.
inline V3 GrabAnchor(const V3& hand, const V3& handVel, double leadS = kGrabAnchorLeadS, double capUu = kGrabAnchorLeadCapUu) {
    using namespace grabdetail;
    if (!Finite(hand) || !Finite(handVel) || !std::isfinite(leadS)) return hand;
    return Add(hand, ClampLen(Scale(handVel, std::max(leadS, 0.0)), capUu));
}

// The pose bone index of a held bone name, or -1. Any case, as FNames compare: the game's head bone is "Head", and up to
// 0.8.0-pre16 a head grab matched no pose bone, so the grabber's screen left nothing to the hold.
inline int PoseBoneIndex(const std::string& name) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    for (int i = 0; i < kPoseBones; ++i) {
        const char* n = kPoseBoneNames[size_t(i)];
        size_t k = 0;
        while (k < name.size() && n[k] && lower(name[k]) == n[k]) ++k;
        if (k == name.size() && !n[k]) return i;
    }
    return -1;
}
// The pose bones (bit per index) the held fighter's stand-in leaves to the grabber's native hold on the grabber's screen,
// so the servo and the grab constraint do not pull the bone two ways: the held bone and its parent, or, for a body that
// cannot struggle, the held limb up to the torso (the owner's own pull brings the rest after the hand). Never the whole
// body: a stand-in's bodies have no gravity and ignore the floor, so an undriven torso would float.
inline uint32_t YieldBones(int bone, bool down) {
    if (bone < 0 || bone >= kPoseBones) return 0;
    if (!down) return (1u << bone) | (1u << kPoseParent[size_t(bone)]);
    uint32_t m = 0;
    for (int b = bone; b != kBonePelvis && !(b >= 2 && b <= 5); b = kPoseParent[size_t(b)]) m |= 1u << b;   // up to pelvis / spine
    return m ? m : (1u << bone);
}

// grab_drag (the default from 0.8.0-pre17): every hold, standing or down, yields the way a carried body's foot does (the
// grab that dragged well in 0.8.0-pre7). A limb, the neck or the head: the chain up to the torso, which swings on one
// joint the way a dragged leg does. A torso bone: itself and its torso neighbours, so no lone body sits between two
// servoed ones, pulled three ways. Still never the whole body.
inline uint32_t DragYieldBones(int bone) {
    if (bone < 0 || bone >= kPoseBones) return 0;
    if (bone == kBonePelvis) return 1u | (1u << 2);   // and spine_02 (spine_01 has no body)
    if (bone <= 5) {
        uint32_t m = 1u << bone;
        for (int b = std::max(bone - 1, 2); b <= std::min(bone + 1, 5); ++b) m |= 1u << b;
        return m;
    }
    return YieldBones(bone, true);
}

// The share of the held bone's pull also given to the pelvis and chest. A carried body: kGrabDownSpread. A standing one
// under grab_drag: a smaller share, so the body comes along and the held bone is not pulled out of its joints alone,
// while the fighter can still struggle free. grab_drag=0: none, as up to 0.8.0-pre16.
constexpr double kGrabStandSpread = 0.25;
// Who's carried rather than held on a soft spring: a body that can't struggle, and anyone held by a Grappler, which is the perk: the pull is a
// change in velocity, so it doesn't care what the body weighs.
inline bool GrabCarried(bool down, bool grabberIsGrappler) { return down || grabberIsGrappler; }
inline double GrabSpread(bool down, bool drag) { return down ? kGrabDownSpread : drag ? kGrabStandSpread : 0.0; }

// A stand-in my fighter holds is not snapped onto its owner's pose (grab_drag) unless its pelvis was this far off at the
// last step: a dragged body lags its owner's pose by a round trip, past the servo's 150 uu snap error, and the snap
// teleported it under the hand, yanking the hold and both fighters. A blade stuck in a stand-in has the same rule
// (owner_sync.cpp kStuckNoShiftUu).
constexpr double kGrabNoSnapUu = 300.0;
inline bool GrabBlocksSnap(uint32_t heldMask, double lastPelvisErrUu) { return heldMask != 0 && lastPelvisErrUu < kGrabNoSnapUu; }

// maxDvUu is a cap per 60 Hz step. A slower machine takes longer steps, so its cap grows with the step (up to
// kGrabCapMaxSteps of them): the same cm/s per second whatever the grabber's frame rate. At 20 fps a held body
// otherwise got a third of the pull a 60 fps grabber gave it (0.8.0-pre19 reports #294, #295, #301).
constexpr double kGrabCapRefDtS = 1.0 / 60.0;
constexpr double kGrabCapMaxSteps = 4.0;
inline double GrabCapScale(double dtS) { return std::isfinite(dtS) ? std::clamp(dtS / kGrabCapRefDtS, 1.0, kGrabCapMaxSteps) : 1.0; }

// The velocity change for the held bone this step (SetPhysicsLinearVelocity, added): backward Euler of the handle's
// spring-damper toward the grabber's hand, the hand advanced by its own velocity over the step and the damping
// relative to it, so a bone moving with the hand needs nothing and a moving hand is followed without lag. Stable at
// any step. Non-finite input: no change.
inline V3 GrabPullDv(const V3& grip, const V3& gripVel, const V3& hand, const V3& handVel, double dtS,
                     const GrabPullParams& p = {}) {
    using namespace grabdetail;
    if (!Finite(grip) || !Finite(gripVel) || !Finite(hand) || !Finite(handVel) || !std::isfinite(dtS)) return {};
    const double dt = std::clamp(dtS, p.minDtS, p.maxDtS);
    const V3 stretch = ClampLen(Sub(Add(hand, Scale(handVel, dt)), grip), p.leadUu);
    const V3 drive = Add(Scale(stretch, p.stiffness), Scale(handVel, p.damping));
    V3 v = Scale(Add(gripVel, Scale(drive, dt)), 1.0 / (1.0 + p.damping * dt + p.stiffness * dt * dt));
    if (p.maxUpUu >= 0.0 && v.z > p.maxUpUu) v.z = p.maxUpUu;   // the bone's velocity after this step, not the change
    const V3 dv = ClampLen(Sub(v, gripVel), p.maxDvUu * GrabCapScale(dt));
    return Finite(dv) ? dv : V3{};
}

}  // namespace hsm::sync
