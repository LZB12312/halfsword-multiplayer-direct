#include "gameplay/save_guard.h"
#include <windows.h>
#include "gameplay/travel.h"
#include "log.h"
#include "session/hsm_gvas.h"
#include "ue/script_hook.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <utility>

namespace hsm::game {
using namespace hsm::ue;
namespace {

constexpr const wchar_t* kSave = L"GameProgress.sav";   // GI_Settings.cpp:1431 SaveGameToSlot(Save File, "GameProgress", 0)
constexpr const char* kHookOwner = "saveguard";
constexpr const wchar_t* kCareer = L"HSMMP_Progression.sav";          // the multiplayer career (session/hsm_mp_career.h)
constexpr const wchar_t* kCareerPrev = L"HSMMP_Progression.prev.sav";  // the career as it was before its last write
// The player's own passports, which a career load leaves as they are: Free Mode's character (the lobby loadout,
// hsm_gvas.h kPassportSaveProperty) and the stock player-2 slot (the default loadout). Load Game reads both from the
// save it loads, and Delete Save resets the second; neither belongs to the career.
constexpr const char* kOwnPassports[] = {"Free Mode Player Character 1", "Free Mode Player Character 2"};

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out, bool& exists) {
    out.clear();
    exists = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    exists = true;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1LL << 30);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// Written beside the target and moved over it, so a crash never leaves half a file.
bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    const std::wstring tmp = path + L".hsmtmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = bytes.empty() || (WriteFile(h, bytes.data(), DWORD(bytes.size()), &put, nullptr) && put == bytes.size());
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

std::wstring BackupName(unsigned long pid) {
    wchar_t b[64];
    std::swprintf(b, 64, L"HSMMP_backup_%lu_GameProgress.sav", pid);
    return b;
}

int64_t FileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return int64_t(u.QuadPart / 10000000ULL) - 11644473600LL;
}

// Is the process that made a backup (written at backupMtimeSec) still running? A process that started after the
// backup was written is a different process on a reused pid, not its owner.
bool ProcessAlive(unsigned long pid, int64_t backupMtimeSec) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (alive && GetProcessTimes(p, &created, &exited, &kernel, &user) && FileTimeToUnix(created) > backupMtimeSec + 1) alive = false;
    CloseHandle(p);
    return alive;
}

bool FileMtime(const std::wstring& path, int64_t& mtime) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    mtime = FileTimeToUnix(d.ftLastWriteTime);
    return true;
}

// Puts the backup back over GameProgress.sav when they differ (an empty backup: deletes a save the session made), then
// deletes the backup. False when it could not.
bool PutBack(const std::wstring& dir, const std::wstring& backup, const char* why) {
    std::vector<uint8_t> was, now;
    bool backupExists = false, saveExists = false;
    if (!ReadAll(dir + backup, was, backupExists) || !backupExists) { HSM_LOG("save: backup %ls unreadable (%s)", backup.c_str(), why); return false; }
    if (!ReadAll(dir + kSave, now, saveExists)) { HSM_LOG("save: %ls unreadable (%s); backup kept", kSave, why); return false; }
    bool ok = true;
    if (was.empty()) {
        if (saveExists) ok = DeleteFileW((dir + kSave).c_str()) != 0;
        HSM_LOG("save: %s: no save before the session; %s", why, saveExists ? (ok ? "the one made since is removed" : "could not remove the one made since") : "none made since");
    } else if (!saveExists || now != was) {
        ok = WriteAll(dir + kSave, was);
        HSM_LOG("save: %s: the game had changed GameProgress (%zu -> %zu bytes); %s", why, was.size(), now.size(),
                ok ? "the backup is put back" : "putting the backup back FAILED, backup kept");
    } else {
        HSM_LOG("save: %s: GameProgress unchanged", why);
    }
    if (ok) DeleteFileW((dir + backup).c_str());
    return ok;
}

// Renames a backup nothing should restore to HSMMP_stale_<utc>_<n>_GameProgress.sav (the backup glob does not match it).
void KeepAside(const std::wstring& dir, const std::wstring& backup, const char* why) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    for (int i = 0; i < 100; ++i) {
        wchar_t b[96];
        std::swprintf(b, 96, L"HSMMP_stale_%04u%02u%02uT%02u%02u%02uZ_%d_GameProgress.sav", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, i);
        if (MoveFileExW((dir + backup).c_str(), (dir + b).c_str(), 0)) { HSM_LOG("save: %ls kept aside as %ls (%s); GameProgress not touched", backup.c_str(), b, why); return; }
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    HSM_LOG("save: %ls could not be moved aside (%s); left as is", backup.c_str(), why);
}

// The player's own save back into the GameProgress slot after the career borrowed it; the backup stays (the session goes
// on). An empty backup means there was no save, so none is left in the slot.
bool RefillSlot(const std::wstring& dir, const std::wstring& backup) {
    std::vector<uint8_t> own;
    bool exists = false;
    if (!ReadAll(dir + backup, own, exists) || !exists) return false;
    if (own.empty()) return DeleteFileW((dir + kSave).c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
    return WriteAll(dir + kSave, own);
}

// Before the career borrows the slot, the backup's time moves to now: a crash while the slot holds the career is then no
// "save newer than its backup" at the next start (hsm_save_guard.h DecideLeftBackup), so the backup still goes back.
void TouchBackup(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(h, nullptr, nullptr, &now);
    CloseHandle(h);
}

// A career file that is no save: renamed HSMMP_Progression_bad_<utc>_<n>.sav (never overwritten). False when it could not be.
bool SetAsideCareer(const std::wstring& dir, std::wstring& to) {
    SYSTEMTIME t;
    GetSystemTime(&t);
    for (int i = 0; i < 100; ++i) {
        wchar_t b[96];
        std::swprintf(b, 96, L"HSMMP_Progression_bad_%04u%02u%02uT%02u%02u%02uZ_%d.sav", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, i);
        if (MoveFileExW((dir + kCareer).c_str(), (dir + b).c_str(), 0)) { to = b; return true; }
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    return false;
}

// The player's own passports from one object to another (the game instance and a blank SG_GameProgress_C have them all,
// same names and types). How many were copied.
size_t CopyPassports(UObject* from, UObject* to) {
    size_t n = 0;
    for (const char* name : kOwnPassports) {
        FProperty* s = FindProperty(ClassOf(from), name);
        FProperty* d = FindProperty(ClassOf(to), name);
        if (!s || !d || PropertySize(s) != PropertySize(d) || PropertyType(s) != PropertyType(d)) continue;
        CopyPropertyValue(d, reinterpret_cast<uint8_t*>(to) + PropertyOffset(d), reinterpret_cast<const uint8_t*>(from) + PropertyOffset(s));
        ++n;
    }
    return n;
}

std::atomic<bool> g_sweepRestored{false};   // the startup sweep put a save back: the game instance needs one Load Game

struct LeftFile {
    std::wstring name;
    unsigned long pid;
    uint64_t bytes;
    int64_t mtime;
};

// Backups left by sessions whose process is gone (a crash, a kill): restore, keep aside or ignore each (hsm_save_guard.h
// DecideLeftBackup). True when one was put back. Win32 only, so it also runs before the engine is up.
bool SweepLeftBackups(const std::wstring& dir, const char* who) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"HSMMP_backup_*_GameProgress.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<LeftFile> found;
    do {
        unsigned long pid = 0;
        if (std::swscanf(fd.cFileName, L"HSMMP_backup_%lu_", &pid) != 1 || !pid) continue;
        found.push_back({fd.cFileName, pid, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, FileTimeToUnix(fd.ftLastWriteTime)});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    bool restored = false;
    for (const LeftFile& f : found) {
        const bool alive = ProcessAlive(f.pid, f.mtime);
        const bool own = f.pid == GetCurrentProcessId() && alive;
        int64_t smtime = 0;
        const bool saveExists = FileMtime(dir + kSave, smtime);
        switch (session::DecideLeftBackup(true, alive, own, f.bytes, f.mtime, saveExists, smtime)) {
        case session::LeftBackup::Ignore: break;
        case session::LeftBackup::Restore: restored = PutBack(dir, f.name, who) || restored; break;
        case session::LeftBackup::KeepAside:
            KeepAside(dir, f.name, f.bytes == 0 ? "empty backup: there was no save" : "the save is newer than the backup");
            break;
        }
    }
    return restored;
}

}  // namespace

void SweepAtStartup() {
    wchar_t base[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    const std::wstring dir = std::wstring(base) + L"\\HalfSwordUE5\\Saved\\SaveGames\\";
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (SweepLeftBackups(dir, "left by a session that did not end (startup)")) g_sweepRestored = true;
}

void SaveGuard::Hook() {
    if (hooked_ || !ScriptHookActive()) return;
    // fightOnly: blocked in a session's fight levels only. In the Progression hub the game makes a new character
    // (Reset Player Character) and dresses it (Override Player Character Equipment) for the next fight; blocking them
    // there left the character screen stuck (test 2026-10-25). The in-memory character is the session's; the backup
    // still goes back over GameProgress when the session is over, and Save Game stays blocked everywhere.
    auto block = [this](const char* what, bool fightOnly = false) {
        return [this, what, fightOnly](ScriptCall&) {
            if (own_ || !policy_.Blocking() || (fightOnly && !matchLevel_)) return false;
            if (passOne_ && std::strcmp(passOne_, what) == 0) { passOne_ = nullptr; return false; }   // that call only, not a nested one
            if (++blocked_ % 50 == 1) HSM_LOG("save: blocked the game's \"%s\" during a session (%u so far)", what, blocked_);
            return true;
        };
    };
    // Literal names, one call each: tools/check_reflection.py verifies HookScript names against the game.
    int n = HookScript(kHookOwner, "GI_Settings_C", "Save Game", block("Save Game"));
    n += HookScript(kHookOwner, "GI_Settings_C", "Reset Player Character", block("Reset Player Character", true));
    n += HookScript(kHookOwner, "GI_Settings_C", "Override Player Character Equipment", block("Override Player Character Equipment", true));
    // The Tavern's DELETE SAVE (UI_DeleteSave_Confirm) in a session: the multiplayer career goes, never the player's own save;
    // the game's call is never let through (it deletes the slot GameProgress). Logged every time: it is the player's act.
    n += HookScript(kHookOwner, "GI_Settings_C", "Delete Save", [this](ScriptCall&) {
        if (own_ || !policy_.Blocking()) return false;
        const session::CareerStep step = career_.DeleteAsked();
        if (step != session::CareerStep::Delete) {
            HSM_LOG("save: the game's Delete Save during a session is blocked: your own save is not deleted, and no multiplayer career is held to delete");
            return true;
        }
        career_.Done(step, DeleteCareer());
        return true;
    });
    // The game reloads the game instance from GameProgress on its level loads (it saved it just before, so the pair
    // kept the session's settings). With the save blocked, the load brought back the pre-session ones: test 2026-10-25,
    // every lobby fight came up in Progression (Free Mode Activated false), so Duel ran as Progression co-op, with the
    // players' own save loadouts and no spawners for the mode. While the guard is on, the game instance in memory is
    // the truth; Restore's own Load Game runs after the guard is off.
    n += HookScript(kHookOwner, "GI_Settings_C", "Load Game", block("Load Game"));
    hooked_ = n > 0;
    if (hooked_ || !hookFailLogged_) HSM_LOG("save: %d game save function(s) hooked", n);
    hookFailLogged_ = !hooked_;
}

void SaveGuard::Tick(Adapter& adapter, bool joined, bool matchLevel) {
    if (!leftChecked_) RestoreLeft(adapter);
    matchLevel_ = matchLevel;
    switch (policy_.Update(joined, matchLevel)) {
    case session::SaveAction::Backup: Backup(adapter); break;
    case session::SaveAction::Restore: Restore(adapter); break;
    case session::SaveAction::None: break;
    }
    // Item 9b: the stock lose screens (UI_GiveUp_C, UI_Lose_C, UI_NextFIght_C) are suppressed only while joined, and the
    // guard is on from the same tick, so their "return to tavern" never reaches Override Player Character Equipment
    // unblocked. Backup hooks once; if that found nothing, keep trying while the guard is on.
    if (policy_.Blocking() && !hooked_ && ++hookRetryTicks_ % 60 == 0) Hook();   // about once a second
    // S2: a put-back that failed keeps the backup; retry while no session is on.
    if (!policy_.Blocking() && !backup_.empty() && ++putBackTicks_ % 300 == 0) RetryPutBack(adapter);   // about every 5 s
}

void SaveGuard::RetryPutBack(Adapter& adapter) {
    const std::wstring dir = adapter.SaveGamesDir();
    if (dir.empty()) return;
    if (PutBack(dir, backup_, "put-back retry")) {
        HSM_LOG("save: the backup is back after %u retr%s", putBackTries_ + 1, putBackTries_ ? "ies" : "y");
        backup_.clear();
        putBackTries_ = 0;
        Reload(adapter);
    } else if (++putBackTries_ >= 12) {
        KeepAside(dir, backup_, "put-back failed 12 times");
        backup_.clear();
        putBackTries_ = 0;
    }
}

void SaveGuard::Backup(Adapter& adapter) {
    Hook();
    blocked_ = 0;
    settings_ = CaptureGameSettings();
    const std::wstring dir = adapter.SaveGamesDir();
    // S2: a backup of this process still on disk is one a failed put-back kept: it holds the player's save from before
    // any session, so it stays. Backing up now would overwrite it with whatever the last session left in GameProgress.
    if (!backup_.empty() || (!dir.empty() && GetFileAttributesW((dir + BackupName(GetCurrentProcessId())).c_str()) != INVALID_FILE_ATTRIBUTES)) {
        if (backup_.empty()) backup_ = BackupName(GetCurrentProcessId());
        putBackTries_ = 0;
        HSM_LOG("save: an earlier backup (%ls) is still there and stays the one to restore; the game's own saves are off until the session is over", backup_.c_str());
        return;
    }
    if (dir.empty()) { HSM_LOG("save: no save folder; the game's own saves are still blocked for this session"); return; }
    std::vector<uint8_t> bytes;
    bool exists = false;
    if (!ReadAll(dir + kSave, bytes, exists)) { HSM_LOG("save: could not read %ls; blocked for this session, no backup", kSave); return; }
    const std::wstring name = BackupName(GetCurrentProcessId());
    if (!WriteAll(dir + name, bytes)) { HSM_LOG("save: could not write the backup; blocked for this session"); return; }
    backup_ = name;   // an empty backup: there was no save, and none is kept after the session
    HSM_LOG("save: GameProgress backed up (%zu bytes%s); the game's own saves are off until the session is over",
            bytes.size(), exists ? "" : ", no save yet");
}

void SaveGuard::Reload(Adapter& adapter) {
    UObject* gi = adapter.GameInstance();
    if (!gi) return;
    Call load(gi, "Load Game");   // GI_Settings.cpp:603: every saved field back into the game instance
    if (!load.valid() || !load.invoke()) HSM_LOG("save: Load Game could not be called");
}

void SaveGuard::Restore(Adapter& adapter) {
    // The career first, while the game instance still holds it and the backup the player's own save.
    if (career_.SessionOver() == session::CareerStep::Save) career_.Done(session::CareerStep::Save, SaveCareer(adapter, "session over"));
    const std::wstring dir = adapter.SaveGamesDir();
    if (!backup_.empty() && !dir.empty()) {
        if (PutBack(dir, backup_, "session over")) backup_.clear();
        else { putBackTries_ = 0; HSM_LOG("save: putting the backup back failed; the backup is kept and retried"); }
    } else {
        backup_.clear();
    }
    Reload(adapter);
    const size_t n = ApplyGameSettings(settings_);
    settings_.clear();
    HSM_LOG("save: guard off; %u game save call(s) were blocked, game instance reloaded, %zu setting(s) as before the session", blocked_, n);
}

void SaveGuard::RestoreLeft(Adapter& adapter) {
    const std::wstring dir = adapter.SaveGamesDir();
    if (dir.empty()) return;   // the engine is not up yet: next tick
    leftChecked_ = true;
    const bool restored = SweepLeftBackups(dir, "left by a session that did not end");
    // The startup sweep (before the engine was up) may have put a save back after the game instance read the old one.
    if (restored || g_sweepRestored.exchange(false)) Reload(adapter);
}

// ------------------------------------------------------------------ the multiplayer career (session/hsm_mp_career.h)

bool SaveGuard::BackupOnDisk(const std::wstring& dir) const {
    return !backup_.empty() && !dir.empty() && GetFileAttributesW((dir + backup_).c_str()) != INVALID_FILE_ATTRIBUTES;
}

void SaveGuard::CareerTravel(Adapter& adapter, bool progression, bool host) {
    if (!policy_.Blocking()) Tick(adapter, true, matchLevel_);   // a travel heard in the tick the session began: the backup first
    const bool guarded = policy_.Blocking() && BackupOnDisk(adapter.SaveGamesDir());
    const session::CareerStep step = career_.Travel(progression, guarded);
    if (step == session::CareerStep::Load) career_.Done(step, LoadCareer(adapter, host));
    else if (step == session::CareerStep::Save) career_.Done(step, SaveCareer(adapter, "Progression left"));
    else if (progression && !career_.Loaded() && !guarded)
        HSM_LOG("career: your own save has no backup this session, so its slot is not borrowed; Progression plays on the career in memory and keeps nothing");
}

bool SaveGuard::CaptureFightEnd(Adapter& adapter) {
    UObject* gi = adapter.GameInstance();
    if (!gi) return false;
    Call capture(gi, "Override Player Character Equipment");   // GI_Settings.cpp:725
    if (!capture.valid()) return false;
    capture.set<bool>("Keep Exact Values", false);
    passOne_ = "Override Player Character Equipment";
    const bool ok = capture.invoke();
    passOne_ = nullptr;
    return ok;
}

void SaveGuard::CareerHubLoaded(Adapter& adapter) {
    if (career_.HubLoaded() == session::CareerStep::Keep) career_.Done(session::CareerStep::Keep, SaveCareer(adapter, "in the Tavern"));
}

bool SaveGuard::LoadCareer(Adapter& adapter, bool host) {
    const std::wstring dir = adapter.SaveGamesDir();
    UObject* gi = adapter.GameInstance();
    if (dir.empty() || !gi) { HSM_LOG("career: no save folder or game instance; Progression plays on the career in memory and keeps nothing"); return false; }
    std::vector<uint8_t> bytes;
    bool exists = false;
    const bool readable = ReadAll(dir + kCareer, bytes, exists);
    std::string bad;
    const bool valid = exists && readable && CheckCareerSave(bytes.data(), bytes.size(), &bad);
    const session::CareerSource from = session::CareerSourceFor(exists, readable, valid);
    if (from == session::CareerSource::Refuse) {
        HSM_LOG("career: %ls could not be read; it is left as it is, and Progression plays on the career in memory and keeps nothing", kCareer);
        return false;
    }
    if (from == session::CareerSource::FreshSetAside) {
        std::wstring aside;
        if (!SetAsideCareer(dir, aside)) {
            HSM_LOG("career: %ls is no career save (%s) and could not be moved aside; Progression plays on the career in memory and keeps nothing", kCareer, bad.c_str());
            return false;
        }
        HSM_LOG("career: %ls is no career save (%s); kept aside as %ls, and a new career starts", kCareer, bad.c_str(), aside.c_str());
    }
    // What a career load leaves as it was: the travel's settings (the modes, Free Mode's choices; on the host the Day Time
    // and Day Count are the career's) and the player's own passports, held in a blank save object over the load.
    std::vector<Setting> keep = CaptureGameSettings();
    if (host) keep.erase(std::remove_if(keep.begin(), keep.end(), [](const Setting& s) { return s.name == "Day Time" || s.name == "Day Count"; }), keep.end());
    UObject* holder = adapter.NewSaveObject();
    const size_t held = holder ? CopyPassports(gi, holder) : 0;
    TouchBackup(dir + backup_);
    bool ok = false;
    own_ = true;
    if (from == session::CareerSource::File) {
        Call load(gi, "Load Game");   // GI_Settings.cpp:603: every saved field from the slot "GameProgress", the career's for this call
        ok = WriteAll(dir + kSave, bytes) && load.valid() && load.invoke();
    } else {
        // A new career: the game's own Delete Save (GI_Settings.cpp:292) resets every career field, makes a character and
        // fills the fight board. First Start stays on, so the Tavern opens with character creation (Map_Hub_Tavern_Frank);
        // the tutorial is marked as played, since finishing creation would otherwise open the Cellar by itself and leave
        // the session (UI_CharacterCreation.cpp:433), and so is the intro video.
        Call reset(gi, "Delete Save");
        ok = reset.valid() && reset.invoke();
        if (ok) { SetBool(gi, "First Start Tutorial Played", true); SetBool(gi, "First Start Intro Played", true); }
    }
    own_ = false;
    if (held) CopyPassports(holder, gi);
    const size_t kept = ApplyGameSettings(keep);
    const bool back = RefillSlot(dir, backup_);
    if (!ok) HSM_LOG("career: the multiplayer career could not be %s; Progression plays on the career in memory and keeps nothing",
                     from == session::CareerSource::File ? "read in" : "made");
    else if (from == session::CareerSource::File) HSM_LOG("career: multiplayer career read in from %ls (%zu bytes); %zu setting(s) and %zu own passport(s) kept", kCareer, bytes.size(), kept, held);
    else HSM_LOG("career: a new multiplayer career (character creation in the Tavern); %zu setting(s) and %zu own passport(s) kept", kept, held);
    if (!back) HSM_LOG("career: putting your own save back in its slot FAILED; the backup still holds it and goes back when the session ends");
    if (ok) careerDir_ = dir;
    return ok;
}

bool SaveGuard::DeleteCareer() {
    if (careerDir_.empty()) { HSM_LOG("career: Delete Save: no save folder known; the multiplayer career is left as it is"); return false; }
    const std::wstring file = careerDir_ + kCareer, prev = careerDir_ + kCareerPrev;
    const bool had = GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
    // One step back, as every career write keeps (HSMMP_Progression.prev.sav): a misclick is not the end of a career.
    const bool gone = !had || MoveFileExW(file.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!gone) {
        HSM_LOG("career: Delete Save: %ls could not be removed (Windows error %lu); the multiplayer career stays", kCareer, GetLastError());
        return false;
    }
    HSM_LOG("career: Delete Save: the multiplayer career is deleted%s; your own save is untouched. The next Progression starts a new "
            "one (character creation)", had ? " (the last one kept as HSMMP_Progression.prev.sav)" : " (it had not been saved yet)");
    return true;
}

bool SaveGuard::SaveCareer(Adapter& adapter, const char* why) {
    const std::wstring dir = adapter.SaveGamesDir();
    UObject* gi = adapter.GameInstance();
    if (dir.empty() || !gi || !BackupOnDisk(dir)) {
        HSM_LOG("career: NOT saved (%s): %s", why, dir.empty() || !gi ? "no save folder or game instance" : "the backup of your own save is gone, so its slot is not borrowed");
        return false;
    }
    // The game's own Save Game writes the career into the slot, emptied first so it makes a fresh save object and nothing
    // of the player's own save rides along into the career's file.
    TouchBackup(dir + backup_);
    DeleteFileW((dir + kSave).c_str());
    Call save(gi, "Save Game");   // GI_Settings.cpp:1355
    own_ = true;
    const bool called = save.valid() && save.invoke();
    own_ = false;
    std::vector<uint8_t> bytes;
    bool exists = false;
    std::string bad;
    const bool read = called && ReadAll(dir + kSave, bytes, exists) && exists;
    const bool good = read && CheckCareerSave(bytes.data(), bytes.size(), &bad);
    bool kept = false;
    if (good) {
        if (GetFileAttributesW((dir + kCareer).c_str()) != INVALID_FILE_ATTRIBUTES)
            CopyFileW((dir + kCareer).c_str(), (dir + kCareerPrev).c_str(), FALSE);   // one step back, should a write go wrong
        kept = WriteAll(dir + kCareer, bytes);
    }
    const bool back = RefillSlot(dir, backup_);
    if (kept) HSM_LOG("career: saved (%s): %zu bytes to %ls", why, bytes.size(), kCareer);
    else HSM_LOG("career: NOT saved (%s): %s", why, !called ? "Save Game could not be called" : !read ? "the game wrote no save" : !good ? bad.c_str() : "the file could not be written");
    if (!back) HSM_LOG("career: putting your own save back in its slot FAILED; the backup still holds it and goes back when the session ends");
    return kept;
}

}  // namespace hsm::game
