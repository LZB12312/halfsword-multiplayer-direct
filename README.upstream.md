# Half Sword Multiplayer Mod (HSMMod)

Online multiplayer for Half Sword in one drop-in DLL. No game files are changed; delete the DLL to go back to vanilla.

[![Discord](https://img.shields.io/badge/Discord-join%20the%20community-5865F2?logo=discord&logoColor=white)](https://discord.gg/QZcSFnwxra)
[![Latest release](https://img.shields.io/github/v/release/Dudiebug/halfsword-multiplayer-mod?label=download)](../../releases/latest)

> **Status:** v0.7.3 is the current release. Expect rough edges and please report what you find with `/bug` in the
> game. Everyone in a session needs the same version (0.7.3 connects only to 0.7.3).
>
> **Prerelease:** v0.8.0-pre21 tries out optional host physics, the same strength for every player by tier, steadier
> bridges and chains, Zombies on the Abyss and give-up in Survival and Zombies, with fixes from the latest play-session
> reports. It connects only to 0.8.0-pre21.

**Community:** find players, get help and talk about the mod on the [HSMMod Discord](https://discord.gg/QZcSFnwxra).

## Contents

- [Install](#install)
- [Features](#features)
- [Settings](#settings)
- [Known limitations](#known-limitations)
- [Bug reports and privacy](#bug-reports-and-privacy)
- [Update check](#update-check)
- [Build from source](#build-from-source-linux-no-msvc)
- [License](#license) and [credits](#credits)

## Install

1. Download `dwmapi.dll` from the [latest release](../../releases/latest).
2. Copy it into `<Half Sword>/HalfswordUE5/Binaries/Win64/`.
3. **Linux / Steam Deck (Proton) only:** Proton loads its own built-in `dwmapi.dll` unless told otherwise. In Steam,
   right-click Half Sword → Properties → General → Launch Options and set:
   ```
   WINEDLLOVERRIDES="dwmapi=n,b" %command%
   ```
4. **UE4SS (optional).** HSMMod loads UE4SS itself, so UE4SS mods keep working next to it:
   1. Install UE4SS as usual (its `dwmapi.dll` goes next to the game exe, the rest in a `ue4ss` folder).
   2. Copy HSMMod's `dwmapi.dll` over UE4SS's `dwmapi.dll` (HSMMod becomes the proxy and loads `ue4ss/UE4SS.dll`).
   3. Updating UE4SS later overwrites HSMMod's `dwmapi.dll` again: copy ours back. The multiplayer page warns when the
      `dwmapi.dll` next to the exe is not HSMMod's.

   UE4SS mods are never synced or sent: each player installs their own. Bug reports carry the UE4SS version and the
   names of the enabled UE4SS mods. An older UE4SS 2.x install (proxy `xinput1_3.dll`) keeps working without any of
   this.
5. Start the game and click **MULTIPLAYER** on the main menu. `HSMMod.ini` is created next to the DLL on first run.

Every player needs the same mod version and the same game build (Steam build 24185754). On any other game build the
mod logs "unsupported build" and does nothing.

## Features

### Playing together

- **Steam lobbies:** host for friends or in public, invite through the Steam overlay, join from the friends list.
  Traffic goes through Steam's relay network, so no port forwarding is needed.
- **Up to 8 players** with their own loadouts, carried over from each player's game, plus bots.
- **Match modes**, scores, chat and spectating with a follow camera behind the fighter you watch. **Brawl** is
  free-for-all with bare fists: everyone spawns naked with no weapons, and your own gear is untouched afterwards.
- **Name tags** over other fighters, hidden behind walls, in team colours in team modes.
- **Hold G to give up** in FFA, team matches and Duel: you die and the last player who hit you in the past 60 s gets
  the kill.
- **Host migration:** if the host of a Steam lobby leaves, another player takes over the lobby and reopens the map,
  and everyone else follows (scores start over).

### Combat sync

- **Owner sync:** your own machine simulates your fighter, so your body has no added latency. Other players are
  physics stand-ins driven by a 60 Hz pose stream.
- **Hits through the game's own damage code:** the attacker claims a hit and the victim's machine replays it on its own
  fighter, so armour, wounds, bleeding, knockouts and deaths work as in single player.
- **Gore, grabs, dropped weapons and level props:** severed limbs, stuck blades, dislocations, grabbing and carrying
  another player (alive or down), loose weapons everyone sees in the same place, and the level's barrels, stools and
  other loose props.
- **Fair grabs:** a player held by another can pull free; from 0.7.3 a hold is about as strong as the game's own grab,
  not an iron grip.
- **Stripping downed players:** a downed player's armour and clothes come off as on a downed NPC: grab the piece and
  press grab again.
- **Loose objects (from 0.8.0-pre1):** anything that starts moving mid-match (a thrown helmet, armour knocked off, a
  bridge pulled down, a snapped plank) gets the same name on every machine, each of its parts moves the same
  everywhere, and a joint that breaks on one screen breaks on all of them.
- **Shattering (from 0.8.0-pre2):** planks, crates and other breakables break the same way on every screen.

### Voice chat

Off by default. The host turns it on under VOICE CHAT on the multiplayer page:

- **Proximity:** you hear players near you, from the side they stand on, fading out by 30 m.
- **Everyone:** every living player hears every living one.

Each player sets up their microphone there or in the Esc menu: push to talk (`V` by default; click CHANGE to pick
another key), open mic with a threshold, or muted; the recording device; voice volume; and HEAR MYSELF with a level
meter. A badly hurt, knocked-out or dead fighter goes quiet, and the dead talk only to each other. A `> ` before a name
tag shows who is talking. Steam must be running.

### Esc menu and moderation

- **Multiplayer Esc menu:** Esc in a session match opens PLAYERS (mute a player or set their volume; the host can kick
  or ban, everyone else can start a vote kick and vote YES or NO), VOICE (your microphone) and NETWORK (role, ping to
  the host, direct links). GAME MENU opens the game's own pause menu, and MULTIPLAYER MENU there brings you back.
  RESUME, REPORT A BUG and LEAVE SESSION are at the bottom.
- **Chat commands for the host:** `/kick <name>`, `/ban <name> [30m|2h|7d|perm]`, `/players`, `/bans`, `/unban`.
- **Vote kick:** `/votekick <name>`, then `/yes` or `/no` (the host votes too). A passed vote keeps the player out for
  15 minutes. On a `role=server` host players can also start a vote ban (VOTE BAN or `/voteban <name>`): it needs two
  thirds of the votes and bans the player for 7 days.

### Experimental

- **Direct client-to-client movement (on by default):** players in a Steam lobby send their movement, blows and other
  fight events straight to each other instead of through the host. The host relays again by itself if a direct path
  fails.
- **Detours (on by default):** the host picks a faster path for each pair of players from their measured pings. When A
  to C through B is quicker than A to C directly, B passes A's movement on as well. Only A and C can make or check
  those packets.
- **Hosting without playing:** `role=server` runs matches and rotates maps with no fighter of its own; players spawn at
  the map's start points. It runs a normal copy of the game and opens a public Steam lobby that players find in the
  game's lobby browser.
- **Modded maps:** put a custom map's `.pak` file in `HalfSwordUE5\Content\Paks\~mods` (create the folder if needed)
  and restart the game. The host's map picker lists the map after the stock ones, marked as modded. Everyone who joins
  needs the same pak file in their own `~mods` folder: a player without it (or with a different version) is told which
  pak to install and goes back to the menu. HSMMod never sends or downloads pak files.

### Crashes

If the game crashes, the last lines of `HSMMod.log` name the fault, and a small crash dump, `HSMMod.crash.dmp`, is
written next to the log in the game's `Binaries\Win64` folder. Please attach both, plus the newest folder under
`%LOCALAPPDATA%\HalfSwordUE5\Saved\Crashes\`, to an issue, or share them on the
[Discord](https://discord.gg/QZcSFnwxra).

## Settings

Settings live in `HSMMod.ini`, next to the DLL, under `[net]` unless noted. The defaults are fine for most players.

| Setting | Default | What it does |
| --- | --- | --- |
| `host_migration` | `1` | `0`: the session ends when the host leaves. |
| `name_tags` | `1` | `0`: no name tags over other fighters. |
| `mesh` | `1` | `0`: send everything through the host instead of directly between players. |
| `detour` | `1` | `0`: no detours through other players. |
| `votekick` | `1` | `0` (host): turn vote kicks and vote bans off. |
| `help_improve` | `0` | `1`: same as HELP IMPROVE THE MOD on the multiplayer page (see below). |
| `help_improve_asked` | `0` | `1`: the one-time HELP IMPROVE THE MOD question was answered; it is not asked again. |
| `reports` | `on` | `off`: no bug reports, log uploads or crash reports at all. |
| `update_check` | `1` | `0`: no update check. |
| `ue4ss` (`[mod]`) | `1` | `0`: do not load UE4SS. |
| `ue4ss_order` (`[mod]`) | `after` | `before`: load UE4SS before HSMMod's own hooks. |

## Known limitations

- **Time of day follows the map.** You cannot pick day or night in a lobby yet; a lobby setting is planned.
- **Brawl:** weapons on the level's own racks and floors can still be picked up.
- **Direct IP was removed in 0.8.0.** Play through Steam lobbies; an old `role=host` or `role=client` in `HSMMod.ini`
  is ignored.
- Loadouts with no fabric armour show no team colour.

## Bug reports and privacy

Found a bug? Open the chat and type `/bug <what went wrong>`, or use REPORT A BUG on the multiplayer page or on the
pause menu in a session fight (it opens the chat with `/bug ` typed). You see "Sending...", then the issue link once
it is filed, or a short reason if it was not. Other players in the session are asked whether to add their logs
(`/sendlog` or `/nolog`).

`/bug` sends your text, the mod and game version, the match setup and your OS version to the report server, which
opens a public issue in this repository with those details. The end of your `HSMMod.log` goes along but is stored
privately with the maintainers, never on the issue. Before it leaves your PC, Steam IDs and IP addresses (IPv4 and
IPv6) in the log are replaced with one-off hashes and your Windows user name is removed from file paths. **Player
names stay in the log.** You can file one report every 30 seconds.

Apart from the update check below, the mod makes no web request unless you type `/bug` or `/sendlog`, or turn on
**HELP IMPROVE THE MOD**. It is **off unless you turn it on.** The first time you click MULTIPLAYER, the mod asks you
once: YES turns it on, NO THANKS (or Esc) leaves it off, and it never asks again. You can change it any time with the
HELP IMPROVE THE MOD button at the top of the multiplayer page or on the Esc menu in a session fight.

With it on:
- your log (redacted the same way) goes to the report server by itself when someone in your lobby files a report;
- if your game crashed, the next start files a crash report: its public text is the crash line only (error code and
  code offset), and your logs stay private;
- your log also gets a short combat summary every few seconds (which weapon struck whom and what the mod did with each
  blow), so missed hits like the flail's can be tracked down. It stays in your log and is only sent as above.

With it off and your game crashed, the mod says so once in the chat and points you to the button. Nothing is sent.
`reports=off` turns all of it off.

## Update check

Each time the game starts, the mod makes one HTTPS request to `api.github.com` to read this repository's list of
releases, and tells you (in `HSMMod.log`, on the multiplayer page and once in the chat of your first fight) if a newer
release exists. The request carries only a `User-Agent: HSMMod/<version>` header, no IDs and no names, and nothing is
downloaded or installed. If you run a prerelease you are told about prereleases and full releases, otherwise only
about full releases.

## Build from source (Linux, no MSVC)

Needs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (set `LLVM_MINGW` to its folder) and, for the
tests, a host `g++`/`clang++` with ASan/UBSan.

```bash
implementation/client/HSMMod/build.sh            # -> implementation/client/HSMMod/build/dwmapi.dll
implementation/shared/run_tests.sh               # game-independent logic under ASan/UBSan
implementation/shared/run_fuzz.sh 300000         # protocol decoder fuzzing
```

`implementation/client/HSMMod` is the DLL (engine hooks, gameplay, transport, UI);
`implementation/shared` is the game-independent, unit-tested logic it builds on.
See [implementation/client/HSMMod/README.md](implementation/client/HSMMod/README.md) for details.

## License

HSMMod is released under the [MIT License](LICENSE). Share builds and modified versions freely; keep the copyright and
license notice.

## Credits

### Contributors

- [Dudiebug](https://github.com/Dudiebug): author and maintainer.
- [CSLewin](https://github.com/CSLewin) (Craig Chouraki-Lewin): Steamworks 1.57 struct layouts, Steam networking and
  lobby fixes, and no combat outside the live match phase
  ([#1](https://github.com/Dudiebug/halfsword-multiplayer-mod/pull/1)).

### Third-party code

Parts of the sync design are ported from [Cyrex0/HalfSword-MP](https://github.com/Cyrex0/HalfSword-MP),
Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT. MinHook is vendored under its own license. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Half Sword is a game by Half Sword Games. This is an unofficial fan mod, not affiliated with or endorsed by them.
