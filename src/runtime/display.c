/*
 * The display: the game's DirectDraw, steered.
 *
 * Rogue Squadron draws through DirectDraw and Direct3D (DirectX 6) on
 * Windows' own ddraw.dll. As shipped it takes the screen: exclusive
 * cooperative level, a display mode change, and a fullscreen flip chain.
 * None of that may happen here (REPO_RULES 13: a headless run takes no
 * screen, and a mode change over RDP lands on the phone), so the seam is the
 * DirectDraw objects themselves. The host binds DDRAW!DirectDrawCreate here,
 * and the vtable of every DirectDraw and surface interface the game gets is
 * patched (each vtable once; the same slot serves every object of it):
 *   - SetCooperativeLevel is always DDSCL_NORMAL, SetDisplayMode only
 *     remembers the mode, and GetDisplayMode answers with it;
 *   - the primary surface and its back buffer are offscreen surfaces of the
 *     mode's size and format (16-bit 565 for a 16-bit mode), and Direct3D
 *     renders into the back one as it would into a real back buffer;
 *   - Flip (or a Blt to the primary) is the present: the picture is read
 *     back, counted, recorded with --record, and on a real screen drawn into
 *     the game's window, scaled with its aspect kept.
 * A surface created without a pixel format gets the mode's: in a window it
 * would otherwise get the desktop's 32 bits, which the game does not expect.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "display.h"

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define DDSCAPS_EXECUTEBUFFER 0x00800000u   /* Direct3D's; DDSCAPS_RESERVED2 in today's ddraw.h */

static int g_headless;
static const char* g_record;
static long g_record_frames;
static FILE* g_ffmpeg;
static long g_recorded;
static volatile LONG g_frames;
static HWND g_hwnd;
static DWORD g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;
static int g_log;

long display_frames(void) { return g_frames; }
void* display_hwnd(void) { return g_hwnd; }

/* ---- vtables -------------------------------------------------------------- */

enum { K_DD, K_SURF };
typedef struct { void** vt; int kind, ver; void* real[48]; } vtab_t;
static vtab_t g_vt[24];
static int g_nvt;

static vtab_t* vt_of(void* obj) {
    void** vt = *(void***)obj;
    for (int i = 0; i < g_nvt; i++)
        if (g_vt[i].vt == vt) return &g_vt[i];
    return NULL;
}
#define REAL(obj, idx, type) ((type)vt_of(obj)->real[idx])

/* Called again for every object of the vtable: Windows' compatibility layer
 * (apphelp.dll, the DWM8And16BitMitigation layer Windows puts on a DirectDraw
 * program) writes its own entries into DirectDraw's vtables at each
 * DirectDrawCreate, over these, so a hook is put back whenever it is gone. */
static void vt_hook(vtab_t* t, int idx, void* fn) {
    DWORD old;
    if (t->vt[idx] == fn) return;
    t->real[idx] = t->vt[idx];
    VirtualProtect(&t->vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    t->vt[idx] = fn;
    VirtualProtect(&t->vt[idx], sizeof(void*), old, &old);
}

static const char* module_of(const void* p) {
    static char buf[4][MAX_PATH];
    static int k;
    HMODULE m = NULL;
    char* b = buf[k++ & 3];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &m) || !GetModuleFileNameA(m, b, MAX_PATH))
        return "?";
    const char* s = strrchr(b, '\\');
    return s ? s + 1 : b;
}

static vtab_t* vt_add(void* obj, int kind, int ver) {
    vtab_t* t = vt_of(obj);
    void** vt = *(void***)obj;
    if (!t && g_log)
        fprintf(stderr, "[ddraw] %s v%d object %p: vtable %p (%s), slot 0 in %s\n", kind == K_DD ? "DirectDraw" : "surface",
                ver, obj, (void*)vt, module_of(vt), module_of(vt[0]));
    if (t) return t;
    if (g_nvt == (int)(sizeof g_vt / sizeof g_vt[0])) return NULL;
    t = &g_vt[g_nvt++];
    t->vt = *(void***)obj, t->kind = kind, t->ver = ver;
    return t;
}

/* ---- the fake flip chain ------------------------------------------------- */

static void* g_front[8];               /* every interface of the primary that the game holds */
static void* g_back[8];
static int g_nfront, g_nback;
static void* g_back_base;              /* the back buffer, as created */

static int in(void* const* set, int n, void* p) {
    for (int i = 0; i < n; i++)
        if (set[i] == p) return 1;
    return 0;
}
static void add(void** set, int* n, void* p) {
    if (*n < 8 && !in(set, *n, p)) set[(*n)++] = p;
}

static const IID* surf_iid(int ver) {
    return ver == 7 ? &IID_IDirectDrawSurface7 : ver == 4 ? &IID_IDirectDrawSurface4 :
           ver == 3 ? &IID_IDirectDrawSurface3 : ver == 2 ? &IID_IDirectDrawSurface2 : &IID_IDirectDrawSurface;
}

/* The mode's pixel format, for a 16-bit mode: 565, as every card the game
 * knew answered. */
static void mode_format(DDPIXELFORMAT* pf) {
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = g_mode_bpp;
    if (g_mode_bpp == 16) pf->dwRBitMask = 0xF800, pf->dwGBitMask = 0x07E0, pf->dwBBitMask = 0x001F;
    else pf->dwRBitMask = 0xFF0000, pf->dwGBitMask = 0x00FF00, pf->dwBBitMask = 0x0000FF;
}

/* ---- presenting ----------------------------------------------------------- */

static uint32_t g_rgb[2048 * 1536];    /* the frame as BGRX */
static DWORD g_rgb_w, g_rgb_h;

static void record_frame(void);

/* The surface read back into g_rgb, from 16-bit 565/555 or 32-bit. */
static int read_surface(void* s) {
    typedef HRESULT (WINAPI *lock_t)(void*, RECT*, DDSURFACEDESC*, DWORD, HANDLE);
    typedef HRESULT (WINAPI *unlock_t)(void*, void*);
    void** vt = *(void***)s;
    DDSURFACEDESC2 d;
    memset(&d, 0, sizeof d);
    d.dwSize = vt_of(s) && vt_of(s)->ver >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
    vtab_t* t = vt_of(s);
    HRESULT hr = ((lock_t)(t && t->real[25] ? t->real[25] : vt[25]))(s, NULL, (DDSURFACEDESC*)&d,
                                                                   DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(hr)) {
        static HRESULT last;
        if (hr != last) fprintf(stderr, "[display] reading the frame back: Lock 0x%08lX (frame %ld)\n", hr, g_frames);
        last = hr;
        return 0;
    }
    DWORD w = d.dwWidth > 2048 ? 2048 : d.dwWidth, h = d.dwHeight > 1536 ? 1536 : d.dwHeight;
    DWORD bpp = d.ddpfPixelFormat.dwRGBBitCount;
    int g6 = d.ddpfPixelFormat.dwGBitMask == 0x07E0;
    for (DWORD y = 0; y < h; y++) {
        const uint8_t* row = (const uint8_t*)d.lpSurface + (size_t)y * d.lPitch;
        uint32_t* o = g_rgb + (size_t)y * w;
        if (bpp == 32) memcpy(o, row, w * 4);
        else if (bpp == 16)
            for (DWORD x = 0; x < w; x++) {
                uint32_t p = ((const uint16_t*)row)[x], r, g, b;
                if (g6) r = p >> 11, g = (p >> 5) & 63, b = p & 31, g = g << 2 | g >> 4;
                else r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31, g = g << 3 | g >> 2;
                o[x] = (r << 3 | r >> 2) << 16 | g << 8 | (b << 3 | b >> 2);
            }
    }
    ((unlock_t)(t && t->real[32] ? t->real[32] : vt[32]))(s, NULL);     /* not surf_Unlock's present */
    g_rgb_w = w, g_rgb_h = h;
    return bpp == 16 || bpp == 32;
}

/* Into the game's window, scaled to fit with the aspect kept, black bars. */
static void blit_window(void) {
    RECT c;
    if (!g_hwnd || !GetClientRect(g_hwnd, &c) || c.right <= 0 || c.bottom <= 0) return;
    int cw = c.right, ch = c.bottom, w = cw, h = (int)((double)cw * g_rgb_h / g_rgb_w);
    if (h > ch) h = ch, w = (int)((double)ch * g_rgb_w / g_rgb_h);
    int x = (cw - w) / 2, y = (ch - h) / 2;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), (LONG)g_rgb_w, -(LONG)g_rgb_h, 1, 32, BI_RGB } };
    HDC dc = GetDC(g_hwnd);
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, x, y, w, h, 0, 0, g_rgb_w, g_rgb_h, g_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
    if (x > 0) PatBlt(dc, 0, 0, x, ch, BLACKNESS), PatBlt(dc, x + w, 0, cw - x - w, ch, BLACKNESS);
    if (y > 0) PatBlt(dc, 0, 0, cw, y, BLACKNESS), PatBlt(dc, 0, y + h, cw, ch - y - h, BLACKNESS);
    ReleaseDC(g_hwnd, dc);
}

static void present(void* s, const char* why) {
    static const char* last_why;
    static long last_frame;
    static int busy;
    if (busy) return;
    busy = 1;
    InterlockedIncrement(&g_frames);
    if (g_log && (why != last_why || g_frames - last_frame > 2000)) {   /* each change of source */
        fprintf(stderr, "[present] frame %ld: %s\n", g_frames, why);
        last_why = why, last_frame = g_frames;
    }
    if ((g_record || !g_headless || g_log) && read_surface(s)) {
        if (g_log && why[0] != 'F') {
            static int shown;
            DWORD lit = 0;
            for (DWORD i = 0; i < g_rgb_w * g_rgb_h; i++) lit += (g_rgb[i] & 0xFFFFFF) != 0;
            if (shown++ < 20) fprintf(stderr, "[present] frame %ld (%s): %lu of %lu pixels lit\n", g_frames, why, lit, g_rgb_w * g_rgb_h);
        }
        if (g_record) record_frame();
        if (!g_headless) blit_window();
    }
    busy = 0;
}

/* ---- IDirectDraw* --------------------------------------------------------- */

static HRESULT WINAPI dd_QueryInterface(void* self, REFIID riid, void** out);

static void dd_patch(void* dd, int ver);

static HRESULT WINAPI dd_SetCooperativeLevel(void* self, HWND h, DWORD flags) {
    typedef HRESULT (WINAPI *f_t)(void*, HWND, DWORD);
    DWORD keep = flags & (DDSCL_MULTITHREADED | DDSCL_FPUSETUP | DDSCL_FPUPRESERVE);
    if (h) g_hwnd = h;
    fprintf(stderr, "[ddraw] SetCooperativeLevel(0x%lX) -> DDSCL_NORMAL\n", flags);
    return REAL(self, 20, f_t)(self, h, DDSCL_NORMAL | keep);
}

static void mode_set(DWORD w, DWORD h, DWORD bpp) {
    fprintf(stderr, "[ddraw] SetDisplayMode(%lux%lux%lu): kept, the screen is left alone\n", w, h, bpp);
    g_mode_w = w, g_mode_h = h, g_mode_bpp = bpp;
}
static HRESULT WINAPI dd1_SetDisplayMode(void* self, DWORD w, DWORD h, DWORD bpp) {
    (void)self;
    mode_set(w, h, bpp);
    return DD_OK;
}
static HRESULT WINAPI dd2_SetDisplayMode(void* self, DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD flags) {
    (void)self, (void)rate, (void)flags;
    mode_set(w, h, bpp);
    return DD_OK;
}
static HRESULT WINAPI dd_RestoreDisplayMode(void* self) { (void)self; return DD_OK; }

static HRESULT WINAPI dd_GetDisplayMode(void* self, DDSURFACEDESC* d) {
    typedef HRESULT (WINAPI *f_t)(void*, DDSURFACEDESC*);
    HRESULT hr = REAL(self, 12, f_t)(self, d);
    if (SUCCEEDED(hr)) {
        d->dwWidth = g_mode_w, d->dwHeight = g_mode_h;
        d->lPitch = (LONG)(g_mode_w * g_mode_bpp / 8);
        mode_format(&d->ddpfPixelFormat);
    }
    return hr;
}

static void surf_patch(void* s, int ver);

/* DDSURFACEDESC and DDSURFACEDESC2 share every field used here, at the
 * same offsets; the copy keeps the caller's dwSize. */
static HRESULT WINAPI dd_CreateSurface(void* self, DDSURFACEDESC* d, void** out, IUnknown* outer) {
    typedef HRESULT (WINAPI *f_t)(void*, DDSURFACEDESC*, void**, IUnknown*);
    f_t real = REAL(self, 6, f_t);
    int ver = vt_of(self)->ver >= 4 ? vt_of(self)->ver : 1;
    DDSURFACEDESC2 c;
    HRESULT hr;
    memset(&c, 0, sizeof c);
    memcpy(&c, d, d->dwSize < sizeof c ? d->dwSize : sizeof c);
    if (c.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
        DWORD caps = c.ddsCaps.dwCaps;
        int backs = (caps & DDSCAPS_FLIP) && (c.dwFlags & DDSD_BACKBUFFERCOUNT) ? (int)c.dwBackBufferCount : 0;
        c.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        c.dwWidth = g_mode_w, c.dwHeight = g_mode_h, c.dwBackBufferCount = 0;
        mode_format(&c.ddpfPixelFormat);
        c.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        hr = real(self, (DDSURFACEDESC*)&c, out, outer);
        fprintf(stderr, "[ddraw] primary (caps 0x%lX, %d back) -> offscreen %lux%lux%lu: 0x%08lX\n", caps, backs,
                g_mode_w, g_mode_h, g_mode_bpp, hr);
        if (FAILED(hr)) return hr;
        g_nfront = g_nback = 0;
        add(g_front, &g_nfront, *out);
        surf_patch(*out, ver);
        if (backs) {
            c.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_3DDEVICE;
            hr = real(self, (DDSURFACEDESC*)&c, &g_back_base, outer);
            fprintf(stderr, "[ddraw]   back buffer: 0x%08lX\n", hr);
            if (FAILED(hr)) return hr;
            add(g_back, &g_nback, g_back_base);
        }
        return DD_OK;
    }
    if (!(c.dwFlags & DDSD_PIXELFORMAT) && (c.dwFlags & DDSD_HEIGHT) &&
        (c.ddsCaps.dwCaps & (DDSCAPS_OFFSCREENPLAIN | DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)) &&
        !(c.ddsCaps.dwCaps & (DDSCAPS_ZBUFFER | DDSCAPS_PALETTE | DDSCAPS_EXECUTEBUFFER))) {
        c.dwFlags |= DDSD_PIXELFORMAT;
        mode_format(&c.ddpfPixelFormat);
    }
    hr = real(self, (DDSURFACEDESC*)&c, out, outer);
    if (g_log)
        fprintf(stderr, "[ddraw] CreateSurface(%lux%lu caps 0x%lX flags 0x%lX bpp %lu) -> 0x%08lX\n", c.dwWidth,
                c.dwHeight, c.ddsCaps.dwCaps, c.dwFlags, c.ddpfPixelFormat.dwRGBBitCount, hr);
    if (SUCCEEDED(hr)) surf_patch(*out, ver);
    return hr;
}

/* Direct3D's, for the log only (d3d.h is not in every SDK). */
static const IID IID_IDirect3D = { 0x3BBA0080, 0x2421, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
static const IID IID_IDirect3D2 = { 0x6AAE1EC1, 0x662A, 0x11D0, { 0x88, 0x9D, 0x00, 0xAA, 0x00, 0xBB, 0xB7, 0x6A } };
static const IID IID_IDirect3D3 = { 0xBB223240, 0xE72B, 0x11D0, { 0xA9, 0xB4, 0x00, 0xAA, 0x00, 0xC0, 0x99, 0x3E } };
static const IID IID_IDirect3D7 = { 0xF5049E77, 0x4861, 0x11D2, { 0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8 } };
static const IID IID_IDirect3DTexture = { 0x2CDCD9E0, 0x25A0, 0x11CF, { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
static const IID IID_IDirect3DTexture2 = { 0x93281502, 0x8CF8, 0x11D0, { 0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29 } };

static const char* iid_name(REFIID r) {
    static const struct { const IID* iid; const char* name; } k[] = {
        { &IID_IDirectDraw2, "IDirectDraw2" }, { &IID_IDirectDraw4, "IDirectDraw4" },
        { &IID_IDirectDraw7, "IDirectDraw7" }, { &IID_IDirect3D, "IDirect3D" }, { &IID_IDirect3D2, "IDirect3D2" },
        { &IID_IDirect3D3, "IDirect3D3" }, { &IID_IDirect3D7, "IDirect3D7" },
        { &IID_IDirectDrawSurface, "IDirectDrawSurface" }, { &IID_IDirectDrawSurface2, "IDirectDrawSurface2" },
        { &IID_IDirectDrawSurface3, "IDirectDrawSurface3" }, { &IID_IDirectDrawSurface4, "IDirectDrawSurface4" },
        { &IID_IDirectDrawSurface7, "IDirectDrawSurface7" }, { &IID_IDirect3DTexture, "IDirect3DTexture" },
        { &IID_IDirect3DTexture2, "IDirect3DTexture2" }, { &IID_IDirectDrawGammaControl, "IDirectDrawGammaControl" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (IsEqualIID(r, k[i].iid)) return k[i].name;
    return "?";
}

static HRESULT WINAPI dd_QueryInterface(void* self, REFIID riid, void** out) {
    typedef HRESULT (WINAPI *f_t)(void*, REFIID, void**);
    HRESULT hr = REAL(self, 0, f_t)(self, riid, out);
    fprintf(stderr, "[ddraw] DirectDraw QueryInterface(%s) -> 0x%08lX\n", iid_name(riid), hr);
    if (SUCCEEDED(hr) && *out) {
        if (IsEqualIID(riid, &IID_IDirectDraw2)) dd_patch(*out, 2);
        else if (IsEqualIID(riid, &IID_IDirectDraw4)) dd_patch(*out, 4);
        else if (IsEqualIID(riid, &IID_IDirectDraw7)) dd_patch(*out, 7);
    }
    return hr;
}

static void dd_patch(void* dd, int ver) {
    vtab_t* t = vt_add(dd, K_DD, ver);
    if (!t) return;
    vt_hook(t, 0, (void*)dd_QueryInterface);
    vt_hook(t, 6, (void*)dd_CreateSurface);
    vt_hook(t, 12, (void*)dd_GetDisplayMode);
    vt_hook(t, 19, (void*)dd_RestoreDisplayMode);
    vt_hook(t, 20, (void*)dd_SetCooperativeLevel);
    vt_hook(t, 21, ver == 1 ? (void*)dd1_SetDisplayMode : (void*)dd2_SetDisplayMode);
}

/* ---- IDirectDrawSurface* -------------------------------------------------- */

static HRESULT WINAPI surf_QueryInterface(void* self, REFIID riid, void** out) {
    typedef HRESULT (WINAPI *f_t)(void*, REFIID, void**);
    HRESULT hr = REAL(self, 0, f_t)(self, riid, out);
    if (SUCCEEDED(hr) && *out) {
        int ver = IsEqualIID(riid, &IID_IDirectDrawSurface7) ? 7 : IsEqualIID(riid, &IID_IDirectDrawSurface4) ? 4 :
                  IsEqualIID(riid, &IID_IDirectDrawSurface3) ? 3 : IsEqualIID(riid, &IID_IDirectDrawSurface2) ? 2 :
                  IsEqualIID(riid, &IID_IDirectDrawSurface) ? 1 : 0;
        if (ver) surf_patch(*out, ver);
        if (in(g_front, g_nfront, self)) add(g_front, &g_nfront, *out);
        if (in(g_back, g_nback, self)) add(g_back, &g_nback, *out);
        if (g_log || in(g_front, g_nfront, self) || in(g_back, g_nback, self))
            fprintf(stderr, "[ddraw] %s QueryInterface(%s) -> 0x%08lX\n",
                    in(g_front, g_nfront, self) ? "primary" : in(g_back, g_nback, self) ? "back buffer" : "surface",
                    iid_name(riid), hr);
    }
    return hr;
}

/* RS_DRAW_LOG: the first calls of each kind onto each surface, by name. */
static void draw_log(const char* what, void* self) {
    static struct { const char* what; void* self; int n; } seen[512];
    static int nseen;
    int i;
    if (!g_log) return;
    for (i = 0; i < nseen; i++)
        if (seen[i].what == what && seen[i].self == self) break;
    if (i == nseen) {
        if (nseen == 512) return;
        seen[nseen].what = what, seen[nseen].self = self, nseen++;
    }
    if (seen[i].n++ < 3)
        fprintf(stderr, "[draw] %s on %s %p (frame %ld)\n", what,
                in(g_front, g_nfront, self) ? "primary" : in(g_back, g_nback, self) ? "back buffer" : "surface",
                self, g_frames);
}

static HRESULT WINAPI surf_Flip(void* self, void* target, DWORD flags) {
    typedef HRESULT (WINAPI *f_t)(void*, void*, DWORD);
    draw_log("Flip", self);
    typedef HRESULT (WINAPI *blt_t)(void*, RECT*, void*, RECT*, DWORD, DDBLTFX*);
    if (!in(g_front, g_nfront, self)) return REAL(self, 11, f_t)(self, target, flags);
    /* The frame becomes the primary's, as a real flip makes it: the game then
     * draws its 2D (menus, the loading screen) onto the primary, over the
     * last frame (sub_004E78F4 locks it). ponytail: a copy, not a swap; the
     * back buffer keeps its frame, which only matters to a game that draws
     * half a frame onto the one before last, and this one clears. */
    REAL(g_front[0], 5, blt_t)(g_front[0], NULL, g_back_base, NULL, DDBLT_WAIT, NULL);
    present(g_front[0], "Flip");
    return DD_OK;
}

static HRESULT WINAPI surf_Blt(void* self, RECT* dr, void* src, RECT* sr, DWORD flags, DDBLTFX* fx) {
    typedef HRESULT (WINAPI *f_t)(void*, RECT*, void*, RECT*, DWORD, DDBLTFX*);
    draw_log(in(g_back, g_nback, src) ? "Blt from the back buffer" : "Blt", self);
    HRESULT hr = REAL(self, 5, f_t)(self, dr, src, sr, flags, fx);
    if (src && in(g_front, g_nfront, self)) present(self, "Blt to the primary");
    return hr;
}

/* The other ways onto the primary: BltFast, a Lock written through, and a
 * GDI DC. Each presents the primary when it is done with. */
static HRESULT WINAPI surf_BltFast(void* self, DWORD x, DWORD y, void* src, RECT* sr, DWORD flags) {
    typedef HRESULT (WINAPI *f_t)(void*, DWORD, DWORD, void*, RECT*, DWORD);
    draw_log(in(g_back, g_nback, src) ? "BltFast from the back buffer" : "BltFast", self);
    HRESULT hr = REAL(self, 7, f_t)(self, x, y, src, sr, flags);
    if (in(g_front, g_nfront, self)) present(self, "BltFast to the primary");
    return hr;
}

static void* g_locked_bits;
static LONG g_locked_pitch;

static HRESULT WINAPI surf_Lock(void* self, RECT* r, DDSURFACEDESC* d, DWORD flags, HANDLE ev) {
    typedef HRESULT (WINAPI *f_t)(void*, RECT*, DDSURFACEDESC*, DWORD, HANDLE);
    HRESULT hr = REAL(self, 25, f_t)(self, r, d, flags, ev);
    if (SUCCEEDED(hr) && in(g_front, g_nfront, self)) g_locked_bits = d->lpSurface, g_locked_pitch = d->lPitch;
    if (g_log && (in(g_front, g_nfront, self) || in(g_back, g_nback, self))) {
        static int n;
        if (n++ < 40)
            fprintf(stderr, "[draw] Lock %s %p flags 0x%lX -> 0x%08lX, pitch %ld, from sub_%08X (frame %ld); "
                    "[0x73A514]=%08X [0x73A518]=%08X\n",
                    in(g_front, g_nfront, self) ? "primary" : "back buffer", self, flags, hr,
                    SUCCEEDED(hr) ? d->lPitch : 0, g_cur_func, g_frames, MEM32(0x0073A514u), MEM32(0x0073A518u));
    }
    return hr;
}

static HRESULT WINAPI surf_Unlock(void* self, void* r) {
    typedef HRESULT (WINAPI *f_t)(void*, void*);
    if (in(g_front, g_nfront, self) || in(g_back, g_nback, self)) draw_log("Unlock", self);
    if (g_log && g_locked_bits && in(g_front, g_nfront, self)) {
        static int n;
        DWORD lit = 0;
        for (DWORD y = 0; y < g_mode_h; y++)
            for (DWORD x = 0; x < g_mode_w; x++)
                lit += ((const uint16_t*)((const uint8_t*)g_locked_bits + y * g_locked_pitch))[x] != 0;
        if (n++ < 20) fprintf(stderr, "[draw] at Unlock, before it: %lu pixels lit at %p\n", lit, g_locked_bits);
    }
    HRESULT hr = REAL(self, 32, f_t)(self, r);
    if (in(g_front, g_nfront, self)) present(self, "Unlock of the primary");
    return hr;
}

static HRESULT WINAPI surf_ReleaseDC(void* self, HDC dc) {
    typedef HRESULT (WINAPI *f_t)(void*, HDC);
    if (in(g_front, g_nfront, self) || in(g_back, g_nback, self)) draw_log("ReleaseDC", self);
    HRESULT hr = REAL(self, 26, f_t)(self, dc);
    if (in(g_front, g_nfront, self)) present(self, "GDI on the primary");
    return hr;
}

/* A plain offscreen surface has no flip to be done with; the fake chain's
 * flip is always done. */
static HRESULT WINAPI surf_GetFlipStatus(void* self, DWORD flags) {
    typedef HRESULT (WINAPI *f_t)(void*, DWORD);
    if (in(g_front, g_nfront, self) || in(g_back, g_nback, self)) {
        draw_log("GetFlipStatus", self);
        return DD_OK;
    }
    return REAL(self, 18, f_t)(self, flags);
}

static HRESULT WINAPI surf_GetAttachedSurface(void* self, DWORD* caps, void** out) {
    typedef HRESULT (WINAPI *f_t)(void*, DWORD*, void**);
    typedef HRESULT (WINAPI *qi_t)(void*, REFIID, void**);
    if (g_back_base && in(g_front, g_nfront, self) && (*caps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP))) {
        HRESULT hr = ((qi_t)(*(void***)g_back_base)[0])(g_back_base, surf_iid(vt_of(self)->ver), out);
        if (SUCCEEDED(hr)) add(g_back, &g_nback, *out);
        fprintf(stderr, "[ddraw] primary GetAttachedSurface(back buffer) -> 0x%08lX\n", hr);
        return hr;
    }
    return REAL(self, 12, f_t)(self, caps, out);
}

static void surf_patch(void* s, int ver) {
    vtab_t* t = vt_add(s, K_SURF, ver);
    if (!t) return;
    vt_hook(t, 0, (void*)surf_QueryInterface);
    vt_hook(t, 5, (void*)surf_Blt);
    vt_hook(t, 7, (void*)surf_BltFast);
    vt_hook(t, 11, (void*)surf_Flip);
    vt_hook(t, 12, (void*)surf_GetAttachedSurface);
    vt_hook(t, 18, (void*)surf_GetFlipStatus);
    vt_hook(t, 25, (void*)surf_Lock);
    vt_hook(t, 26, (void*)surf_ReleaseDC);
    vt_hook(t, 32, (void*)surf_Unlock);
}

/* ---- the backstop --------------------------------------------------------- *
 * Whatever reaches DirectDraw past the hooks above (an interface whose vtable
 * was never seen, a compatibility shim's wrapper), it cannot change the
 * screen: every loaded DLL's import of ChangeDisplaySettings* is pointed at a
 * refusal. ddraw.dll changes modes only through those (its imports: user32
 * ChangeDisplaySettingsA/ExA). */
static LONG WINAPI no_mode_change(void) {
    fprintf(stderr, "[ddraw] a display mode change was refused (the backstop)\n");
    return DISP_CHANGE_SUCCESSFUL;
}
static LONG WINAPI no_mode_a(DEVMODEA* m, DWORD f) { (void)m, (void)f; return no_mode_change(); }
static LONG WINAPI no_mode_exa(LPCSTR d, DEVMODEA* m, HWND h, DWORD f, LPVOID p) {
    (void)d, (void)m, (void)h, (void)f, (void)p;
    return no_mode_change();
}
static LONG WINAPI no_mode_w(DEVMODEW* m, DWORD f) { (void)m, (void)f; return no_mode_change(); }
static LONG WINAPI no_mode_exw(LPCWSTR d, DEVMODEW* m, HWND h, DWORD f, LPVOID p) {
    (void)d, (void)m, (void)h, (void)f, (void)p;
    return no_mode_change();
}

static void guard_module(HMODULE m) {
    static const struct { const char* name; void* fn; } k[] = {
        { "ChangeDisplaySettingsA", (void*)no_mode_a }, { "ChangeDisplaySettingsExA", (void*)no_mode_exa },
        { "ChangeDisplaySettingsW", (void*)no_mode_w }, { "ChangeDisplaySettingsExW", (void*)no_mode_exw },
    };
    HMODULE user = GetModuleHandleA("user32.dll");
    void* real[4];
    uint8_t* b = (uint8_t*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!user || !dir->VirtualAddress) return;
    for (int i = 0; i < 4; i++) real[i] = (void*)GetProcAddress(user, k[i].name);
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dir->VirtualAddress); d->Name; d++)
        for (void** slot = (void**)(b + d->FirstThunk); *slot; slot++)
            for (int i = 0; i < 4; i++)
                if (*slot == real[i]) {
                    DWORD old;
                    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
                    *slot = k[i].fn;
                    VirtualProtect(slot, sizeof *slot, old, &old);
                }
}

/* Every DLL loaded so far but this host and user32 itself. */
static void guard_modes(void) {
    HMODULE self = GetModuleHandleA(NULL), user = GetModuleHandleA("user32.dll");
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t* p = (uint8_t*)0x10000; p < (uint8_t*)0x7FFF0000; p += mi.RegionSize) {
        if (!VirtualQuery(p, &mi, sizeof mi)) break;
        if (mi.Type == MEM_IMAGE && mi.AllocationBase == mi.BaseAddress && mi.BaseAddress != (void*)self &&
            mi.BaseAddress != (void*)user && mi.BaseAddress != (void*)0x00400000)
            guard_module((HMODULE)mi.BaseAddress);
    }
}

/* ---- the entry point the guest imports ------------------------------------ */

void display_shim_DirectDrawCreate(void) {
    typedef HRESULT (WINAPI *ddc_t)(GUID*, void**, IUnknown*);
    static ddc_t real;
    void** out = (void**)(uintptr_t)ARG(1);
    if (!real) real = (ddc_t)GetProcAddress(LoadLibraryA("ddraw.dll"), "DirectDrawCreate");
    guard_modes();
    HRESULT hr = real ? real((GUID*)(uintptr_t)ARG(0), out, (IUnknown*)(uintptr_t)ARG(2)) : E_FAIL;
    fprintf(stderr, "[ddraw] DirectDrawCreate(%s) -> 0x%08lX\n", ARG(0) ? "a device" : "the display", hr);
    guard_modes();                     /* and whatever it loaded */
    if (SUCCEEDED(hr) && *out) dd_patch(*out, 1);
    g_eax = (uint32_t)hr;
    g_esp += 4 + 3 * 4;
}

/* ---- recording ------------------------------------------------------------ */

void display_record_close(void) {
    if (g_ffmpeg) {
        _pclose(g_ffmpeg);
        g_ffmpeg = NULL;
        fprintf(stderr, "[record] %ld frames -> %s\n", g_recorded, g_record);
    }
}

/* g_rgb to ffmpeg at the run's clock, 30 frames a second: each present writes
 * as many frames as are owed since the start, so "at 22 s" in a script is 0:22
 * in the file. The size is the first frame's; a later mode is scaled into it,
 * nearest neighbour. */
static void record_frame(void) {
    static DWORD start;
    static uint32_t frame[2048 * 1536];
    static DWORD rw, rh;
    DWORD now = GetTickCount();
    if (!start) start = now;
    long owed = (long)((now - start) / 33.333) + 1 - g_recorded;
    if (owed <= 0) return;
    if (!g_ffmpeg) {
        char cmd[MAX_PATH * 2];
        rw = g_rgb_w & ~1u, rh = g_rgb_h & ~1u;
        _snprintf(cmd, sizeof cmd - 1, "ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgr0 -s %lux%lu -r 30 "
                  "-i - -c:v libx264 -pix_fmt yuv420p \"%s\"", rw, rh, g_record);
        g_ffmpeg = _popen(cmd, "wb");
        fprintf(stderr, "[record] %lux%lu -> %s\n", rw, rh, g_record);
    }
    for (DWORD y = 0; y < rh; y++)
        for (DWORD x = 0; x < rw; x++)
            frame[y * rw + x] = g_rgb[(size_t)(y * g_rgb_h / rh) * g_rgb_w + x * g_rgb_w / rw];
    for (; g_ffmpeg && owed > 0; owed--) {
        fwrite(frame, 4, (size_t)rw * rh, g_ffmpeg);
        if (++g_recorded == g_record_frames) break;
    }
    if (g_ffmpeg && g_recorded == g_record_frames) {
        display_record_close();
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
}

static DWORD WINAPI frame_meter(LPVOID unused) {
    (void)unused;
    for (int t = 10;; t += 10) {
        Sleep(10000);
        fprintf(stderr, "[display] %d s: %ld frames\n", t, g_frames);
    }
}

int display_init(const char* rdir, int headless, const char* record, long frames) {
    (void)rdir;
    g_headless = headless, g_record = record, g_record_frames = frames;
    g_log = getenv("RS_DRAW_LOG") != NULL;
    if (g_log) CloseHandle(CreateThread(NULL, 0, frame_meter, NULL, 0, NULL));
    return 1;
}
