/*
 * DirectInput, steered.
 *
 * The game reads the keyboard, the mouse and a joystick through DirectInput 3
 * (DirectInputCreateA, version 0x300), each device cooperative-level
 * FOREGROUND | NONEXCLUSIVE. Every device's vtable is patched here (each
 * vtable once, re-checked for every device, as display.c does for DirectDraw):
 *   - Headless, the window is never in the foreground, so Acquire would fail
 *     and every read with it, and the game takes that for lost focus and stops
 *     its frame (docs/bringup.md 3). Acquire succeeds, and a failed read is an
 *     idle device: zeroed state, no buffered events. Nothing of the real
 *     devices reaches a headless run.
 *   - On a real screen an exclusive device is made non-exclusive, so the
 *     cursor and its clicks stay Windows' (as in Shadows of the Empire).
 *   - --key DIK@T[+D] holds a DirectInput key code from T seconds for D
 *     (default 0.2), in both the state and the buffered reads: scripted input
 *     for headless runs.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "input.h"
#include "display.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define DI_OK_ 0
#define DISCL_EXCLUSIVE_    0x1
#define DISCL_NONEXCLUSIVE_ 0x2

static int g_headless;
static DWORD g_t0;

/* ---- scripted keys -------------------------------------------------------- */

#define MAX_KEYS 256
static struct { int dik; DWORD on, off; } g_keys[MAX_KEYS];
static int g_nkeys;

int input_arg(int argc, char** argv, int i) {
    if (strcmp(argv[i], "--key") || i + 1 >= argc || g_nkeys == MAX_KEYS) return 0;
    const char* s = argv[i + 1];
    const char* at = strchr(s, '@');
    if (!at) return 0;
    double t = atof(at + 1), d = 0.2;
    const char* plus = strchr(at, '+');
    if (plus) d = atof(plus + 1);
    g_keys[g_nkeys].dik = (int)strtol(s, NULL, 0) & 0xFF;
    g_keys[g_nkeys].on = (DWORD)(t * 1000), g_keys[g_nkeys].off = (DWORD)((t + d) * 1000);
    g_nkeys++;
    return 2;
}

static void scripted(uint8_t held[256]) {
    DWORD now = GetTickCount() - g_t0;
    memset(held, 0, 256);
    for (int k = 0; k < g_nkeys; k++)
        if (now >= g_keys[k].on && now < g_keys[k].off) held[g_keys[k].dik] = 0x80;
}

/* ---- devices --------------------------------------------------------------- */

enum { DEV_KEYBOARD = 1, DEV_MOUSE, DEV_JOYSTICK };
static const GUID SYS_KEYBOARD = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID SYS_MOUSE = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

static struct { void* dev; int kind; } g_dev[16];
static int g_ndev;

static int dev_kind(void* d) {
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].dev == d) return g_dev[i].kind;
    return 0;
}
static void dev_add(void* d, int kind) {
    if (!dev_kind(d) && g_ndev < 16) g_dev[g_ndev].dev = d, g_dev[g_ndev++].kind = kind;
}

typedef struct { void** vt; void* real[32]; } vtab_t;
static vtab_t g_vt[8];
static int g_nvt;

static vtab_t* vt_of(void* obj) {
    for (int i = 0; i < g_nvt; i++)
        if (g_vt[i].vt == *(void***)obj) return &g_vt[i];
    return NULL;
}
#define REAL(obj, idx, type) ((type)vt_of(obj)->real[idx])

static void vt_hook(vtab_t* t, int idx, void* fn) {
    DWORD old;
    if (t->vt[idx] == fn) return;
    t->real[idx] = t->vt[idx];
    VirtualProtect(&t->vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    t->vt[idx] = fn;
    VirtualProtect(&t->vt[idx], sizeof(void*), old, &old);
}

static vtab_t* vt_add(void* obj) {
    vtab_t* t = vt_of(obj);
    if (!t && g_nvt < 8) t = &g_vt[g_nvt++], t->vt = *(void***)obj;
    return t;
}

static void dev_patch(void* d);

static HRESULT WINAPI dev_QueryInterface(void* self, REFIID riid, void** out) {
    typedef HRESULT (WINAPI *f_t)(void*, REFIID, void**);
    HRESULT hr = REAL(self, 0, f_t)(self, riid, out);
    if (SUCCEEDED(hr) && *out && dev_kind(self)) {   /* IDirectInputDevice2A: the same device */
        dev_add(*out, dev_kind(self));
        dev_patch(*out);
    }
    return hr;
}

static HRESULT WINAPI dev_Acquire(void* self) {
    typedef HRESULT (WINAPI *f_t)(void*);
    HRESULT hr = REAL(self, 7, f_t)(self);
    return g_headless ? DI_OK_ : hr;
}

static HRESULT WINAPI dev_GetDeviceState(void* self, DWORD size, void* buf) {
    typedef HRESULT (WINAPI *f_t)(void*, DWORD, void*);
    HRESULT hr = REAL(self, 9, f_t)(self, size, buf);
    if (g_headless) {
        if (FAILED(hr)) memset(buf, 0, size);
        hr = DI_OK_;
    }
    if (dev_kind(self) == DEV_KEYBOARD && size >= 256 && g_nkeys) {
        uint8_t held[256];
        scripted(held);
        for (int k = 0; k < 256; k++) ((uint8_t*)buf)[k] |= held[k];
    }
    return hr;
}

/* Buffered reads: the scripted keys' changes since the last read, appended as
 * DIDEVICEOBJECTDATA (offset = key code, 0x80 down / 0 up). */
static HRESULT WINAPI dev_GetDeviceData(void* self, DWORD each, void* data, DWORD* inout, DWORD flags) {
    typedef HRESULT (WINAPI *f_t)(void*, DWORD, void*, DWORD*, DWORD);
    static uint8_t told[256];
    static DWORD seq = 0x10000;
    DWORD room = *inout;
    HRESULT hr = REAL(self, 10, f_t)(self, each, data, inout, flags);
    if (g_headless && FAILED(hr)) *inout = 0, hr = DI_OK_;
    if (dev_kind(self) == DEV_KEYBOARD && g_nkeys && data && !(flags & 1 /* DIGDD_PEEK */) && each >= 16) {
        uint8_t held[256];
        scripted(held);
        for (int k = 0; k < 256 && *inout < room; k++) {
            if (held[k] == told[k]) continue;
            DWORD* e = (DWORD*)((uint8_t*)data + (size_t)*inout * each);
            e[0] = (DWORD)k, e[1] = held[k], e[2] = GetTickCount(), e[3] = seq++;
            told[k] = held[k];
            (*inout)++;
        }
    }
    return hr;
}

static HRESULT WINAPI dev_SetCooperativeLevel(void* self, HWND h, DWORD flags) {
    typedef HRESULT (WINAPI *f_t)(void*, HWND, DWORD);
    DWORD asked = flags;
    if (!g_headless && (flags & DISCL_EXCLUSIVE_)) flags = (flags & ~DISCL_EXCLUSIVE_) | DISCL_NONEXCLUSIVE_;
    HRESULT hr = REAL(self, 13, f_t)(self, h, flags);
    fprintf(stderr, "[input] %s SetCooperativeLevel(0x%lX -> 0x%lX) -> 0x%08lX\n",
            dev_kind(self) == DEV_KEYBOARD ? "keyboard" : dev_kind(self) == DEV_MOUSE ? "mouse" : "joystick",
            asked, flags, hr);
    return hr;
}

static void dev_patch(void* d) {
    vtab_t* t = vt_add(d);
    if (!t) return;
    vt_hook(t, 0, (void*)dev_QueryInterface);
    vt_hook(t, 7, (void*)dev_Acquire);
    vt_hook(t, 9, (void*)dev_GetDeviceState);
    vt_hook(t, 10, (void*)dev_GetDeviceData);
    vt_hook(t, 13, (void*)dev_SetCooperativeLevel);
}

static HRESULT WINAPI di_CreateDevice(void* di, const GUID* g, void** out, void* outer) {
    typedef HRESULT (WINAPI *f_t)(void*, const GUID*, void**, void*);
    HRESULT hr = REAL(di, 3, f_t)(di, g, out, outer);
    if (SUCCEEDED(hr) && *out) {
        int kind = IsEqualGUID(g, &SYS_KEYBOARD) ? DEV_KEYBOARD : IsEqualGUID(g, &SYS_MOUSE) ? DEV_MOUSE : DEV_JOYSTICK;
        dev_add(*out, kind);
        dev_patch(*out);
    }
    return hr;
}

/* DirectInputCreateA checks that hinst is a module the loader knows; the
 * guest's (0x00400000, mapped by native32) is not. The host's stands in. */
void input_shim_DirectInputCreateA(void) {
    typedef HRESULT (WINAPI *dic_t)(HINSTANCE, DWORD, void**, void*);
    static dic_t real;
    void** out = (void**)(uintptr_t)ARG(2);
    if (!real) real = (dic_t)GetProcAddress(LoadLibraryA("dinput.dll"), "DirectInputCreateA");
    HRESULT hr = real ? real(GetModuleHandleA(NULL), ARG(1), out, (void*)(uintptr_t)ARG(3)) : E_FAIL;
    fprintf(stderr, "[input] DirectInputCreateA(version 0x%X) -> 0x%08lX\n", ARG(1), hr);
    if (SUCCEEDED(hr) && *out) {
        vtab_t* t = vt_add(*out);
        if (t) vt_hook(t, 3, (void*)di_CreateDevice);
    }
    g_eax = (uint32_t)hr;
    g_esp += 4 + 4 * 4;
}

/* The game also takes keys from its window's WM_KEYDOWN/WM_KEYUP (the scan
 * code in lParam, queued by its window procedure, sub_00587670), and that is
 * where its menus read them. A headless window never has the focus, so the
 * scripted keys are posted to it as well, as a keyboard would send them. */
static DWORD WINAPI key_poster(LPVOID unused) {
    static uint8_t down[256];
    (void)unused;
    for (;;) {
        uint8_t held[256];
        HWND h = (HWND)display_hwnd();
        Sleep(5);
        if (!h) continue;
        scripted(held);
        for (int k = 0; k < 256; k++) {
            if (!held[k] == !down[k]) continue;
            UINT scan = (UINT)(k & 0x7F) | (k & 0x80 ? 0xE000u : 0);
            UINT vk = MapVirtualKeyA(scan, 3 /* MAPVK_VSC_TO_VK_EX */);
            LPARAM lp = 1 | ((LPARAM)(k & 0x7F) << 16) | (k & 0x80 ? 1 << 24 : 0) | (held[k] ? 0 : 0xC0000000);
            PostMessageA(h, held[k] ? WM_KEYDOWN : WM_KEYUP, vk, lp);
            fprintf(stderr, "[input] key 0x%02X %s (scripted)\n", k, held[k] ? "down" : "up");
            down[k] = held[k];
        }
    }
}

void input_init(int headless) {
    g_headless = headless;
    g_t0 = GetTickCount();
    if (g_nkeys) CloseHandle(CreateThread(NULL, 0, key_poster, NULL, 0, NULL));
}
