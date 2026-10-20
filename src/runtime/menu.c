/*
 * The F10 menu: cheats and display, around the game.
 *
 * Rogue Squadron's cheats are its passcodes (Settings, General, "enter
 * passcode"): sub_004291E0 hashes what was typed (CRC-32, seed 0xFAC5FAC5,
 * upper case padded with '_' to 10 characters) against 35 hashes at
 * 0x005EB300, and a match sets that passcode's bit in 0x00704248 (0..31) or
 * 0x0070424C (32..34); the game tests the bits wherever a cheat matters, and
 * saves them with the pilot. The menu holds bits on instead of typing, so a
 * cheat is the game's own. Only passcodes whose name and effect are known are
 * listed (docs/menu.md has the table and how the names were checked).
 *
 * The menu is a Win32 popup on the game's window, as in the other pcrecomp
 * ports. --cmd runs the same commands on a timeline, for headless checks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "display.h"
#include "menu.h"

#define CHEAT_BITS 0x00704248u      /* u32: passcodes 0..31 entered */

static const struct { int bit; const char* name; const char* code; } k_cheats[] = {
    { 2, "Millennium Falcon", "WOOKIEPELT" },
    { 3, "TIE Interceptor", "GOODGUYTIE" },
    { 28, "All bonus missions", "OOMPAWAMPA" },
    { 6, "Ace mode: every craft at 40 hit points, not 75", "" },
};
#define NCHEATS (int)(sizeof k_cheats / sizeof k_cheats[0])

rs_settings_t g_set = { 0, 0 };
static int g_headless;
static char g_ini[MAX_PATH];

/* ---- cheats ---------------------------------------------------------------- *
 * Held on from a thread: the game loads the pilot's own bits over them when a
 * pilot is chosen (sub_004DB037), and a craft's cheats are read when the
 * mission builds it, so they are put back every 100 ms. Turning one off clears
 * it once. */
static volatile LONG g_clear;          /* bits to clear once */

static DWORD WINAPI cheat_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        uint32_t clear = (uint32_t)InterlockedExchange(&g_clear, 0);
        uint32_t v = MEM32(CHEAT_BITS), want = (v & ~clear) | g_set.cheats;
        if (want != v) MEM32(CHEAT_BITS) = want;
        Sleep(100);
    }
}

static void toggle_cheat(int i) {
    uint32_t bit = 1u << k_cheats[i].bit;
    g_set.cheats ^= bit;
    if (!(g_set.cheats & bit)) InterlockedOr(&g_clear, (LONG)bit);
    fprintf(stderr, "[menu] %s %s\n", k_cheats[i].name, g_set.cheats & bit ? "on" : "off");
}

/* ---- settings (rogue.ini) -------------------------------------------------- */
static const char* const k_window[] = { "borderless", "window" };

static void menu_save(void) {
    char b[32];
    if (g_headless) return;                    /* a test run leaves the player's settings alone */
    WritePrivateProfileStringA("display", "window", k_window[g_set.window], g_ini);
    for (int i = 0; i < NCHEATS; i++) {
        _snprintf(b, sizeof b - 1, "passcode_%d", k_cheats[i].bit), b[sizeof b - 1] = 0;
        WritePrivateProfileStringA("cheats", b, g_set.cheats >> k_cheats[i].bit & 1 ? "1" : "0", g_ini);
    }
}

static void load(void) {
    char b[32], *slash;
    GetModuleFileNameA(NULL, g_ini, sizeof g_ini);
    slash = strrchr(g_ini, '\\');
    strcpy(slash ? slash + 1 : g_ini, "rogue.ini");
    GetPrivateProfileStringA("display", "window", "borderless", b, sizeof b, g_ini);
    for (int i = 0; i < 2; i++)
        if (!_stricmp(b, k_window[i])) g_set.window = i;
    for (int i = 0; i < NCHEATS; i++) {
        _snprintf(b, sizeof b - 1, "passcode_%d", k_cheats[i].bit), b[sizeof b - 1] = 0;
        if (GetPrivateProfileIntA("cheats", b, 0, g_ini)) g_set.cheats |= 1u << k_cheats[i].bit;
    }
}

/* ---- the popup ------------------------------------------------------------- */
enum { ID_CHEAT = 100, ID_WINDOW = 200 };

static void add(HMENU m, UINT id, const char* text, int checked, int enabled) {
    AppendMenuA(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text);
}

static void run(int cmd) {
    if (cmd >= ID_CHEAT && cmd < ID_CHEAT + NCHEATS) toggle_cheat(cmd - ID_CHEAT);
    else if (cmd >= ID_WINDOW && cmd < ID_WINDOW + 2) {
        g_set.window = cmd - ID_WINDOW;
        display_place();
    }
    if (cmd) menu_save();
}

static void show(HWND h) {
    HMENU m = CreatePopupMenu(), ch = CreatePopupMenu(), dp = CreatePopupMenu();
    char b[128];
    POINT at;

    for (int i = 0; i < NCHEATS; i++) {
        _snprintf(b, sizeof b - 1, "%s%s%s%s", k_cheats[i].name, k_cheats[i].code[0] ? " (" : "", k_cheats[i].code,
                  k_cheats[i].code[0] ? ")" : "");
        b[sizeof b - 1] = 0;
        add(ch, ID_CHEAT + i, b, g_set.cheats >> k_cheats[i].bit & 1, 1);
    }
    AppendMenuA(ch, MF_SEPARATOR, 0, NULL);
    add(ch, 0, "Craft and ace mode take effect at the next mission", 0, 0);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)ch, "Cheats");

    add(dp, ID_WINDOW + 0, "Fullscreen, borderless\tF11", g_set.window == 0, 1);
    add(dp, ID_WINDOW + 1, "Window\tF11", g_set.window == 1, 1);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)dp, "Display");

    /* the game hides the cursor: shown for the menu */
    int shown = 0;
    while (ShowCursor(TRUE) < 0) shown++;
    shown++;
    ClipCursor(NULL);
    GetCursorPos(&at);
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, h, NULL);
    PostMessageA(h, WM_NULL, 0, 0);
    DestroyMenu(m);
    while (shown--) ShowCursor(FALSE);
    /* the key or click that chose stays down a moment: not the game's */
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 500;) {
        if (!((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
               GetAsyncKeyState(VK_ESCAPE)) & 0x8000)) break;
        Sleep(10);
    }
    run(cmd);
}

int menu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* r) {
    *r = 0;
    if (g_headless) return 0;
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_F10) { show(h); return 1; }                 /* F10 arrives as a system key */
        if (w == VK_F11 || (w == VK_RETURN && (l & (1 << 29)))) {
            g_set.window = !g_set.window;
            display_place();
            menu_save();
            return 1;
        }
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        return w == VK_F10 || w == VK_F11;                      /* not the window menu */
    }
    return 0;
}

/* ---- --cmd NAME@T: the menu's commands on a timeline ----------------------- */
#define MAX_CMDS 32
static struct { int id; double t; } g_script[MAX_CMDS];
static int g_nscript;

static DWORD WINAPI script_thread(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_nscript; i++) {
        while ((GetTickCount() - t0) / 1000.0 < g_script[i].t) Sleep(20);
        run(g_script[i].id);
    }
    return 0;
}

int menu_arg(int argc, char** argv, int i) {
    static const struct { const char* name; int id; } names[] = {
        { "falcon", ID_CHEAT + 0 }, { "tie", ID_CHEAT + 1 }, { "bonus", ID_CHEAT + 2 }, { "ace", ID_CHEAT + 3 },
    };
    if (strcmp(argv[i], "--cmd") || i + 1 >= argc) return 0;
    char name[32];
    const char* at = strchr(argv[i + 1], '@');
    int id = -1, n = at ? (int)(at - argv[i + 1]) : 0;
    if (!at || n >= (int)sizeof name || g_nscript == MAX_CMDS) { fprintf(stderr, "--cmd NAME@SECONDS\n"); exit(1); }
    memcpy(name, argv[i + 1], n), name[n] = 0;
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
        if (!strcmp(name, names[k].name)) id = names[k].id;
    if (id < 0) { fprintf(stderr, "--cmd %s: unknown (falcon, tie, bonus, ace)\n", name); exit(1); }
    g_script[g_nscript].id = id, g_script[g_nscript++].t = atof(at + 1);
    return 2;
}

void menu_init(int headless) {
    g_headless = headless;
    load();
    fprintf(stderr, "[menu] F10; %s, cheats 0x%08X (%s)\n", k_window[g_set.window], g_set.cheats, g_ini);
    CloseHandle(CreateThread(NULL, 0, cheat_thread, NULL, 0, NULL));
    if (g_nscript) CloseHandle(CreateThread(NULL, 0, script_thread, NULL, 0, NULL));
}
