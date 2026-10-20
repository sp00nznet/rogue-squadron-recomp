# Bring-up

Every wall so far, in the order it was hit, and what got past it.

## 1. Unwrap, catalog, lift

pcrecomp's `steamstub.py` already handled SteamStub 3.x (added for Shadows of
the Empire, pcrecomp#110), and this exe came apart on the first try:

```
SteamStub 3.x: app 455910, header 0x007FF220
  .text 0x00401000+0x1BE000 decrypted (AES-256-CBC); OEP 0x005A2200
```

The catalog took about 10 minutes (13,327 functions, 98.2% of `.text`), and
the lift about 5:

```
py -3 run_lift.py --all
  lifted 13330   not-lifted stubs 0   errors 0   no terminator 285
  3,667,538 lines of C in 134 files, 283.4s; 137 changed
```

## 2. The display

The game takes the screen as shipped: exclusive DirectDraw, a mode change, a
flip chain. Unlike Shadows of the Empire, whose Steam release brings a
Direct3D 9 wrapper that gave the host one seam (its `Direct3DCreate9`), this
one gets Windows' own ddraw.dll. So the seam is the DirectDraw objects:
`display.c` binds `DirectDrawCreate` and patches the vtable of every DirectDraw
and surface interface the game gets. The cooperative level is always normal,
the mode change is only remembered, and the primary and its back buffer are
offscreen surfaces of the mode's size and format; Flip reads the back buffer,
records it, and draws it into the window.

### 2a. Windows rewrites DirectDraw's vtables

The first runs reached exclusive fullscreen anyway. The third DirectDraw object
the game made called the real `SetCooperativeLevel(0xC11)` (fullscreen,
exclusive, multithreaded, FPU setup), past the hook:

```
[ddraw] DirectDraw v1 object 0D96BCB8: vtable 7231D3C0 (DDRAW.dll), slot 0 in apphelp.dll
...
[native] ?  (014BDF50 003A006E 00000C11 00000000) from sub_005812C0 @74AB1400
```

Windows' Program Compatibility Assistant had put the `DWM8And16BitMitigation`
layer on `rogue.exe` (HKCU and HKLM `AppCompatFlags\Layers`) after the first
run, and that layer's `apphelp.dll` writes its own entries into DirectDraw's
vtables at every `DirectDrawCreate`, putting the originals back over a hook. So
a hook is re-checked and re-applied on every object (`vt_hook`), and as a
backstop every loaded DLL's import of `ChangeDisplaySettings*` refuses: a mode
change cannot reach the screen whatever path it takes.

### 2b. The primary is the last frame

A real flip makes the frame the primary's, and the game draws 2D onto the
primary over it (`sub_004E78F4` locks it: fades, the loading screen, the front
end). The fake Flip copies the back buffer onto the primary, and a Lock/Unlock,
Blt, BltFast or GDI on the primary presents it too.

## 3. Input

DirectInput 3, keyboard and mouse at `FOREGROUND | NONEXCLUSIVE`. Headless,
Acquire succeeds and a failed read is an idle device (`input.c`). The front
end reads its keys from window messages, not DirectInput: the window procedure
(`sub_00587670`) queues WM_KEYDOWN/WM_KEYUP scan codes, and `ToAscii` turns
them into text. `--key DIK@T[+D]` does both.

## 4. The registry, one key at a time

After the intro (logos, the trench flyover) the screen stayed black: the front
end was running, polling input, writing nothing but black. The game reads its
settings by opening `Software`, then `LucasArts Entertainment Company LLC`,
then `Rogue Squadron`, then `v1.0`, each under the last, so the host's redirect
of the full HKLM path to HKCU never matched, and every read failed:

```
RS_REG_LOG=1
[reg] open Software -> 0
[reg] open LucasArts Entertainment Company LLC -> 0
[reg] open Rogue Squadron -> 2
```

Without `Source Dir` the front end drew nothing. Now a key opened as
HKLM\Software is remembered and a LucasArts key under it opens from HKCU; the
title screen and the pilot roster come up.

`--original` (the shipping machine code under the same host) stalled at the
same place, which said it was the environment and not the lift.

## 5. Into a mission

With the registry read, nothing else stood in the way. A scripted run clicks
Start on the pilot roster (`--mouse 568,107@54 --click 0@56`) and presses Enter
through Select Level and Select Your Craft; the launch cutscene, the briefing
and the flight over Nonnah follow, with the radar and the HUD:

```
build-farm\rogue.exe --run --headless --mute --mouse 568,107@54 --click 0@56 ^
    --key 0x1C@66 --key 0x1C@80 --key 0x1C@95 --record r.mp4 --watchdog 160
```

`docs/screenshots/` has stills from that run.
