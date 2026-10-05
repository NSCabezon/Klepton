// The glibc door's SDL video seam — see kl_flatwin.h for the backend half.
//
// Source 2 drives its window, its Vulkan surface and its input through SDL2,
// taking almost all of SDL by dlsym (and Panorama by plain imports). The guest's
// own libSDL2 is linked against X11/Wayland, neither of which is loaded here,
// so its video subsystem cannot come up: SDL_Vulkan_GetInstanceExtensions
// fails, the render system reports #Vulkan_InitializationFailure and quits.
//
// So the VIDEO half of SDL is answered here and everything else stays the
// guest's: SDL_Init loses SDL_INIT_VIDEO on its way to the real one (and gains
// EVENTS, so the event queue exists), windows are ours, the Vulkan surface is
// MoltenVK's on the backend's CAMetalLayer (or kl_vulkan's synthetic one when
// there is no window), and host input is pushed into the guest's own event
// queue with its own SDL_PushEvent — on the guest's thread, inside the
// SDL_PumpEvents/SDL_PollEvent family it was already calling. Joysticks, audio,
// timers, threads, hints and the keyboard tables remain the real SDL.
//
// Reached two ways, both from kl_glibc.c: the glibc ABI tier (imports bound at
// relocation — Panorama's) and the dlsym override (the engine's SDL table).
// The SDL2 ABI this writes against: SDL_Event is a 56-byte union, SDL_Window
// is opaque, SDL_DisplayMode is { format, w, h, refresh_rate, driverdata }.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "klepton.h"
#include "kl_env.h"
#include "kl_flatwin.h"
#include "kl_vulkan.h"
#include "kl_glibc_sdl.h"

static const kl_flatwin_backend *g_be;
void kl_flatwin_set_backend(const kl_flatwin_backend *b) { g_be = b; }
const kl_flatwin_backend *kl_flatwin_backend_now(void) { return g_be; }

// ---- the real SDL, for what stays the guest's -----------------------------------
static void *real(const char *name) {
    // The guest images only: kl_guest_sym_global never consults the shim tier,
    // so this cannot find our own override of the same name.
    void *p = kl_guest_sym_global(name);
    if (!p) fprintf(stderr, "  [sdl] guest SDL has no %s\n", name);
    return p;
}
#define REAL(type, name) ((type)real(name))

// ---- windows ----------------------------------------------------------------------
#define SDL_WINDOW_FULLSCREEN    0x1u
#define SDL_WINDOW_SHOWN         0x4u
#define SDL_WINDOW_HIDDEN        0x8u
#define SDL_WINDOW_INPUT_FOCUS   0x200u
#define SDL_WINDOW_MOUSE_FOCUS   0x400u
#define SDL_WINDOW_VULKAN        0x10000000u
#define SDL_INIT_VIDEO           0x20u
#define SDL_INIT_EVENTS          0x4000u

typedef struct {
    uint32_t magic, id, flags;
    int w, h, pw, ph, x, y;
    char title[256];
    void *be_win, *layer;
} sdlwin;
#define SDLWIN_MAGIC 0x6b6c7377u     // "klsw"
static sdlwin *g_wins[16];
static uint32_t g_next_id = 1;
static int g_video_inited, g_relative, g_text_input;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static sdlwin *W(void *p) {
    sdlwin *w = p;
    return (w && w->magic == SDLWIN_MAGIC) ? w : NULL;
}
static sdlwin *focus_win(void) {
    for (int i = 0; i < 16; i++) if (g_wins[i]) return g_wins[i];
    return NULL;
}

static void *sdl_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags) {
    sdlwin *win = calloc(1, sizeof *win);
    if (!win) return NULL;
    win->magic = SDLWIN_MAGIC;
    win->flags = (flags & ~SDL_WINDOW_HIDDEN) | SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS |
                 SDL_WINDOW_MOUSE_FOCUS;
    // A fullscreen request at the desktop size becomes a window: this is a
    // debugging frontend, and the engine asks for the display's own size.
    int dw = 1280, dh = 720, hz = 60;
    if (g_be && g_be->display) g_be->display(&dw, &dh, &hz);
    if (w <= 0 || h <= 0 || w > dw || h > dh) { w = dw * 3 / 4; h = dh * 3 / 4; }
    win->w = win->pw = w;
    win->h = win->ph = h;
    win->x = x; win->y = y;
    snprintf(win->title, sizeof win->title, "%s", title ? title : "");
    if (g_be && g_be->create)
        win->be_win = g_be->create(win->title, w, h, flags, &win->layer, &win->pw, &win->ph);
    pthread_mutex_lock(&g_mu);
    win->id = g_next_id++;
    for (int i = 0; i < 16; i++) if (!g_wins[i]) { g_wins[i] = win; break; }
    pthread_mutex_unlock(&g_mu);
    fprintf(stderr, "  [sdl] SDL_CreateWindow(\"%s\", %dx%d, flags %#x) -> window %u "
                    "(%s, drawable %dx%d)\n", win->title, w, h, flags, win->id,
            win->layer ? "host window, CAMetalLayer" : "no window: synthetic surface",
            win->pw, win->ph);
    return win;
}
static void sdl_DestroyWindow(void *p) {
    sdlwin *w = W(p);
    if (!w) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < 16; i++) if (g_wins[i] == w) g_wins[i] = NULL;
    pthread_mutex_unlock(&g_mu);
    if (g_be && g_be->destroy && w->be_win) g_be->destroy(w->be_win);
    w->magic = 0;
    free(w);
}
static void sdl_GetWindowSize(void *p, int *w, int *h) {
    sdlwin *s = W(p);
    if (w) *w = s ? s->w : 0;
    if (h) *h = s ? s->h : 0;
}
static void sdl_GetDrawableSize(void *p, int *w, int *h) {
    sdlwin *s = W(p);
    if (w) *w = s ? s->pw : 0;
    if (h) *h = s ? s->ph : 0;
}
static void sdl_SetWindowSize(void *p, int w, int h) {
    sdlwin *s = W(p);
    if (!s || w <= 0 || h <= 0) return;
    if (g_be && g_be->set_size && s->be_win) g_be->set_size(s->be_win, w, h);
}
static void sdl_GetWindowPosition(void *p, int *x, int *y) {
    sdlwin *s = W(p);
    if (x) *x = s ? s->x : 0;
    if (y) *y = s ? s->y : 0;
}
static void sdl_SetWindowPosition(void *p, int x, int y) {
    sdlwin *s = W(p);
    if (s) { s->x = x; s->y = y; }
}
static uint32_t sdl_GetWindowFlags(void *p) { sdlwin *s = W(p); return s ? s->flags : 0; }
static uint32_t sdl_GetWindowID(void *p) { sdlwin *s = W(p); return s ? s->id : 0; }
static void *sdl_GetWindowFromID(uint32_t id) {
    for (int i = 0; i < 16; i++) if (g_wins[i] && g_wins[i]->id == id) return g_wins[i];
    return NULL;
}
static const char *sdl_GetWindowTitle(void *p) { sdlwin *s = W(p); return s ? s->title : ""; }
static void sdl_SetWindowTitle(void *p, const char *t) {
    sdlwin *s = W(p);
    if (!s) return;
    snprintf(s->title, sizeof s->title, "%s", t ? t : "");
    if (g_be && g_be->set_title && s->be_win) g_be->set_title(s->be_win, s->title);
}
static void sdl_ShowWindow(void *p) {
    sdlwin *s = W(p);
    if (s && g_be && g_be->show && s->be_win) g_be->show(s->be_win, 1);
}
static void sdl_HideWindow(void *p) {
    sdlwin *s = W(p);
    if (s && g_be && g_be->show && s->be_win) g_be->show(s->be_win, 0);
}
static void sdl_noop_win(void *p) { (void)p; }
static void sdl_noop_win_i(void *p, int v) { (void)p; (void)v; }
static int  sdl_ret0_win_i(void *p, uint32_t v) { (void)p; (void)v; return 0; }
static int  sdl_ret0_win_p(void *p, const void *v) { (void)p; (void)v; return 0; }
static int  sdl_GetWindowGrab(void *p) { (void)p; return 0; }
static int  sdl_GetWindowDisplayIndex(void *p) { (void)p; return 0; }
static void *sdl_GetKeyboardFocus(void) { return focus_win(); }
static void *sdl_GetMouseFocus(void) { return focus_win(); }
static void *sdl_GetGrabbedWindow(void) { return NULL; }
// SDL_GetWindowWMInfo: no X11 Window or Wayland surface to give. SDL_FALSE is
// the documented answer for "this driver has none", and the Vulkan path does
// not need one — the surface comes from SDL_Vulkan_CreateSurface.
static int sdl_GetWindowWMInfo(void *p, void *info) { (void)p; (void)info; return 0; }

// ---- displays ------------------------------------------------------------------------
typedef struct { uint32_t format; int w, h, refresh_rate; void *driverdata; } sdl_mode;
typedef struct { int x, y, w, h; } sdl_rect;
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u

static void display(int *w, int *h, int *hz) {
    *w = 1920; *h = 1080; *hz = 60;
    if (g_be && g_be->display) g_be->display(w, h, hz);
}
static int sdl_GetNumVideoDisplays(void) { return 1; }
static int sdl_GetNumDisplayModes(int i) { (void)i; return 1; }
static int sdl_mode_fill(sdl_mode *m) {
    int w, h, hz;
    display(&w, &h, &hz);
    if (m) *m = (sdl_mode){ SDL_PIXELFORMAT_ARGB8888, w, h, hz, NULL };
    return 0;
}
static int sdl_GetDisplayMode(int d, int i, sdl_mode *m) { (void)d; (void)i; return sdl_mode_fill(m); }
static int sdl_GetDesktopDisplayMode(int d, sdl_mode *m) { (void)d; return sdl_mode_fill(m); }
static int sdl_GetCurrentDisplayMode(int d, sdl_mode *m) { (void)d; return sdl_mode_fill(m); }
static int sdl_GetWindowDisplayMode(void *p, sdl_mode *m) { (void)p; return sdl_mode_fill(m); }
static int sdl_GetDisplayBounds(int d, sdl_rect *r) {
    (void)d;
    int w, h, hz;
    display(&w, &h, &hz);
    if (r) *r = (sdl_rect){ 0, 0, w, h };
    return 0;
}
static int sdl_GetDisplayDPI(int d, float *dd, float *hd, float *vd) {
    (void)d;
    if (dd) *dd = 110; if (hd) *hd = 110; if (vd) *vd = 110;
    return 0;
}
static const char *sdl_GetDisplayName(int d) { (void)d; return "Klepton host display"; }

// ---- mouse, cursor, keyboard odds and ends ------------------------------------------
static int  sdl_SetRelativeMouseMode(int on) {
    g_relative = on;
    if (g_be && g_be->set_relative_mouse) g_be->set_relative_mouse(on);
    return 0;
}
static int  sdl_GetRelativeMouseMode(void) { return g_relative; }
static void sdl_WarpMouseInWindow(void *p, int x, int y) { (void)p; (void)x; (void)y; }
static int  sdl_WarpMouseGlobal(int x, int y) { (void)x; (void)y; return 0; }
static int  sdl_ShowCursor(int t) { (void)t; return 1; }
static int  g_cursor_tok;
static void *sdl_CreateCursor_any(void) { return &g_cursor_tok; }
static void *sdl_CreateCursor(const void *a, const void *b, int w, int h, int x, int y) {
    (void)a; (void)b; (void)w; (void)h; (void)x; (void)y; return &g_cursor_tok;
}
static void *sdl_CreateColorCursor(void *s, int x, int y) { (void)s; (void)x; (void)y; return &g_cursor_tok; }
static void *sdl_CreateSystemCursor(int id) { (void)id; return &g_cursor_tok; }
static void sdl_SetCursor(void *c) { (void)c; }
static void sdl_FreeCursor(void *c) { (void)c; }
static char *sdl_GetClipboardText(void) { return strdup(""); }
static int  sdl_HasClipboardText(void) { return 0; }
static int  sdl_SetClipboardText(const char *t) { (void)t; return 0; }
static void sdl_StartTextInput(void) { g_text_input = 1; }
static void sdl_StopTextInput(void) { g_text_input = 0; }
static int  sdl_IsTextInputActive(void) { return g_text_input; }
static void sdl_SetTextInputRect(const void *r) { (void)r; }
static int  sdl_ret0(void) { return 0; }
static void sdl_void(void) {}
static int  sdl_IsScreenSaverEnabled(void) { return 0; }
static int  sdl_GL_SetAttribute(int a, int v) { (void)a; (void)v; return 0; }

// ---- init: VIDEO is ours ----------------------------------------------------------------
static int sdl_InitSubSystem(uint32_t flags) {
    if (flags & SDL_INIT_VIDEO) g_video_inited = 1;
    uint32_t rest = flags & ~SDL_INIT_VIDEO;
    if (flags & SDL_INIT_VIDEO) rest |= SDL_INIT_EVENTS;
    if (!rest) return 0;
    int (*r)(uint32_t) = REAL(int (*)(uint32_t), "SDL_InitSubSystem");
    return r ? r(rest) : 0;
}
static int sdl_Init(uint32_t flags) { return sdl_InitSubSystem(flags); }
static void sdl_QuitSubSystem(uint32_t flags) {
    if (flags & SDL_INIT_VIDEO) g_video_inited = 0;
    void (*r)(uint32_t) = REAL(void (*)(uint32_t), "SDL_QuitSubSystem");
    if (r && (flags & ~SDL_INIT_VIDEO)) r(flags & ~SDL_INIT_VIDEO);
}
static uint32_t sdl_WasInit(uint32_t flags) {
    uint32_t (*r)(uint32_t) = REAL(uint32_t (*)(uint32_t), "SDL_WasInit");
    uint32_t v = r ? r(flags) : 0;
    if (g_video_inited) v |= SDL_INIT_VIDEO & (flags ? flags : ~0u);
    return v;
}

// ---- Vulkan ------------------------------------------------------------------------------
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const void *pLayer; } metal_surface_ci;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; void *window; } android_surface_ci;
#define VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT   1000217000u
#define VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR 1000008000u
typedef void *(*gipa_fn)(void *inst, const char *name);
typedef int (*create_surface_fn)(void *inst, const void *ci, const void *alloc, uint64_t *out);

static int sdl_Vulkan_LoadLibrary(const char *path) {
    if (kl_env_on("KL_TRACE_SDL", 0))
        fprintf(stderr, "  [sdl] SDL_Vulkan_LoadLibrary(%s) -> 0\n", path ? path : "NULL");
    return 0;
}
static void sdl_Vulkan_UnloadLibrary(void) {}
static void *sdl_Vulkan_GetVkGetInstanceProcAddr(void) { return kl_vulkan_sym("vkGetInstanceProcAddr"); }

static int window_has_layer(void *p) {
    sdlwin *s = W(p);
    if (s) return s->layer != NULL;
    return g_be && g_be->create;          // no window yet: what one would have
}
static int sdl_Vulkan_GetInstanceExtensions(void *p, unsigned *count, const char **names) {
    const char *ext[2] = { "VK_KHR_surface",
                           window_has_layer(p) ? "VK_EXT_metal_surface" : "VK_KHR_android_surface" };
    if (kl_env_on("KL_TRACE_SDL", 0))
        fprintf(stderr, "  [sdl] SDL_Vulkan_GetInstanceExtensions(%p, %u, %p) -> %s, %s\n",
                p, count ? *count : 0, (void *)names, ext[0], ext[1]);
    if (!count) return 0;
    if (!names) { *count = 2; return 1; }
    if (*count < 2) return 0;
    names[0] = ext[0]; names[1] = ext[1];
    *count = 2;
    return 1;
}
static int sdl_Vulkan_CreateSurface(void *p, void *instance, uint64_t *surface) {
    sdlwin *s = W(p);
    gipa_fn gipa = (gipa_fn)kl_vulkan_sym("vkGetInstanceProcAddr");
    if (!gipa || !surface) return 0;
    int r;
    if (s && s->layer) {
        create_surface_fn f = (create_surface_fn)gipa(instance, "vkCreateMetalSurfaceEXT");
        metal_surface_ci ci = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, NULL, 0, s->layer };
        r = f ? f(instance, &ci, NULL, surface) : -1;
        fprintf(stderr, "  [sdl] SDL_Vulkan_CreateSurface: vkCreateMetalSurfaceEXT -> %d\n", r);
    } else {
        create_surface_fn f = (create_surface_fn)gipa(instance, "vkCreateAndroidSurfaceKHR");
        android_surface_ci ci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, NULL };
        r = f ? f(instance, &ci, NULL, surface) : -1;
        fprintf(stderr, "  [sdl] SDL_Vulkan_CreateSurface: synthetic surface -> %d\n", r);
    }
    return r == 0;
}

// ---- events: host input into the guest's own queue ------------------------------------------
// SDL2's SDL_Event, the members used here (offsets per SDL_events.h, 2.x).
#define SDL_QUIT_           0x100u
#define SDL_WINDOWEVENT_    0x200u
#define SDL_KEYDOWN_        0x300u
#define SDL_KEYUP_          0x301u
#define SDL_TEXTINPUT_      0x303u
#define SDL_MOUSEMOTION_    0x400u
#define SDL_MOUSEBUTTONDOWN_ 0x401u
#define SDL_MOUSEBUTTONUP_  0x402u
#define SDL_MOUSEWHEEL_     0x403u
typedef union { uint8_t b[56]; uint32_t u[14]; } sdl_event;

static void put_u32(sdl_event *e, int off, uint32_t v) { memcpy(e->b + off, &v, 4); }
static void put_i32(sdl_event *e, int off, int32_t v)  { memcpy(e->b + off, &v, 4); }
static void put_f32(sdl_event *e, int off, float v)    { memcpy(e->b + off, &v, 4); }

static int translate(const kl_fw_event *f, sdl_event *e) {
    memset(e, 0, sizeof *e);
    sdlwin *w = focus_win();
    uint32_t wid = w ? w->id : 1;
    switch (f->kind) {
    case KL_FW_QUIT:
        put_u32(e, 0, SDL_QUIT_); return 1;
    case KL_FW_CLOSE:
        put_u32(e, 0, SDL_WINDOWEVENT_); put_u32(e, 8, wid); e->b[12] = 14; return 1;
    case KL_FW_FOCUS:
        put_u32(e, 0, SDL_WINDOWEVENT_); put_u32(e, 8, wid); e->b[12] = f->down ? 12 : 13;
        return 1;
    case KL_FW_RESIZED:
        if (w) { w->w = f->x; w->h = f->y; w->pw = f->dx; w->ph = f->dy; }
        put_u32(e, 0, SDL_WINDOWEVENT_); put_u32(e, 8, wid); e->b[12] = 6;   // SIZE_CHANGED
        put_i32(e, 16, f->x); put_i32(e, 20, f->y);
        return 1;
    case KL_FW_KEY:
        put_u32(e, 0, f->down ? SDL_KEYDOWN_ : SDL_KEYUP_); put_u32(e, 8, wid);
        e->b[12] = f->down ? 1 : 0; e->b[13] = (uint8_t)f->repeat;
        put_i32(e, 16, f->scancode); put_i32(e, 20, f->keycode);
        { uint16_t m = (uint16_t)f->mod; memcpy(e->b + 24, &m, 2); }
        return 1;
    case KL_FW_TEXT:
        put_u32(e, 0, SDL_TEXTINPUT_); put_u32(e, 8, wid);
        memcpy(e->b + 12, f->text, 31);
        return 1;
    case KL_FW_MOTION:
        put_u32(e, 0, SDL_MOUSEMOTION_); put_u32(e, 8, wid);
        put_u32(e, 16, (uint32_t)f->buttons);
        put_i32(e, 20, f->x); put_i32(e, 24, f->y);
        put_i32(e, 28, f->dx); put_i32(e, 32, f->dy);
        return 1;
    case KL_FW_BUTTON:
        put_u32(e, 0, f->down ? SDL_MOUSEBUTTONDOWN_ : SDL_MOUSEBUTTONUP_); put_u32(e, 8, wid);
        e->b[16] = (uint8_t)f->button; e->b[17] = f->down ? 1 : 0;
        e->b[18] = (uint8_t)(f->clicks ? f->clicks : 1);
        put_i32(e, 20, f->x); put_i32(e, 24, f->y);
        return 1;
    case KL_FW_WHEEL:
        put_u32(e, 0, SDL_MOUSEWHEEL_); put_u32(e, 8, wid);
        put_i32(e, 16, (int32_t)f->fx); put_i32(e, 20, (int32_t)f->fy);
        put_f32(e, 28, f->fx); put_f32(e, 32, f->fy);
        return 1;
    }
    return 0;
}

// Move whatever the backend collected into the guest's queue. Guest thread.
static void drain_host_events(void) {
    if (!g_be || !g_be->poll) return;
    static int (*push)(sdl_event *);
    if (!push) push = REAL(int (*)(sdl_event *), "SDL_PushEvent");
    if (!push) return;
    kl_fw_event f;
    sdl_event e;
    while (g_be->poll(&f))
        if (translate(&f, &e)) push(&e);
}
static void sdl_PumpEvents(void) {
    drain_host_events();
    void (*r)(void) = REAL(void (*)(void), "SDL_PumpEvents");
    if (r) r();
}
static int sdl_PollEvent(sdl_event *e) {
    drain_host_events();
    int (*r)(sdl_event *) = REAL(int (*)(sdl_event *), "SDL_PollEvent");
    return r ? r(e) : 0;
}
static int sdl_WaitEventTimeout(sdl_event *e, int ms) {
    // In slices, so host input that arrives during the wait is delivered.
    int (*r)(sdl_event *, int) = REAL(int (*)(sdl_event *, int), "SDL_WaitEventTimeout");
    if (!r) return 0;
    for (int left = ms; ; ) {
        drain_host_events();
        int slice = (ms < 0 || left > 10) ? 10 : left;
        int got = r(e, slice);
        if (got) return got;
        if (ms >= 0 && (left -= slice) <= 0) return 0;
    }
}
static int sdl_WaitEvent(sdl_event *e) { return sdl_WaitEventTimeout(e, -1); }
static int sdl_PeepEvents(sdl_event *e, int n, int action, uint32_t lo, uint32_t hi) {
    drain_host_events();
    int (*r)(sdl_event *, int, int, uint32_t, uint32_t) =
        REAL(int (*)(sdl_event *, int, int, uint32_t, uint32_t), "SDL_PeepEvents");
    return r ? r(e, n, action, lo, hi) : 0;
}

// ---- the table ----------------------------------------------------------------------------
typedef struct { const char *name; void *fn; } entry;
#define F(n, f) { n, (void *)(f) }
static const entry g_sdl[] = {
    F("SDL_Init", sdl_Init), F("SDL_InitSubSystem", sdl_InitSubSystem),
    F("SDL_QuitSubSystem", sdl_QuitSubSystem), F("SDL_WasInit", sdl_WasInit),
    F("SDL_CreateWindow", sdl_CreateWindow), F("SDL_DestroyWindow", sdl_DestroyWindow),
    F("SDL_GetWindowSize", sdl_GetWindowSize), F("SDL_SetWindowSize", sdl_SetWindowSize),
    F("SDL_GetWindowSizeInPixels", sdl_GetDrawableSize),
    F("SDL_Vulkan_GetDrawableSize", sdl_GetDrawableSize),
    F("SDL_GL_GetDrawableSize", sdl_GetDrawableSize),
    F("SDL_GetWindowPosition", sdl_GetWindowPosition), F("SDL_SetWindowPosition", sdl_SetWindowPosition),
    F("SDL_GetWindowFlags", sdl_GetWindowFlags), F("SDL_GetWindowID", sdl_GetWindowID),
    F("SDL_GetWindowFromID", sdl_GetWindowFromID),
    F("SDL_GetWindowTitle", sdl_GetWindowTitle), F("SDL_SetWindowTitle", sdl_SetWindowTitle),
    F("SDL_ShowWindow", sdl_ShowWindow), F("SDL_HideWindow", sdl_HideWindow),
    F("SDL_RaiseWindow", sdl_noop_win), F("SDL_MinimizeWindow", sdl_noop_win),
    F("SDL_MaximizeWindow", sdl_noop_win), F("SDL_RestoreWindow", sdl_noop_win),
    F("SDL_SetWindowFullscreen", sdl_ret0_win_i), F("SDL_SetWindowBordered", sdl_noop_win_i),
    F("SDL_SetWindowResizable", sdl_noop_win_i),
    F("SDL_SetWindowGrab", sdl_noop_win_i), F("SDL_GetWindowGrab", sdl_GetWindowGrab),
    F("SDL_SetWindowIcon", sdl_ret0_win_p), F("SDL_SetWindowGammaRamp", sdl_ret0),
    F("SDL_GetWindowDisplayIndex", sdl_GetWindowDisplayIndex),
    F("SDL_GetWindowDisplayMode", sdl_GetWindowDisplayMode),
    F("SDL_SetWindowDisplayMode", sdl_ret0_win_p),
    F("SDL_GetWindowWMInfo", sdl_GetWindowWMInfo),
    F("SDL_GetKeyboardFocus", sdl_GetKeyboardFocus), F("SDL_GetMouseFocus", sdl_GetMouseFocus),
    F("SDL_GetGrabbedWindow", sdl_GetGrabbedWindow),
    F("SDL_GetNumVideoDisplays", sdl_GetNumVideoDisplays),
    F("SDL_GetNumDisplayModes", sdl_GetNumDisplayModes), F("SDL_GetDisplayMode", sdl_GetDisplayMode),
    F("SDL_GetDesktopDisplayMode", sdl_GetDesktopDisplayMode),
    F("SDL_GetCurrentDisplayMode", sdl_GetCurrentDisplayMode),
    F("SDL_GetDisplayBounds", sdl_GetDisplayBounds), F("SDL_GetDisplayUsableBounds", sdl_GetDisplayBounds),
    F("SDL_GetDisplayDPI", sdl_GetDisplayDPI), F("SDL_GetDisplayName", sdl_GetDisplayName),
    F("SDL_SetRelativeMouseMode", sdl_SetRelativeMouseMode),
    F("SDL_GetRelativeMouseMode", sdl_GetRelativeMouseMode),
    F("SDL_WarpMouseInWindow", sdl_WarpMouseInWindow), F("SDL_WarpMouseGlobal", sdl_WarpMouseGlobal),
    F("SDL_ShowCursor", sdl_ShowCursor), F("SDL_CreateCursor", sdl_CreateCursor),
    F("SDL_CreateColorCursor", sdl_CreateColorCursor), F("SDL_CreateSystemCursor", sdl_CreateSystemCursor),
    F("SDL_SetCursor", sdl_SetCursor), F("SDL_FreeCursor", sdl_FreeCursor),
    F("SDL_GetCursor", sdl_CreateCursor_any), F("SDL_GetDefaultCursor", sdl_CreateCursor_any),
    F("SDL_GetClipboardText", sdl_GetClipboardText), F("SDL_HasClipboardText", sdl_HasClipboardText),
    F("SDL_SetClipboardText", sdl_SetClipboardText),
    F("SDL_StartTextInput", sdl_StartTextInput), F("SDL_StopTextInput", sdl_StopTextInput),
    F("SDL_IsTextInputActive", sdl_IsTextInputActive), F("SDL_SetTextInputRect", sdl_SetTextInputRect),
    F("SDL_HasScreenKeyboardSupport", sdl_ret0), F("SDL_IsScreenKeyboardShown", sdl_ret0),
    F("SDL_DisableScreenSaver", sdl_void), F("SDL_EnableScreenSaver", sdl_void),
    F("SDL_IsScreenSaverEnabled", sdl_IsScreenSaverEnabled),
    F("SDL_GL_SetAttribute", sdl_GL_SetAttribute),
    F("SDL_Vulkan_LoadLibrary", sdl_Vulkan_LoadLibrary),
    F("SDL_Vulkan_UnloadLibrary", sdl_Vulkan_UnloadLibrary),
    F("SDL_Vulkan_GetVkGetInstanceProcAddr", sdl_Vulkan_GetVkGetInstanceProcAddr),
    F("SDL_Vulkan_GetInstanceExtensions", sdl_Vulkan_GetInstanceExtensions),
    F("SDL_Vulkan_CreateSurface", sdl_Vulkan_CreateSurface),
    F("SDL_PumpEvents", sdl_PumpEvents), F("SDL_PollEvent", sdl_PollEvent),
    F("SDL_WaitEvent", sdl_WaitEvent), F("SDL_WaitEventTimeout", sdl_WaitEventTimeout),
    F("SDL_PeepEvents", sdl_PeepEvents),
};

void *kl_glibc_sdl_lookup(const char *name) {
    if (!name || name[0] != 'S' || strncmp(name, "SDL_", 4)) return NULL;
    if (!kl_env_on("KL_GLIBC_SDL_SEAM", 1)) return NULL;
    for (size_t i = 0; i < sizeof g_sdl / sizeof *g_sdl; i++)
        if (!strcmp(g_sdl[i].name, name)) return g_sdl[i].fn;
    return NULL;
}
