The UE4SS build HSMMod carries: `UE4SS_v3.0.1-1217-g4fb9b615.zip` from
https://github.com/UE4SS-RE/RE-UE4SS/releases/tag/experimental-latest, unchanged (MIT, its `ue4ss/LICENSE` is inside).
`src/ue4ss_bundle.cpp` embeds it. UE4SS's own `dwmapi.dll` in the zip is never unpacked.

To move to a newer build: replace the zip, change the file name in `src/ue4ss_bundle.cpp`, set `kBundledUe4ss` in
`shared/match/hsm_dll_rules.h` and add the hash to `kKnownUe4ss`, then run `go generate .` in `launcher/`.
`tests/test_ue4ss_bundle.py` checks the zip against the header. Keep the DLL under the updater's cap
(`kMaxUpdateDllBytes`): older clients refuse a bigger one.
