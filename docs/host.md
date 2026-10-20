# The host

`rogue.exe` is a 32-bit Windows program on pcrecomp's `runtime/native32`. It
maps `game\ROGUE\Rogue.unwrapped.exe` at its own base (0x00400000; the host is
linked at 0x60000000 to leave room), binds the guest's imports to Windows,
except the shimmed ones, and calls the entry point. Every guest function is
lifted C; a call into Windows goes through native32's bridge, and Windows'
calls back into the game (window procedure, timers, threads) fault on the
guest's non-executable code and land in the lifted body.

| File | What it owns |
|---|---|
| `src/runtime/host.c` | the command line, the guest's identity (module name, command line), the registry, headless windows, the fault report, the watchdog and profiler |
| `src/runtime/display.c` | DirectDraw, steered: no exclusive mode, no mode change, the emulated flip chain, presenting, recording, the window placement |
| `src/runtime/input.c` | DirectInput, steered: headless devices, scripted keys and mouse |
| `src/runtime/menu.c` | the F10 menu (docs/menu.md) |
| `src/runtime/oracle.c` | `--original`: the shipping machine code under the same host |
| `run_lift.py` | the lift: `work/functions.json` to `src/recomp/gen/` |

## The display

Windows' own ddraw.dll, DirectX 6: the game asks for `DDSCL_FULLSCREEN |
DDSCL_EXCLUSIVE`, sets 640x480x16 and makes a primary with one back buffer.
`display.c` hooks every DirectDraw and surface vtable the game gets
(bringup.md 2):

- `SetCooperativeLevel` is always `DDSCL_NORMAL`; `SetDisplayMode` is only
  remembered and `GetDisplayMode` answers with it.
- The primary and the back buffer are offscreen surfaces of the mode's size
  and format (16-bit 565), the back one a Direct3D render target in video
  memory. `GetAttachedSurface` on the primary gives the back buffer.
- `Flip` copies the back buffer onto the primary and presents it; a Lock, Blt,
  BltFast or GDI on the primary presents it too (the front end draws there).
- Presenting reads the primary back, converts it to 32-bit, records it with
  `--record`, and on a real screen draws it into the game's window with
  `StretchDIBits`, aspect kept, black bars.
- A backstop: every loaded DLL's import of `ChangeDisplaySettings*` refuses,
  so nothing can change the desktop's mode.

## The registry

The game opens `HKLM\Software`, then `LucasArts Entertainment Company LLC`,
`Rogue Squadron` and `v1.0` under it, one at a time. A handle opened as
HKLM\Software is remembered, and a LucasArts key under it opens from HKCU,
where `tools\setup.ps1` writes the values Steam's `Install.bat` and
`installScript.vdf` would write to HKLM (bringup.md 4).

## Headless

`--headless`: the game's window is real but invisible (layered at alpha 0,
click-through, never activated), so the game runs as if it were on screen and
nothing appears. Input devices are idle; `--key`, `--mouse` and `--click` are
the only input. Over RDP it takes no screen (REPO_RULES 13).

## Command line

```
rogue [--run] [--original] [--headless [--record out.mp4] [--frames N]] [--mute] [--game DIR]
      [--key DIK@T[+D]] [--mouse X,Y@T] [--click B@T[+D]] [--cmd NAME@T]
      [--watchdog S] [--native-trace] [--trace-from S] [--callbacks] [-- game args]
```

| Switch | |
|---|---|
| `--run` | enter the game (without it: map and bind, then stop) |
| `--headless` | no window on screen |
| `--record F --frames N` | a video at 30 fps of the run's own clock; stop after N frames |
| `--mute` | this process's audio at zero |
| `--key DIK@T[+D]` | hold a DirectInput key code (scan code) from T seconds for D (0.2); both in DirectInput and as window messages, where the front end reads keys |
| `--mouse X,Y@T` | the cursor to X,Y (640x480) at T |
| `--click B@T[+D]` | mouse button B (0 left) held from T |
| `--cmd NAME@T` | an F10 menu command: `falcon`, `tie`, `bonus`, `ace` |
| `--original` | the shipping machine code, natively, under the same shims |
| `--watchdog S` | after S seconds: where it is, the last calls, then exit |
| `--native-trace`, `--trace-from S` | every call into Windows, from the start or from S seconds |

Debug switches (environment): `RS_DRAW_LOG=1` (surface traffic, present
sources, a frame meter), `RS_REG_LOG=1` (registry), `RS_PROFILE=1` (hot lifted
functions at the watchdog). A trace build (`netlab build rogue --slot trace --
-DRS_TRACE=ON`) adds pcrecomp's `--calltrace`, `--firsthit`, `--argtrace`, and
the last 1,024 function entries at the watchdog.
