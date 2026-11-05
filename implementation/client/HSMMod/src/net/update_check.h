// Direct fork: auto-download verified releases from LZB12312/halfsword-multiplayer-direct.
// Restart the game to use an installed update; update_check=0 disables checking/downloading.
// Update check: once per game start, one HTTPS GET of GitHub's releases list (api.github.com) on a worker thread, with
// a User-Agent of "HSMMod/<version>" and nothing else about the player. The result is logged, shown on the multiplayer
// page and once in the chat of the first fight level. Which release counts as newer is session/hsm_version.h; the
// request goes through net/http_client.h, so WinHTTP is loaded only now. update_check=0 under [net] turns it off.
//
// UPDATE NOW: when the newer release carries dwmapi.dll and dwmapi.dll.sha256 (session/hsm_update.h), the notice gets a
// button. A click downloads both on a worker thread, checks the SHA-256, the size and that the file is a 64-bit dwmapi
// proxy DLL, writes dwmapi.dll.new beside the running DLL, renames the running one to dwmapi.dll.old and the new one
// into place (any failure puts the old one back). The player restarts the game; the next start deletes the .old file.
#pragma once
#include <string>

namespace hsm::update {
// Starts the check (a no-op when disabled or already started). Returns at once; the worker logs the outcome.
void Start(bool enabled);
// Game thread: "HSMMod 0.7.0-pre3 is out (you have 0.7.0-pre2). Get it from GitHub: Dudiebug/halfsword-multiplayer-mod"
// once a newer release is known, else empty.
std::string Notice();
// Game thread: the update button is offered (a newer release with usable assets is known, and no install is running
// or finished).
bool CanInstall();
// Game thread: the button was clicked. Starts the worker and returns at once; InstallStatus() tells how it goes.
void StartInstall();
// "Downloading 3.6 MB...", "Update ready: restart the game", "Update failed: <reason>", or empty before a click.
std::string InstallStatus();
// Notice() the first time it is non-empty, then empty: for the one chat line.
std::string TakeChatNotice();
}  // namespace hsm::update
