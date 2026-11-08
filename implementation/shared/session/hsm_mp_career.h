// Progression's own career for multiplayer (pure, no engine types; the client side is gameplay/save_guard.cpp).
//
// A session's Progression plays on a career kept apart from the player's single-player one, in HSMMP_Progression.sav
// beside the game's GameProgress.sav, and what the session does to it is kept (owner decision 2026-11-03). Each machine
// keeps its own: each player fights as their own career character, with their own purse and body.
//
// The game saves and loads only the slot "GameProgress" (GI_Settings Save Game / Load Game), and the save guard keeps the
// player's own GameProgress.sav backed up for the whole session. So the career borrows that slot for a moment: to load
// it, its file goes into the slot and the game's own Load Game reads it; to keep it, the game's own Save Game writes the
// slot and the file is copied out. Either way the player's own save goes straight back into the slot, so the file on disk
// is theirs at all times but that moment, and a crash leaves it as it was.
//
//   MpCareer         which career the game instance holds, and what a travel, a hub load or the session's end does to it
//   CareerSourceFor  where a career comes from when it is loaded: its file, or a fresh one
#pragma once
#include <cstdint>

namespace hsm::session {

enum class CareerStep : uint8_t {
    None,
    Load,   // the career goes into the game instance (from its file, or a fresh one when there is none)
    Save,   // the career goes to its file and the game instance stops holding it (Progression left, the session over)
    Keep,   // the career goes to its file and stays (the Tavern after a fight: a crash loses that fight at most)
    Delete, // the career's file goes and nothing keeps it again (the Tavern's DELETE SAVE in a session)
};

class MpCareer {
public:
    // This machine opens a level in a session. progression: a career level (the Tavern hub or one of its fights: Free Mode
    // off, not a menu). guarded: the save guard holds the player's own save, so the slot may be borrowed; without that the
    // session plays on the career in memory as it always did, and nothing is kept. A load that failed is not tried again
    // until Progression is left: every travel between the Tavern and its fights would fail the same way.
    CareerStep Travel(bool progression, bool guarded) {
        if (progression) return !loaded_ && !failed_ && guarded ? CareerStep::Load : CareerStep::None;
        failed_ = false;
        return loaded_ ? CareerStep::Save : CareerStep::None;
    }
    CareerStep HubLoaded() const { return loaded_ ? CareerStep::Keep : CareerStep::None; }
    CareerStep SessionOver() const { return loaded_ ? CareerStep::Save : CareerStep::None; }
    // The game's Delete Save in a session (the Tavern's DELETE SAVE, UI_DeleteSave_Confirm): it is the multiplayer career that
    // goes, never the player's own save (the report: the button did nothing in a session, the call was blocked). With no
    // career held (progression_save=0, or none loaded) the call stays blocked: the player's own save is not deleted either.
    CareerStep DeleteAsked() const { return loaded_ ? CareerStep::Delete : CareerStep::None; }

    // What came of a step. ok for a Load: the game instance holds the career now (it was read in, or a fresh one was made).
    // A Save gives the career up either way: the game instance is about to hold something else (a lobby fight's setup,
    // the player's own save), and keeping a career whose write failed would save that over the file later. A Delete done
    // gives it up too, so nothing saves it back; the next Progression loads, finds no file and starts a new one.
    void Done(CareerStep s, bool ok) {
        if (s == CareerStep::Load) { loaded_ = ok; failed_ = !ok; }
        else if (s == CareerStep::Save) loaded_ = false;
        else if (s == CareerStep::Delete && ok) loaded_ = false;
    }
    bool Loaded() const { return loaded_; }

private:
    bool loaded_ = false;
    bool failed_ = false;
};

// Where a career being loaded comes from. exists: HSMMP_Progression.sav is there; readable: its bytes could be read;
// valid: they read as a career save (hsm_gvas.h CheckCareerSave).
enum class CareerSource : uint8_t {
    File,           // the career on file
    Fresh,          // no career yet: a new one (the Tavern starts with character creation)
    FreshSetAside,  // a file that is no career save: renamed aside (never overwritten), then a new one
    Refuse,         // a file that could not be read: no career this time, so the next save cannot overwrite it
};
inline CareerSource CareerSourceFor(bool exists, bool readable, bool valid) {
    if (!exists) return CareerSource::Fresh;
    if (!readable) return CareerSource::Refuse;
    return valid ? CareerSource::File : CareerSource::FreshSetAside;
}

}  // namespace hsm::session
