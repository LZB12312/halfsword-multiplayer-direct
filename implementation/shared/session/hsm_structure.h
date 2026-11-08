// Constraint-built level props, "structures" (0.8.0-pre18, #230 "bridge is falling over and over", #85). A level prop that is one
// actor with several physics bodies joined by physics constraints (a hanging bridge, a chain, a plank walkway) is a structure.
// Its joints are held by the game's own physics, and a hanging bridge never reads as at rest (hsm_lease.h PropAtRest), so up to
// 0.8.0-pre17 it was streamed like a loose prop at sixty states a second and the receiving copy moved every body by teleport,
// each from a different packet. A teleport ignores the joints, which took huge impulses, snapped, and the bridge fell on that
// machine only. sync_structures (HSMMod.ini [net]) picks what is done instead:
//
//   soft  (default) The host simulates the structure and streams all its bodies in one go, every kStructureStreamMs while any
//                   of them moves. A receiver never teleports a body of it: it steers each one toward the host's pose with
//                   velocity (sync/hsm_soft_correct.h) and places the whole structure at once only when it is badly off (a
//                   late-found prop that broke on one screen alone is placed from there on: FollowsWhenBroken). Nobody
//                   leases it (a machine's fighter pushes its own copy; the host's copy is pushed by that fighter's stand-in),
//                   and the map reset leaves it as it is.
//   local           Never streamed, never driven, never leased, never reset: every machine simulates its own copy and the copies
//                   start alike. Breaks still travel as ObjectEvents from whichever machine sees them.
//   full            As 0.8.0-pre17: nothing is a structure; it is streamed, teleported, leased and reset like any loose prop.
//
// A body that detaches is an object of its own (one body, no constraints: not a structure) and streams as any prop in every mode.
// The rule is by shape and does not change when a joint breaks; bodies still joined after a break stay a structure. Weapons are
// never structures. A spawned object is one only when it is a level actor the host's walk found after the census (foundLevelActor):
// breakable barrels and planks do not simulate yet when the census runs (the barrel respawns itself at 2.5 s, a plank turns its
// physics on after a start-up delay), so the next walk announces them under spawned ids. Until 0.8.0-pre29 those were teleported
// body by body on every state, which broke their joints on every client while the host's copy stood still. Game-independent.
//
// Bandwidth of the soft stream (BodyState is 62 bytes, one per body): bodies x 62 B x 20 Hz. A 10-body bridge sends 12.4 KB/s
// (about 100 kbit/s) to each client while it swings; at rest the same 620 B once a second. That is the price of "every machine
// sees the same bridge"; the old 60 Hz stream was three times it.
#pragma once
#include "session/hsm_lease.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace hsm {

enum class StructureMode : uint8_t { Soft = 0, Local = 1, Full = 2 };

// While any body of a soft structure moves, the host sends all its bodies this often (20 Hz); at rest every kFreeIdleMs.
constexpr double kStructureStreamMs = 50.0;

// The INI value. "soft", "local" and "full" by name (any case); the old on/off values keep their meaning of "not pre17" and
// "pre17": "0" is soft, "1" is full. Anything else (or nothing) is soft.
inline StructureMode ParseStructureMode(const std::string& v) {
    std::string s;
    for (char c : v) if (c != ' ' && c != '\t') s += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (s == "local") return StructureMode::Local;
    if (s == "full" || s == "1") return StructureMode::Full;
    return StructureMode::Soft;
}
inline const char* StructureModeName(StructureMode m) {
    return m == StructureMode::Local ? "local" : m == StructureMode::Full ? "full" : "soft";
}

// The shape that makes a structure: a level prop (or a level actor found after the census and announced under a spawned id)
// that is not a detached body, with two or more bodies and a constraint.
inline bool IsStructure(uint64_t propId, bool detachedBody, size_t bodies, size_t constraints, bool foundLevelActor = false) {
    return (IsLevelProp(propId) || foundLevelActor) && !IsWeaponId(propId) && !detachedBody && bodies >= 2 && constraints >= 1;
}

// local: this machine keeps its own copy and nothing about the prop is streamed, driven, claimed or reset.
inline bool StructureLocal(StructureMode mode, uint64_t propId, bool detachedBody, size_t bodies, size_t constraints,
                           bool foundLevelActor = false) {
    return mode == StructureMode::Local && IsStructure(propId, detachedBody, bodies, constraints, foundLevelActor);
}

// soft: the host streams it as one snapshot, a receiver steers its copy.
inline bool StructureSoft(StructureMode mode, uint64_t propId, bool detachedBody, size_t bodies, size_t constraints,
                          bool foundLevelActor = false) {
    return mode == StructureMode::Soft && IsStructure(propId, detachedBody, bodies, constraints, foundLevelActor);
}

// Nobody leases it, claims a blow on it or resets it: local and soft (every machine's own physics acts on its copy).
inline bool StructureOwnCopy(StructureMode mode, uint64_t propId, bool detachedBody, size_t bodies, size_t constraints,
                             bool foundLevelActor = false) {
    return mode != StructureMode::Full && IsStructure(propId, detachedBody, bodies, constraints, foundLevelActor);
}

// The announce says how many bodies the host's copy has (ObjectAnnounceMsg.bodyCount, both sides counted the same way and capped
// alike). A copy here with another count is a different shape: body N here is not body N there, and driving it would put pieces
// in each other's places. 0 is "not known" (a census prop): they agree.
inline bool ShapesAgree(size_t hostBodies, size_t localBodies) {
    return hostBodies == 0 || hostBodies == localBodies;
}

// Joint breaks of own-copy structures (soft and local). Any machine reports a joint that broke in its copy, and the host breaks
// its own and passes it on, so every screen ends up with the same pieces. A break this machine's sync could have caused is
// not passed on: while a structure settles after it was bound here (anchors and joints of breakables settle for some seconds
// after load), or just after this machine placed bodies of it (a snap, a rest placement, or PlaceBroken in object_sync.cpp). Such a break stays on
// this screen only, and the copy has diverged (CopyDiverged): steering its pieces toward a whole structure would throw them
// about. So does any break of a copy with another shape than the
// host's (ShapesAgree): breaks travel by index, and its joint N is not joint N on the host. One past this machine's report budget
// (JointReportBudget) is not sent yet: the host would not take it, and a copy that believes its break was shared would be
// corrected back to the whole structure. NextBreakStep says which of these wait and which stay here for good.
constexpr double kBreakSettleMs = 6000.0;   // sync::kSnapSettleMs is the same wait (object_sync.cpp checks that they agree)
constexpr double kBreakAfterSnapMs = 1000.0;
enum class LocalBreak : uint8_t { Share = 0, Settling = 1, AfterSnap = 2, ShapeDiffers = 3, OverBudget = 4, AfterRestPlace = 5 };
// A joint broke, or a body came off, on this screen only (the masks hold what was not passed on).
inline bool CopyDiverged(uint64_t localBroken, uint64_t bodyOffHere) { return localBroken != 0 || bodyOffHere != 0; }

// A diverged copy was left to its own physics for good up to 0.8.0-pre30, and Alley's barrels ended metres from the host's. Under
// soft, with structure_follow_broken on, a late-found prop's pieces go to the host's places instead. Not a census structure: a
// bridge is what teleports brought down (#230), so it stays as it is. Not another shape: body N here is not body N there.
inline bool FollowsWhenBroken(StructureMode mode, bool followBroken, bool found, bool shapeDiffers) {
    return mode == StructureMode::Soft && followBroken && found && !shapeDiffers;
}

// A body that hangs on another body of the object goes where that one goes, and setting its own transform is what tears it off:
// only the root and the bodies that hang on nothing are placed, by a snap or by PlaceBroken. (The masks hold bodies 1 to 63.)
inline bool BodyPlacedOnItsOwn(size_t body, uint64_t attachedMask) {
    return body == 0 || body >= 64 || !((attachedMask >> body) & 1);
}

// Should a broken copy's body go to the host's place? Only when it is off (offCm), and once placed only when the host's body has
// moved on since (hostMovedCm). A piece that fell here while the host's stood still stays where it fell: placing it each time a
// state arrived would reset it once a second, undo what this machine's fighter does to it, and bring a stale state back.
constexpr double kPlaceDeadBandCm = 10.0;
inline bool ShouldPlaceBody(bool placedBefore, double hostMovedCm, double offCm) {
    if (offCm < kPlaceDeadBandCm) return false;
    return !placedBefore || hostMovedCm >= kPlaceDeadBandCm;
}

// lastRestPlaceMs is when the rest lock last placed this copy (sync/hsm_rest_lock.h). A rest placement moves bodies as a snap does,
// so it takes the same window.
// lastHitMs is when a fighter or a weapon last touched this copy here. A break that soon after it is a hit, not the start-up that
// differs per machine, so it does not count as settling even in the first seconds.
constexpr double kBreakHitMs = 750.0;
inline LocalBreak ClassifyLocalBreak(double nowMs, double settledSinceMs, double lastSnapMs, bool shapeDiffers = false,
                                     double lastRestPlaceMs = -1e18, double lastHitMs = -1e18) {
    if (shapeDiffers) return LocalBreak::ShapeDiffers;
    if (nowMs - settledSinceMs < kBreakSettleMs && nowMs - lastHitMs >= kBreakHitMs) return LocalBreak::Settling;
    if (nowMs - lastRestPlaceMs < kBreakAfterSnapMs) return LocalBreak::AfterRestPlace;
    if (nowMs - lastSnapMs < kBreakAfterSnapMs) return LocalBreak::AfterSnap;
    return LocalBreak::Share;
}
// What PollEvents does with a break, each poll until it is sent or kept. Up to 0.8.0-pre30 every break not sent at once stayed on
// this screen for good, and Alley's barrels ended metres from the host's. The game has no way back from a break, so a copy
// that broke cannot rejoin the host's whole one: the host's has to break too.
//   Send  passed on now.
//   Hold  not yet. Over the budget it goes when a token comes. After a snap it goes when kBreakAfterSnapMs is over: nothing places
//         a copy that holds a break (BreaksHeld), so that time does run out. Sharing one that the snap caused costs a barrel that
//         comes apart on every screen, which is what the copies need to agree. A census structure does not take that risk: a
//         bridge is what teleports brought down (#230), so its break after a snap stays here.
//   Keep  here for good. While the structure settles nobody has touched it: the game's own start-up differs per machine (a
//         plank traces its anchors at BeginPlay), so the same break need not be real anywhere else.
//         A break just after a rest placement is kept as well: this machine's own placing moved the bodies (AfterRestPlace).
// shareHeld is structure_share_held_breaks; off, every break not sent at once is kept and a joint the game took away is not seen,
// as 0.8.0-pre30.
enum class BreakStep : uint8_t { Send = 0, Hold = 1, Keep = 2 };
inline BreakStep NextBreakStep(LocalBreak why, bool shareHeld, bool found) {
    if (why == LocalBreak::Share) return BreakStep::Send;
    if (shareHeld && (why == LocalBreak::OverBudget || (why == LocalBreak::AfterSnap && found))) return BreakStep::Hold;
    return BreakStep::Keep;
}
// A break waits to be passed on (the masks hold what waits): the copy is neither steered nor placed until it has gone.
// A plank of the game's barrel that came off on another machine comes off here by the barrel's own code (object_events.cpp), which
// also turns off its collision with the rings and lowers the barrel's Health. Not a ring: the game never breaks those by a hit.
inline bool ReplaysGameBreak(bool setting, bool ownCopy, bool barrelPlank) { return setting && ownCopy && barrelPlank; }

// Which bodies a rest placement moves. A census structure (a bridge) goes whole, so its joints see the host's shape; a late-found
// one as a snap does, the root and what hangs on nothing.
inline bool BodyPlacedAtRest(size_t body, uint64_t attachedMask, bool found) { return !found || BodyPlacedOnItsOwn(body, attachedMask); }

inline bool BreaksHeld(uint64_t heldJoints, uint64_t heldBodies) { return heldJoints != 0 || heldBodies != 0; }

inline const char* LocalBreakName(LocalBreak b) {
    return b == LocalBreak::Settling       ? "settling"
           : b == LocalBreak::AfterSnap    ? "after a snap"
           : b == LocalBreak::AfterRestPlace ? "after a rest placement"
           : b == LocalBreak::ShapeDiffers ? "body count differs"
           : b == LocalBreak::OverBudget   ? "too many at once"
                                           : "shared";
}

// Who may report a joint break: the streamer always; anyone on an own-copy structure.
inline bool MayReportJoint(bool isStreamer, bool ownCopy) { return isStreamer || ownCopy; }

// The host takes at most this many joint breaks a second from one machine (a burst refills over a second). A barrel that comes
// apart breaks 19 joints at once, so the burst fits two whole barrels. A machine keeps its own budget with a smaller burst, so
// what it sends fits the host's even though the two refill a little apart: what the host drops, the sender never hears of.
constexpr double kJointReportsPerSec = 8.0;
constexpr double kJointReportBurst = 40.0;
constexpr double kJointSendBurst = 30.0;
struct JointReportBudget {
    JointReportBudget() : JointReportBudget(kJointReportBurst) {}
    explicit JointReportBudget(double burstSize) : burst(burstSize), tokens(burstSize) {}
    double burst;
    double tokens;
    double lastMs = -1e18;
    // True (and one token spent) when a report from this machine at nowMs is taken.
    bool Take(double nowMs) {
        if (lastMs > -1e17) {
            const double dt = nowMs - lastMs;
            if (dt > 0) tokens = tokens + dt * (kJointReportsPerSec / 1000.0);
            if (tokens > burst) tokens = burst;
        }
        lastMs = nowMs;
        if (tokens < 1.0) return false;
        tokens -= 1.0;
        return true;
    }
};

// The host sends a soft structure's bodies when this is true: lastSentMs far in the past means never sent. atRest: none of
// its bodies moves (hsm_lease.h PropAtRest on each).
inline double StructureStreamPeriodMs(bool atRest) { return atRest ? kFreeIdleMs : kStructureStreamMs; }
inline bool StructureStreamDue(double lastSentMs, bool atRest, double nowMs) {
    return nowMs - lastSentMs >= StructureStreamPeriodMs(atRest);
}

}  // namespace hsm
