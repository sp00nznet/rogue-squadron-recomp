#pragma once
#include <stdint.h>

/* The game's DirectDraw, steered (display.c): never exclusive, never a mode
 * change; the flip chain is offscreen surfaces, and each Flip is presented
 * into the game's window (or, headless, only counted and recorded). With
 * `record` set each frame goes to ffmpeg, and the run exits after `frames`
 * recorded frames (0: never). 1 on success. */
int display_init(const char* rdir, int headless, const char* record, long frames);
void display_shim_DirectDrawCreate(void);      /* native32 shim for DDRAW!DirectDrawCreate */
long display_frames(void);
void* display_hwnd(void);                      /* the game's window, once DirectDraw has it */
void display_record_close(void);
void display_place(void);                      /* the game window as the F10 menu says: borderless or a window */
