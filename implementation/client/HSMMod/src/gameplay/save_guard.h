// Keeps a multiplayer session out of the player's own game save (session/hsm_save_guard.h has the rule).
//
// While the guard is on: GameProgress.sav is backed up (HSMMP_backup_<pid>_GameProgress.sav next to it), and the
// game instance's own Save Game, Reset Player Character, Override Player Character Equipment, Delete Save and Load
// Game are blocked in the script VM. When it goes off: the backup is put back if the game wrote the save anyway, the game
// instance is reloaded from it (Load Game) and the settings it had before the session (play mode, Free Mode, ...) are
// written back, so the tavern after a session is the one before it. A backup whose process is gone (a crash) is put
// back at the next start. Game thread only.
//
// A session's Progression plays on the multiplayer career instead (session/hsm_mp_career.h): HSMMP_Progression.sav,
// read in through the game's own Load Game and written out through its own Save Game, each time with the player's own
// save put straight back into the GameProgress slot.
#pragma once
#include "gameplay/adapter.h"
#include "hsm_protocol.h"
#include "session/hsm_mp_career.h"
#include "session/hsm_save_guard.h"
#include <string>
#include <vector>

namespace hsm::game {

// Startup sweep (S1): backups left by sessions that did not end are restored, kept aside or ignored before anything
// else, even with role=off. Win32 only (resolves %LOCALAPPDATA%\HalfSwordUE5\Saved\SaveGames itself).
void SweepAtStartup();

class SaveGuard {
public:
    // Every tick. joined: in a session (lobby joined or hosting); matchLevel: this machine is in a session fight level.
    void Tick(Adapter& adapter, bool joined, bool matchLevel);
    bool Blocking() const { return policy_.Blocking(); }

    // The multiplayer career. In a session, this machine is about to open a level: progression = a career level (Free
    // Mode off, not a menu). Entering Progression reads the career in (a new one the first time), leaving it writes the
    // career out. host: the career's Day Time stays; a client keeps the one the host's travel brings, so every machine
    // lights the level alike. Call it before the travel's settings are set on the game instance.
    void CareerTravel(Adapter& adapter, bool progression, bool host);
    void CareerHubLoaded(Adapter& adapter);    // the Tavern of a session loaded: the career is written as it is now
    bool CareerLoaded() const { return career_.Loaded(); }
    // A Progression fight's end (session/hsm_career.h): the game's own capture, Override Player Character Equipment with Keep
    // Exact Values false (the body, the kit of a clean win, the bolts). Only that call goes through the guard; the Save Game it
    // ends with stays blocked (the career is written in the Tavern). False when it could not be called.
    bool CaptureFightEnd(Adapter& adapter);

private:
    void Hook();
    bool LoadCareer(Adapter& adapter, bool host);
    bool SaveCareer(Adapter& adapter, const char* why);
    bool DeleteCareer();                  // the Tavern's DELETE SAVE in a session: the career's file goes (kept one step back)
    bool BackupOnDisk(const std::wstring& dir) const;
    void Backup(Adapter& adapter);
    void Restore(Adapter& adapter);
    void RestoreLeft(Adapter& adapter);   // once per process: backups of sessions whose process is gone
    void RetryPutBack(Adapter& adapter);   // S2: a failed put-back keeps the backup; tried again every few seconds
    static void Reload(Adapter& adapter);

    session::SaveGuardPolicy policy_;
    session::MpCareer career_;
    bool own_ = false;                    // the guard's own call of a blocked function (the career's load and save) goes through
    const char* passOne_ = nullptr;       // the one blocked function let through for this call (the fight end's capture), not what it calls
    bool hooked_ = false, leftChecked_ = false, hookFailLogged_ = false;
    bool matchLevel_ = false;             // this tick's matchLevel (the fight-only blocks)
    std::wstring backup_;                 // this session's backup file (empty: none made)
    std::wstring careerDir_;              // the SaveGames folder of the career held (set by its load)
    std::vector<Setting> settings_;       // the game instance's settings before the session
    unsigned blocked_ = 0;                // calls blocked this session
    unsigned hookRetryTicks_ = 0, putBackTicks_ = 0, putBackTries_ = 0;
};

}  // namespace hsm::game
