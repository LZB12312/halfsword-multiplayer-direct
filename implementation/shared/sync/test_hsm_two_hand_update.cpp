#include "sync/hsm_two_hand_update.h"
#include <array>
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static bool Same(const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static bool Same(const GripFrame& a, const GripFrame& b) {
    return Same(a.pos, b.pos) && Same(a.primary, b.primary) && Same(a.secondary, b.secondary);
}
static bool Same(const std::array<GripFrame, 2>& a, const std::array<GripFrame, 2>& b) {
    return Same(a[0], b[0]) && Same(a[1], b[1]);
}
static std::array<GripFrame, 2> Frames(double offset) {
    return {GripFrame{{offset, 2, 3}, {0, 1, 0}, {0, 0, 1}},
            GripFrame{{-offset, 5, 6}, {0, 0, 1}, {1, 0, 0}}};
}

// Independent engine model: failed calls cannot mutate a frame; successful calls change the physical reference.
struct Writer {
    std::array<GripFrame, 2> physical;
    std::vector<bool> outcomes;
    std::vector<size_t> hands;
    std::vector<GripFrame> values;

    bool operator()(size_t hand, const GripFrame& frame) {
        const size_t call = hands.size();
        hands.push_back(hand);
        values.push_back(frame);
        CHECK(hand < physical.size());
        CHECK(call < outcomes.size());
        if (hand >= physical.size() || call >= outcomes.size() || !outcomes[call]) return false;
        physical[hand] = frame;
        return true;
    }
};

static void TestPairSucceeds() {
    const auto original = Frames(10);
    const auto requested = Frames(20);
    auto current = original;
    Writer write{original, {true, true}, {}, {}};
    const auto result = UpdateTwoHandFrames(current, requested, write);
    CHECK(result.applied && !result.rollbackFailed);
    CHECK((write.hands == std::vector<size_t>{0, 1}));
    CHECK(Same(current, requested));
    CHECK(Same(current, write.physical));
    CHECK(Same(original, Frames(10)));
    CHECK(Same(requested, Frames(20)));
}

static void TestFirstFailureDoesNotTouchLeft() {
    const auto original = Frames(10);
    const auto requested = Frames(20);
    auto current = original;
    Writer write{original, {false}, {}, {}};
    const auto result = UpdateTwoHandFrames(current, requested, write);
    CHECK(!result.applied && !result.rollbackFailed);
    CHECK((write.hands == std::vector<size_t>{0}));
    CHECK(Same(current, original));
    CHECK(Same(current, write.physical));
}

static void TestSecondFailureRollsBackRight() {
    const auto original = Frames(10);
    const auto requested = Frames(20);
    auto current = original;
    Writer write{original, {true, false, true}, {}, {}};
    const auto result = UpdateTwoHandFrames(current, requested, write);
    CHECK(!result.applied && !result.rollbackFailed);
    CHECK((write.hands == std::vector<size_t>{0, 1, 0}));
    CHECK(write.values.size() == 3);
    if (write.values.size() == 3) CHECK(Same(write.values[2], original[0]));
    CHECK(Same(current, original));
    CHECK(Same(current, write.physical));
    CHECK(Same(original, Frames(10)));
    CHECK(Same(requested, Frames(20)));
}

static void TestRollbackFailureKeepsTruthAndCanResume() {
    const auto original = Frames(10);
    const auto requested = Frames(20);
    auto current = original;
    Writer write{original, {true, false, false}, {}, {}};
    const auto result = UpdateTwoHandFrames(current, requested, write);
    CHECK(!result.applied && result.rollbackFailed);
    CHECK((write.hands == std::vector<size_t>{0, 1, 0}));
    CHECK(Same(current[0], requested[0]));
    CHECK(Same(current[1], original[1]));
    CHECK(Same(current, write.physical));
    CHECK(Same(original, Frames(10)));
    CHECK(Same(requested, Frames(20)));

    // A subsequent update's rollback must preserve this actual mixed state, not falsely restore the native snapshot.
    const auto mixed = current;
    Writer retry{write.physical, {true, false, true}, {}, {}};
    const auto failedAgain = UpdateTwoHandFrames(current, Frames(30), retry);
    CHECK(!failedAgain.applied && !failedAgain.rollbackFailed);
    CHECK(Same(current, mixed));
    CHECK(Same(current, retry.physical));

    Writer resume{retry.physical, {true, true}, {}, {}};
    const auto recovered = UpdateTwoHandFrames(current, requested, resume);
    CHECK(recovered.applied && !recovered.rollbackFailed);
    CHECK(Same(current, requested));
    CHECK(Same(current, resume.physical));

    // The caller's independently saved native frames remain usable for an explicit restoration.
    Writer restore{resume.physical, {true, true}, {}, {}};
    const auto restored = UpdateTwoHandFrames(current, original, restore);
    CHECK(restored.applied && !restored.rollbackFailed);
    CHECK(Same(current, original));
    CHECK(Same(current, restore.physical));
}

static void TestAliasedTarget() {
    auto current = Frames(10);
    Writer write{current, {true, true}, {}, {}};
    const auto result = UpdateTwoHandFrames(current, current, write);
    CHECK(result.applied && !result.rollbackFailed);
    CHECK(Same(current, Frames(10)));
    CHECK(Same(current, write.physical));
}

static void TestUnchangedNeedsNoRestoration() {
    for (GripJointState state : {GripJointState::Same, GripJointState::Gone, GripJointState::Unavailable}) {
        bool changed = false;
        int calls = 0;
        CHECK(RestoreGripFrame(Frames(10)[0], changed, state, [&](const GripFrame&) { ++calls; return false; }));
        CHECK(!changed && calls == 0);
    }
}

static void TestUnavailableKeepsRestorationWithoutWriting() {
    const GripFrame original = Frames(10)[0];
    bool changed = true;
    int calls = 0;
    CHECK(!RestoreGripFrame(original, changed, GripJointState::Unavailable,
                            [&](const GripFrame&) { ++calls; return true; }));
    CHECK(changed && calls == 0);
    CHECK(Same(original, Frames(10)[0]));
}

static void TestGoneDischargesWithoutWritingNewJoint() {
    const GripFrame original = Frames(10)[0];
    const GripFrame newJoint = Frames(30)[0];
    GripFrame physical = newJoint;
    bool changed = true;
    int calls = 0;
    CHECK(RestoreGripFrame(original, changed, GripJointState::Gone, [&](const GripFrame& frame) {
        ++calls; physical = frame; return true;
    }));
    CHECK(!changed && calls == 0);
    CHECK(Same(physical, newJoint));
}

static void TestFailedRestoreRetriesWithoutLosingOriginal() {
    const GripFrame original = Frames(10)[0];
    const GripFrame target = Frames(20)[0];
    GripFrame physical = target;
    bool changed = true;
    int calls = 0;
    auto fail = [&](const GripFrame& frame) {
        ++calls;
        CHECK(Same(frame, original));
        return false;   // engine failure leaves physical unchanged
    };
    CHECK(!RestoreGripFrame(original, changed, GripJointState::Same, fail));
    CHECK(changed && calls == 1 && Same(physical, target));
    CHECK(!RestoreGripFrame(original, changed, GripJointState::Unavailable, fail));
    CHECK(changed && calls == 1 && Same(physical, target));
    auto succeed = [&](const GripFrame& frame) {
        ++calls; physical = frame; return true;
    };
    CHECK(RestoreGripFrame(original, changed, GripJointState::Same, succeed));
    CHECK(!changed && calls == 2 && Same(physical, original));
    CHECK(Same(original, Frames(10)[0]));
    CHECK(RestoreGripFrame(original, changed, GripJointState::Same, fail));
    CHECK(!changed && calls == 2);   // a completed restoration is not written again
}

int main() {
    TestPairSucceeds();
    TestFirstFailureDoesNotTouchLeft();
    TestSecondFailureRollsBackRight();
    TestRollbackFailureKeepsTruthAndCanResume();
    TestAliasedTarget();
    TestUnchangedNeedsNoRestoration();
    TestUnavailableKeepsRestorationWithoutWriting();
    TestGoneDischargesWithoutWritingNewJoint();
    TestFailedRestoreRetriesWithoutLosingOriginal();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("test_hsm_two_hand_update: ok");
    return 0;
}
