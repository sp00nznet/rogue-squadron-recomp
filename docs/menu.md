# The F10 menu

F10 opens a popup on the game's window (`src/runtime/menu.c`). Its settings
are kept in `rogue.ini` beside `rogue.exe`; a headless run never writes it.

| Menu | Item | |
|---|---|---|
| Cheats | Millennium Falcon | passcode `WOOKIEPELT`: the Falcon in the craft select |
| | TIE Interceptor | passcode `GOODGUYTIE` |
| | All bonus missions | passcode `OOMPAWAMPA` |
| | Ace mode | every craft starts at 40 hit points, not 75 |
| Display | Fullscreen, borderless | the default; F11 or Alt+Enter switches |
| | Window | the game's 640x480 scaled to 90% of the screen, aspect kept |

The craft and ace mode apply when a mission builds its craft, so at the next
mission.

## How the cheats work

They are the game's own passcodes, held on. Settings, General, "enter passcode
here" goes to `sub_004291E0`, which hashes what was typed and looks it up in a
table of 35 hashes at `0x005EB300`; a match sets that passcode's bit in
`0x00704248` (passcodes 0 to 31) or `0x0070424C` (32 to 34). The game tests
those bits wherever a cheat matters, and saves them with the pilot. The menu
keeps its bits set from a thread every 100 ms, because choosing a pilot loads
that pilot's own bits over them (`sub_004DB037`).

The hash is CRC-32 (reflected, polynomial `0xEDB88320`), seed `0xFAC5FAC5`,
no final xor, over the code in capitals padded with `_` to 10 bytes. Codes
from fan lists were checked against the table; the Steam release (1.3) has
different codes from the ones usually listed (`IAMDOLLY`, `TOUGHGUY` and
`DIRECTOR` are not in it). What matched:

| Passcode | Bit | What it does |
|---|---|---|
| `KOELSCH` | 1 | tested in eight places; not worked out |
| `WOOKIEPELT` | 2 | the Millennium Falcon |
| `GOODGUYTIE` | 3 | the TIE Interceptor |
| `THOMASHEAD` | 18 | one of twelve pilot-head swaps (bits 7 to 18, `the_head%d`) |
| `JULIANHEAD` | 21 | another head, by its name; bit 21 is tested outside the head code (`sub_0043C56C`, `sub_0044BEC7`), so not certain |
| `OOMPAWAMPA` | 28 | all bonus missions |
| `USEDAFORCE` | 30 | sets `0x00703600`; not worked out |
| `LEIAWRKOUT` | 34 | force feedback |

Bit 6 has no known code; it cuts every craft's hit points from 75 to 40 in
the nine craft set-up functions (`sub_0040BBC0` and its siblings), the "ace"
cheat. The other bits are left alone until what they do is known.

`--cmd falcon@T`, `tie`, `bonus` and `ace` run the same toggles on a timeline.
