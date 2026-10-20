# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- F10 menu: infinite lives, complete this mission, abort this mission
  (`--cmd lives`, `win`, `abort`), from the addresses in a speedrunner's
  Cheat Engine table, checked against the lift. `docs/menu.md`.

## [0.1.0-alpha.1] - 2026-10-19

### Added
- The Steam `Rogue Squadron.EXE` (v1.3) lifts whole with pcrecomp: 13,331
  functions, 0 errors, about 3.7 million lines of C, after SteamStub 3.x is
  removed statically. `docs/RECON.md`.
- A 32-bit host on pcrecomp's `native32` runs it: the intro, the front end
  (pilot roster, settings, level and craft select) and the missions.
- The display layer (`src/runtime/display.c`): DirectDraw is never exclusive
  and never changes the mode; the flip chain is emulated with offscreen
  surfaces and drawn into the game's window, borderless fullscreen or a window
  (F11, Alt+Enter); a backstop refuses any display mode change.
- The input layer (`src/runtime/input.c`): headless devices; scripted input
  (`--key`, `--mouse`, `--click`).
- The F10 menu (`src/runtime/menu.c`, `docs/menu.md`): the game's passcodes
  held on (Millennium Falcon, TIE Interceptor, all bonus missions, ace mode),
  kept in `rogue.ini`; `--cmd NAME@T` for headless runs.
- The game's settings live under HKCU, so nothing needs an administrator.
- `--headless --record out.mp4 --frames N`, `--mute`, `--watchdog`,
  `--original`, `--trace-from`; `RS_DRAW_LOG`, `RS_REG_LOG`, `RS_PROFILE`.
- `Setup.cmd` quick start, and the installer (recomp-launcher with
  `launcher/launcher.json`, built by `.github/workflows/installer.yml`).

[Unreleased]: https://github.com/sp00nznet/rogue-squadron-recomp/compare/v0.1.0-alpha.1...HEAD
[0.1.0-alpha.1]: https://github.com/sp00nznet/rogue-squadron-recomp/releases/tag/v0.1.0-alpha.1
