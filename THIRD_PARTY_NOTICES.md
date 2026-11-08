# Third-party notices

## Contributions by CSLewin

Parts of the Steam and match code come from CSLewin's pull request #1 to Dudiebug/halfsword-multiplayer-mod,
contributed under this project's MIT license (see LICENSE). Thank you, CSLewin.

| File in this repository | From the pull request |
|---|---|
| `implementation/shared/steam/hsm_steam_abi.h` | the Steamworks 1.57 Win64 struct layouts and their size checks |
| `implementation/client/HSMMod/src/steam/steam_api.cpp` | SessionFailed registered at its real size and its end reason logged; the relay-status and Steam-server callbacks; the RunCallbacks pump that takes over whenever the game stops pumping; failed lobby calls (no call handle) reported at once; the typed connection status for the ping |
| `implementation/client/HSMMod/src/gameplay/lobby.{h,cpp}` | ignoring a lobby enter that is not the lobby being joined; following a Steam join request from inside a lobby |
| `implementation/shared/match/hsm_match.h` (`CombatPhase`), `implementation/client/HSMMod/src/gameplay/session.cpp` (`CombatEnabled`) | no blow between session fighters counts before the match is live or after it ended |

## HalfSword-MP (HSMP)

Parts of the multiplayer sync code are ported from, or follow, HalfSword-MP (https://github.com/Cyrex0/HalfSword-MP),
upstream commit `4f3018826bd2f12c105f2146986bb9052febb129`. HSMP is dual-licensed MIT OR Apache-2.0; this project
uses it under the MIT license below.

| File in this repository | Upstream source |
|---|---|
| `implementation/shared/sync/hsm_pose.h` | bone list, parents and bodies of `crates/hsmp-pose/src/posecodec_v2.rs` |
| `implementation/shared/sync/hsm_pose_codec.h` | `crates/hsmp-pose/src/posecodec_v2.rs` (without the control layer) |
| `implementation/shared/sync/hsm_poseplay.h` | `crates/hsmp-pose/src/poseplay.rs` |
| `implementation/shared/sync/test_hsm_poseplay.cpp` | several tests adapted from the `poseplay.rs` test module |
| `implementation/shared/sync/hsm_servo.h` | `crates/hsmp-native/src/servo.rs`, `mods/HSMPAvatars/Scripts/avatars_pure.lua` |
| `implementation/shared/sync/hsm_standin.h` | aim and acceleration estimate of `mods/HSMPAvatars/Scripts/avatars_pure.lua` and `main.lua:4013-4040`; contact bodies and blow detection of `main.lua:3885`, `main.lua:4178-4196` |
| `implementation/client/HSMMod/src/gameplay/standin_rig.{h,cpp}` | the stand-in policy of `mods/HSMPAvatars/Scripts/main.lua` (`neutralise_bp`, grip constraints, `snap_mesh`, `drive_root`, weapon-root gravity/collision), re-implemented on this project's native engine layer |
| `implementation/shared/combat/hsm_claim.h` | claim fields, bone-frame rule and per-bone claim limit of `docs/development/subsystems/combat.md` (re-implemented) |
| `implementation/client/HSMMod/src/gameplay/claims.{h,cpp}` | the replay's reset of `Last Complex Damage Impulse` of `docs/development/subsystems/combat.md` (re-implemented; the hooks are this project's) |
| `implementation/client/HSMMod/src/gameplay/severs.{h,cpp}` | the vitals-driven sever design (the owner decides, stand-ins follow its vitals) of `docs/development/subsystems/vitals.md` (re-implemented; the capture and replay of the game's own dismember calls are this project's) |
| `implementation/shared/sync/hsm_gore.h`, `implementation/client/HSMMod/src/gameplay/standin_gore.{h,cpp}` | the severed-subtree exclusion, the tagged-children hide and "a severed part never regrows" of `mods/HSMPAvatars/Scripts/body_injury.lua` (re-implemented) |
| `implementation/shared/sync/hsm_grab.h`, `implementation/shared/sync/test_hsm_grab.cpp`, `implementation/client/HSMMod/src/gameplay/grab_sync.{h,cpp}` | the grab design and constants of `mods/HSMPInteract/Scripts/main.lua` (`cfg`, `drive_held`: the grip in the held bone's frame, the grabber's hand as the anchor, the lead clamp, the lease and break-free), re-implemented as a per-step velocity change |
| `implementation/shared/session/hsm_lease.h` | the claim lifecycle (touch or hold, release at rest), the rest thresholds and the 20 Hz stream of bodies nobody leases of `mods/HSMPWorld` (`docs/development/subsystems/world-replication.md`), re-implemented |

```
MIT License

Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Vendored libraries

Built into `dwmapi.dll`; each keeps its license file in its directory.

| Library | Directory | License |
|---|---|---|
| MinHook | `implementation/client/HSMMod/third_party/minhook` | BSD 2-Clause (`LICENSE.txt`) |
| miniz 3.1.2 | `implementation/client/HSMMod/third_party/miniz` | MIT (`LICENSE`) |

## UE4SS

`dwmapi.dll` carries an unchanged build of RE-UE4SS (https://github.com/UE4SS-RE/RE-UE4SS),
`implementation/client/HSMMod/third_party/ue4ss/UE4SS_v3.0.1-1217-g4fb9b615.zip`, and unpacks it next to the game. Its
`ue4ss/LICENSE` goes with it:

> MIT License
>
> Copyright (c) 2022 Narknon
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
> documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
> rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
> persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
> Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
> WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
> COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
> OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

UE4SS.dll is built from these projects, under their own licenses:

| Project | License |
|---|---|
| PolyHook 2 (https://github.com/stevemk14ebr/PolyHook_2_0) | MIT |
| Zydis (https://github.com/zyantific/zydis) | MIT |
| Dear ImGui (https://github.com/ocornut/imgui) | MIT |
| {fmt} (https://github.com/fmtlib/fmt) | MIT |
| patternsleuth (https://github.com/trumank/patternsleuth) | MIT OR Apache-2.0 |
| GLFW (https://github.com/glfw/glfw) | Zlib |
| raw_pdb (https://github.com/MolecularMatters/raw_pdb) | BSD 2-Clause |
| Tracy (https://github.com/wolfpld/tracy) | BSD 3-Clause |

## Not used

Half Sword Enhancer's license forbids redistributing any part of it. No code or structure from it is in this
repository; field offsets observed in its dump are listed as facts in `docs/REVERSE_ENGINEERING.md` §5.

## Direct-IP fork dependencies

ENet 1.3.18, commit `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`, by Lee Salzman.
Source: https://github.com/lsalzman/enet

```
Copyright (c) 2002-2024 Lee Salzman

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

```

MiniUPnPc 2.3.3, miniupnp commit `bf4215a7574f88aa55859db9db00e3ae58cf42d6`, by Thomas Bernard and contributors.
Source: https://github.com/miniupnp/miniupnp

```
BSD 3-Clause License

Copyright (c) 2005-2025, Thomas BERNARD
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

```

Historical upstream paths in the tables above describe original contributions; the Steam API/transport implementations have been replaced in this fork.
