#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* What the F10 menu sets, kept in rogue.ini beside rogue.exe (docs/menu.md). */
typedef struct {
    int window;          /* 0 borderless fullscreen, 1 a window */
    uint32_t cheats;     /* passcode bits held on (bit n = the game's passcode n) */
} rs_settings_t;
extern rs_settings_t g_set;

void menu_init(int headless);
int  menu_arg(int argc, char** argv, int i);     /* args consumed, 0 if not ours */
/* the game window's messages: 1 if handled (*r is the result) */
int  menu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* r);
