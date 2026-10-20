# What is in the Steam install

Measured from the Steam release (app 455910), `E:\SteamLibrary\steamapps\common\RogueSquadron`.

## Files

```
Install.bat                       the compatibility shim and four HKLM values, on first launch
installScript.vdf                 six HKLM path values (executable, Install Path, CD Path, ...)
Manual.pdf
ROGUE\Rogue Squadron.EXE          2,894,784 bytes  the game, SteamStub 3.x
ROGUE\ROGUE.EXE                   1,008,064 bytes  LucasArts' launcher (MFC), SteamStub 3.x
ROGUE\IFORCE2.DLL                 Immersion force feedback, imported by the game
ROGUE\BUNDLE.000-003, DATA\...    the game's data (~65 MB)
ROGUE\Star Wars - Rogue Squadron 3D.sdb   Windows compatibility shims
ROGUE\PlayerProfiles\             saves and high scores (Steam Cloud)
```

The game is version 1.3, the one with the Naboo starfighter (`README.TXT`).

## The executables

| | `Rogue Squadron.EXE` | `ROGUE.EXE` |
|---|---|---|
| PE timestamp | `0x3777F753` (1999-06-28) | `0x37EA66E6` (1999-09-23) |
| `.text` | 0x00401000 + 0x1BDF99, AES ciphertext | 0x00401000 + 0x4B53C |
| OEP after unwrapping | `0x005A2200` | `0x00410C31` |
| image end | `0x00862000` (with `.bind`) | |

Both unwrap with pcrecomp's `tools/drm/steamstub.py`:

```
SteamStub 3.x: app 455910, header 0x007FF220
  .text 0x00401000+0x1BE000 decrypted (AES-256-CBC); OEP 0x005A2200
```

The launcher only starts the game; the recomp runs the game directly.

## Imports of the game

- **DDRAW**: `DirectDrawCreate`, `DirectDrawEnumerateExA`. The renderer is
  Direct3D on DirectDraw (DirectX 6), with Glide as the other choice (loaded by
  name, not imported; no Glide DLL ships). No Direct3D wrapper ships, unlike
  Shadows of the Empire: the game gets Windows' own ddraw.dll.
- **DINPUT** `DirectInputCreateA`, **DSOUND** by ordinal (1 `DirectSoundCreate`,
  2 `DirectSoundEnumerateA`), **IFORCE2** (four force-feedback calls).
- **USER32**: 27 functions, a `GetMessageA` loop and no dialogs. **KERNEL32**:
  threads (`CreateThread`, `SuspendThread`, `ResumeThread`, `TerminateThread`),
  events, semaphores, and a multimedia timer (`timeSetEvent`).

## The compatibility shims

The `.sdb` that `Install.bat` installs applies two Windows shims to
`Rogue Squadron.EXE`: `EmulateEnvironmentBlock` (the C runtime's copy of a
large environment block overflows) and `IgnoreAltTab`.

## The registry

`HKLM\SOFTWARE\LucasArts Entertainment Company LLC\Rogue Squadron\v1.0`:
`executable`, `Install Path`, `Source Path`, `CD Path`, `Analyze Path`,
`Source Dir` (strings, from `installScript.vdf`) and `JoystickID` "1",
`SoundCard` "TRUE", `InstallType` 9, `Magic` 600124 (from `Install.bat`).
The recomp keeps them under HKCU (host.c, `reg_root`).

## The catalog

```
py -3 ..\tools\tools\disasm\disasm32.py game\ROGUE\Rogue.unwrapped.exe -o work\functions.json
[*] Functions: 13327  (thunks=6, leaves=4726)
[*] Instructions: 2,763,097
[*] Byte coverage: 1,794,263 / 1,826,816 (98.2% of code range)
```

About five times Shadows of the Empire's 2,806 functions.
