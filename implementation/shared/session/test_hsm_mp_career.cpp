// Unit tests for hsm_mp_career.h. Run via implementation/shared/run_tests.sh test_hsm_mp_career.
#include "session/hsm_mp_career.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::session;

static void TestLobbyFightsNeverTouchIt() {
    MpCareer m;
    CHECK(m.Travel(false, true) == CareerStep::None);   // a lobby fight or a menu, no career held
    CHECK(m.HubLoaded() == CareerStep::None);
    CHECK(m.SessionOver() == CareerStep::None);
    CHECK(!m.Loaded());
}

static void TestEnteringProgressionLoadsOnce() {
    MpCareer m;
    CHECK(m.Travel(true, true) == CareerStep::Load);
    m.Done(CareerStep::Load, true);
    CHECK(m.Loaded());
    CHECK(m.Travel(true, true) == CareerStep::None);    // the Tavern to a fight and back: the same career, in memory
    CHECK(m.Travel(true, true) == CareerStep::None);
}

static void TestUnguardedSessionKeepsNothing() {
    MpCareer m;
    CHECK(m.Travel(true, false) == CareerStep::None);   // no backup of the player's own save: the slot is not borrowed
    CHECK(!m.Loaded());
    CHECK(m.HubLoaded() == CareerStep::None);
    CHECK(m.SessionOver() == CareerStep::None);
}

static void TestHubKeepsAndStays() {
    MpCareer m;
    m.Travel(true, true); m.Done(CareerStep::Load, true);
    CHECK(m.HubLoaded() == CareerStep::Keep);
    m.Done(CareerStep::Keep, true);
    CHECK(m.Loaded());                                   // a checkpoint is no hand-over
    m.Done(CareerStep::Keep, false);
    CHECK(m.Loaded());                                   // nor a failed one
}

static void TestLeavingProgressionSaves() {
    MpCareer m;
    m.Travel(true, true); m.Done(CareerStep::Load, true);
    CHECK(m.Travel(false, true) == CareerStep::Save);   // to a lobby fight or the menu: before its setup reaches the career
    m.Done(CareerStep::Save, true);
    CHECK(!m.Loaded());
    CHECK(m.SessionOver() == CareerStep::None);          // saved already, and the game instance holds no career
    CHECK(m.Travel(true, true) == CareerStep::Load);    // back to the Tavern: read in again
}

static void TestAFailedSaveStillLetsGo() {
    MpCareer m;
    m.Travel(true, true); m.Done(CareerStep::Load, true);
    m.Done(CareerStep::Save, false);
    CHECK(!m.Loaded());                                  // the game instance is about to hold something else
    CHECK(m.SessionOver() == CareerStep::None);
}

static void TestSessionOverSaves() {
    MpCareer m;
    m.Travel(true, true); m.Done(CareerStep::Load, true);
    CHECK(m.SessionOver() == CareerStep::Save);
    m.Done(CareerStep::Save, true);
    CHECK(!m.Loaded());
    CHECK(m.SessionOver() == CareerStep::None);
}

static void TestAFailedLoadWaitsForTheNextVisit() {
    MpCareer m;
    CHECK(m.Travel(true, true) == CareerStep::Load);
    m.Done(CareerStep::Load, false);
    CHECK(!m.Loaded());
    CHECK(m.Travel(true, true) == CareerStep::None);    // the Tavern's fights would fail the same way
    CHECK(m.HubLoaded() == CareerStep::None);
    CHECK(m.SessionOver() == CareerStep::None);          // nothing held, nothing to save over the file
    CHECK(m.Travel(false, true) == CareerStep::None);   // Progression left
    CHECK(m.Travel(true, true) == CareerStep::Load);    // the next visit tries again
}

// The report: the Tavern's DELETE SAVE did nothing in a session. It deletes the multiplayer career, and nothing saves it back.
static void TestDeleteSave() {
    MpCareer m;
    CHECK(m.DeleteAsked() == CareerStep::None);                // no career held: blocked, the player's own save stays
    m.Travel(true, true); m.Done(CareerStep::Load, true);
    CHECK(m.DeleteAsked() == CareerStep::Delete);
    m.Done(CareerStep::Delete, true);
    CHECK(!m.Loaded() && m.SessionOver() == CareerStep::None);  // gone: the session's end keeps nothing of it
    CHECK(m.Travel(false, true) == CareerStep::None);           // the confirm's trip to the menu saves nothing either
    CHECK(m.Travel(true, true) == CareerStep::Load);            // the next Progression: no file, a new career
    m.Done(CareerStep::Load, true);
    m.Done(CareerStep::Delete, false);                          // the file could not go: the career stays held, as it was
    CHECK(m.Loaded() && m.HubLoaded() == CareerStep::Keep);
}

static void TestSources() {
    CHECK(CareerSourceFor(false, false, false) == CareerSource::Fresh);
    CHECK(CareerSourceFor(false, true, true) == CareerSource::Fresh);
    CHECK(CareerSourceFor(true, true, true) == CareerSource::File);
    CHECK(CareerSourceFor(true, true, false) == CareerSource::FreshSetAside);
    CHECK(CareerSourceFor(true, false, false) == CareerSource::Refuse);   // never a fresh career over a file it could not read
    CHECK(CareerSourceFor(true, false, true) == CareerSource::Refuse);
}

int main() {
    TestDeleteSave();
    TestLobbyFightsNeverTouchIt();
    TestEnteringProgressionLoadsOnce();
    TestUnguardedSessionKeepsNothing();
    TestHubKeepsAndStays();
    TestLeavingProgressionSaves();
    TestAFailedSaveStillLetsGo();
    TestSessionOverSaves();
    TestAFailedLoadWaitsForTheNextVisit();
    TestSources();
    if (g_fail) { std::printf("test_hsm_mp_career: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_mp_career: ok\n");
    return 0;
}
