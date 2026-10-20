# Roadmap

## Next

- **Play it on a real screen, with a real pad.** Everything so far ran
  headless under scripted input. The window, F10, F11 and a controller (the
  game's own DirectInput joystick support) have not been seen on a monitor.
- **Resolution and widescreen.** The emulated flip chain takes any size, but
  the game's Video settings page lists nothing (it builds its list from
  DirectDraw's enumeration), and the 3D projection is 4:3. The mode is set by
  `sub_00588070` → `sub_0056EDD0` → `sub_0057BBF0` from a mode record; the
  front end draws 640x480 2D.
- **Invincibility.** Infinite lives and completing a mission are in the F10
  menu; damage is not. A craft's hit points are at +0xC0 of its object
  (`sub_0040BBC0`), so the place that subtracts damage is the way in.
- **An intermittent fault at the pilot roster**: one run in four headless
  ones died at 54 s, an indirect call through a null pointer from
  `0x00597280` and then a fault in `sub_00569850`.
- **Play through.** Only The Search for the Nonnah has flown.
- **Sound.** Runs so far were muted; DirectSound and the music are untested.
- **A conformance harness**: boot, roster, level select, a mission, counted
  from the log; and a netlab `QA` line for it.
- **The other passcodes** (docs/menu.md): `KOELSCH`, `USEDAFORCE` and the
  bits without a known code.

## Toolkit (pcrecomp)

- `disasm32` decoded a jump table at `0x004C4152` as code and swallowed the
  function after it (`0x004C418F`); a run found it (`seed_from_log.py`). The
  catalog should stop a body at a table it has already found.
- The vtable-hooking and IAT-patching helpers here and in Shadows of the
  Empire's display layer are the same idea twice; they belong in
  `runtime/native32`.

## Later

- Mouse steering and modern pad defaults, as Shadows of the Empire has.
- The Glide renderer (`glide2x.dll`, loaded by name) is untouched; Direct3D
  is the path.
