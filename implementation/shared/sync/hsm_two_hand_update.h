// Apply the two grip references as a pair. Engine calls can fail independently, so a failed second write rolls the
// first back. If rollback also fails, current still describes the reference that was actually applied, for a later
// recovery/restoration attempt. No original/native-frame snapshots are owned or changed here.
#pragma once
#include "sync/hsm_two_hand_pose.h"
#include <array>
#include <cstddef>

namespace hsm::sync {

struct TwoHandWriteResult {
    bool applied = false;          // both requested references were applied
    bool rollbackFailed = false;   // a successful first write could not be undone after the second failed
};

// Same: the saved constraint still joins the verified original bodies/bones. Gone: it is proven destroyed or
// rebound and no longer owns that reference. Unavailable: identity could not be checked; this is not proof of Gone.
enum class GripJointState { Same, Gone, Unavailable };

// A saved native reference remains an obligation until a write succeeds or its original joint is proven gone.
// write(original) returns true only when the original frame was physically applied. A failed write leaves the
// engine unchanged. The callback must not mutate changed; false leaves it set for a later restoration attempt.
template<class Write>
bool RestoreGripFrame(const GripFrame& original, bool& changed, GripJointState state, Write&& write) {
    if (!changed) return true;
    if (state == GripJointState::Gone) { changed = false; return true; }
    if (state != GripJointState::Same || !write(original)) return false;
    changed = false;
    return true;
}

// current must describe the two references in the engine on entry. write(hand, frame) returns true only when that
// frame was physically applied; false guarantees that call left the frame unchanged. Hands are 0 right, 1 left.
// A failed first write never attempts the left hand. Copies permit requested to alias current and keep the rollback
// target stable even while successful calls update current. The callback must not mutate current itself.
template<class Write>
TwoHandWriteResult UpdateTwoHandFrames(std::array<GripFrame, 2>& current,
                                       const std::array<GripFrame, 2>& requested, Write&& write) {
    const std::array<GripFrame, 2> before = current;
    const std::array<GripFrame, 2> target = requested;
    if (!write(size_t{0}, target[0])) return {};
    current[0] = target[0];
    if (write(size_t{1}, target[1])) {
        current[1] = target[1];
        return {true, false};
    }
    if (write(size_t{0}, before[0])) {
        current[0] = before[0];
        return {};
    }
    return {false, true};
}

}  // namespace hsm::sync
