/* USER32 + GDI32 on AppKit.
 *
 * The game runs on the main thread and drives its own loop with
 * PeekMessageA/DispatchMessageA; PeekMessageA pumps the AppKit event queue,
 * turning key/focus/close events into Win32 messages for the guest WndProc.
 * Each window's content view is backed by a CAMetalLayer that the native
 * video/renderer modules draw into (user32_metal_layer).
 */
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <ctype.h>
#include <sys/stat.h>
#include <math.h>
#include <mach/mach_time.h>

#include "guest_call.h"
#include "guest_memory.h"
#include "input/dinput.h"
#include "input/gamepad.h"
#include "render3d/world3d.h"
#include "video/presenter.h"
void d3ddll_probe_next_frame(void);
#include "input/player.h"
#include "platform/keymap.h"
#include "winapi/imports.h"
#include "winapi/user32_api.h"
#include "winapi/win32.h"

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ACTIVATEAPP 0x001C
#define WM_NCCREATE 0x0081
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202

/* ---- state ---------------------------------------------------------------- */

@interface GTA2View : NSView
@property(nonatomic) uint32_t hwnd;
@end

@interface GTA2WindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) uint32_t hwnd;
@end

typedef struct {
    char name[64];
    uint32_t wndproc;
} WndClass;

typedef struct {
    uint32_t hwnd, wndproc, style, exstyle, userdata;
    int w, h;
    void *window;   /* NSWindow, retained via CFBridgingRetain */
    void *view;     /* GTA2View */
    void *delegate;
} Window;

typedef struct {
    uint32_t hwnd, msg, wparam, lparam, time;
} Msg;

typedef struct {
    uint32_t hwnd, id, period_ms, proc;
    uint64_t next_ms;
    int active;
} Timer;

#define MAX_CLASSES 16
#define MAX_WINDOWS 16
#define QUEUE_SIZE 256
#define MAX_TIMERS 16
#define HWND_BASE 0x00010010u

static WndClass g_classes[MAX_CLASSES];
static Window g_windows[MAX_WINDOWS];
static Msg g_queue[QUEUE_SIZE];
static unsigned g_qhead, g_qtail;
static Timer g_timers[MAX_TIMERS];
static int g_quit_posted, g_quit_code;
static int g_app_ready;

static uint64_t now_ms(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000000u;
}

static Window *find_window(uint32_t hwnd)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_windows[i].hwnd && g_windows[i].hwnd == hwnd) return &g_windows[i];
    return NULL;
}

static void post(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l)
{
    unsigned next = (g_qtail + 1) % QUEUE_SIZE;
    if (next == g_qhead) {
        rt_log("user32: message queue full, dropping %04X", msg);
        return;
    }
    g_queue[g_qtail] = (Msg){ hwnd, msg, w, l, (uint32_t)now_ms() };
    g_qtail = next;
}

static uint32_t wnd_send(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l)
{
    Window *win = find_window(hwnd);
    if (!win || !win->wndproc) return 0;
    uint32_t args[4] = { hwnd, msg, w, l };
    return guest_call(win->wndproc, 4, args);
}

/* ---- AppKit glue -------------------------------------------------------------- */

/* One key transition from any source (AppKit or the GTA2_KEYS test script). */
void user32_update_title(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_windows[i].hwnd) continue;
        ((__bridge NSWindow *)g_windows[i].window).title =
            world3d_enabled() ? @"GTA2 – 3D renderer (⌘1: original)" : @"GTA2 – original renderer (⌘1: 3D)";
    }
}

void user32_action_toggle_renderer(void)
{
    world3d_toggle();
    user32_update_title();
}

void user32_action_snapshot(void)
{
    world3d_set(0); /* the probe reads the original renderer's stream */
    d3ddll_probe_next_frame();
    presenter_request_ab();
    user32_update_title();
}

void user32_action_replay(void) { presenter_save_replay(); }

/* While a movie plays, presses only count as "skip" and never reach the game. */
static int g_swallow_keys;
static unsigned g_presses;

static void key_transition(uint32_t hwnd, unsigned mac_keycode, int down, int repeat, NSString *chars)
{
    if (g_swallow_keys) {
        if (down && !repeat) g_presses++;
        return;
    }
    uint8_t vk, dik;
    if (!keymap_from_mac(mac_keycode, &vk, &dik)) return;
    /* F-keys still work (scripts use them); the menu's Cmd-1/2/3 are the easy way */
    if (vk == 0x79 /* F10 */ && down && !repeat) user32_action_snapshot();
    if (vk == 0x78 /* F9 */ && down && !repeat) user32_action_toggle_renderer();
    if (!repeat) dinput_key_event(dik, down, (uint32_t)now_ms());
    g_key_down[dik] = down ? 0x80 : 0;
    uint32_t lparam = 1u | (uint32_t)(dik & 0x7F) << 16 | (dik & 0x80 ? 1u << 24 : 0) |
                      (repeat ? 1u << 30 : 0) | (down ? 0 : 0xC0000000u);
    post(hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, lparam);
    if (down) {
        for (NSUInteger i = 0; i < chars.length; i++) {
            unichar c = [chars characterAtIndex:i];
            if (c < 256 && c != 0x7F && (c >= 0x20 || c == '\r' || c == '\b' || c == 0x1B))
                post(hwnd, WM_CHAR, c, lparam);
        }
    }
}

static void key_event(NSEvent *e, int down)
{
    uint32_t hwnd = ((GTA2View *)e.window.contentView).hwnd;
    /* isARepeat and characters are only valid for key-down/up events: AppKit
     * throws for flagsChanged (modifier keys), which crashed the game on Ctrl. */
    if (e.type == NSEventTypeFlagsChanged) {
        key_transition(hwnd, e.keyCode, down, 0, nil);
        return;
    }
    key_transition(hwnd, e.keyCode, down, down && e.isARepeat, down ? e.characters : nil);
}

/* GTA2_KEYS="ms:key[:hold_ms][,...]": press (and release 80 ms, or hold_ms,
 * later) keys at times after startup, for unattended test runs. Keys: return
 * escape space up down left right tab, or a single letter/digit.
 * GTA2_MEMSNAP="ms[,ms...]" (with GTA2_MEMSNAP_DIR): dump the game image's
 * data and the guest heap at those times, for finding game variables. */
typedef struct { uint64_t at; unsigned mac; int down; } ScriptKey;
static uint64_t g_snap_at[32];
static int g_nsnap, g_snap_pos;
static ScriptKey g_script[256];
static int g_nscript, g_script_pos;
static uint64_t g_t0;

static int mac_code_for(const char *name)
{
    static const struct { const char *n; int c; } named[] = {
        { "return", 0x24 }, { "enter", 0x24 }, { "escape", 0x35 }, { "esc", 0x35 }, { "space", 0x31 },
        { "up", 0x7E }, { "down", 0x7D }, { "left", 0x7B }, { "right", 0x7C }, { "tab", 0x30 },
        { "ctrl", 0x3B }, { "shift", 0x38 }, { "alt", 0x3A }, { "f9", 0x65 }, { "f10", 0x6D },
    };
    for (size_t i = 0; i < sizeof named / sizeof *named; i++)
        if (!strcasecmp(name, named[i].n)) return named[i].c;
    static const char letters[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.";
    if (strlen(name) == 1)
        for (int i = 0; letters[i] || i < 0x30; i++)
            if (letters[i] && tolower((unsigned char)name[0]) == letters[i]) return i;
    return -1;
}

static void script_load(void)
{
    const char *s = getenv("GTA2_KEYS");
    g_t0 = now_ms();
    if (!s) return;
    char *copy = strdup(s), *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok && g_nscript < 254; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = 0;
        char *hold = strchr(colon + 1, ':');
        uint64_t dur = 80;
        if (hold) { *hold = 0; dur = strtoull(hold + 1, NULL, 10); }
        int code = mac_code_for(colon + 1);
        if (code < 0) { rt_log("GTA2_KEYS: unknown key %s", colon + 1); continue; }
        uint64_t at = strtoull(tok, NULL, 10);
        g_script[g_nscript++] = (ScriptKey){ at, (unsigned)code, 1 };
        g_script[g_nscript++] = (ScriptKey){ at + dur, (unsigned)code, 0 };
    }
    free(copy);
    /* keep the script in time order (holds overlap later presses) */
    for (int i = 1; i < g_nscript; i++)
        for (int j = i; j > 0 && g_script[j - 1].at > g_script[j].at; j--) {
            ScriptKey t = g_script[j]; g_script[j] = g_script[j - 1]; g_script[j - 1] = t;
        }
    const char *m = getenv("GTA2_MEMSNAP");
    if (m) {
        char *c2 = strdup(m), *sv = NULL;
        for (char *tok = strtok_r(c2, ",", &sv); tok && g_nsnap < 32; tok = strtok_r(NULL, ",", &sv))
            g_snap_at[g_nsnap++] = strtoull(tok, NULL, 10);
        free(c2);
    }
}

/* GTA2_POKE="ms:addr:value[,...]": write a 16-bit value into game memory at
 * a time (hex addr), for testing what a variable does. */
static void poke_run(uint64_t t)
{
    static struct { uint64_t at; uint32_t addr; int v; } pk[32];
    static int n = -1, pos;
    if (n < 0) {
        n = 0;
        const char *e = getenv("GTA2_POKE");
        char *c = e ? strdup(e) : NULL, *sv = NULL;
        for (char *tok = c ? strtok_r(c, ",", &sv) : NULL; tok && n < 32; tok = strtok_r(NULL, ",", &sv)) {
            unsigned long long at; unsigned addr; int v;
            if (sscanf(tok, "%llu:%x:%d", &at, &addr, &v) == 3) pk[n].at = at, pk[n].addr = addr, pk[n++].v = v;
        }
        free(c);
    }
    while (pos < n && pk[pos].at <= t) {
        if (pk[pos].v != -32768) *(int16_t *)XBOX_PTR(pk[pos].addr) = (int16_t)pk[pos].v; /* -32768: just log */
        float c[3];
        world3d_camera(c);
        rt_log("poke %08X = %d (now %d, on foot %d) at %llu ms (camera %.2f, %.2f)", pk[pos].addr, pk[pos].v,
               *(int16_t *)XBOX_PTR(pk[pos].addr), player_on_foot(), (unsigned long long)t, c[0], c[1]);
        pos++;
    }
}

static void memsnap_write(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[4] = { 0x400000u, 0x700000u, GUEST_HEAP_LO, guest_heap_top() };
    fwrite(hdr, 4, 4, f);
    fwrite((const void *)XBOX_PTR(hdr[0]), 1, hdr[1] - hdr[0], f);
    fwrite((const void *)XBOX_PTR(hdr[2]), 1, hdr[3] - hdr[2], f);
    fclose(f);
}

/* Debug menu / Cmd-4: save a memory snapshot now (numbered), for finding game state. */
void user32_action_memsnap(void)
{
    static int n;
    const char *dir = getenv("GTA2_AB_DIR") ?: "captures";
    mkdir(dir, 0755);
    char path[1100];
    snprintf(path, sizeof path, "%s/memsnap_%02d.bin", dir, n++);
    memsnap_write(path);
    rt_log("memsnap saved %s", path);
}

static void memsnap_run(uint64_t t)
{
    poke_run(t);
    while (g_snap_pos < g_nsnap && g_snap_at[g_snap_pos] <= t) {
        const char *dir = getenv("GTA2_MEMSNAP_DIR") ?: ".";
        char path[1100];
        snprintf(path, sizeof path, "%s/snap_%02d.bin", dir, g_snap_pos);
        memsnap_write(path);
        float c[3];
        world3d_camera(c);
        int16_t *fa = player_facing();
        rt_log("memsnap %d at %llu ms -> %s (camera %.2f, %.2f) facing %d", g_snap_pos, (unsigned long long)t, path,
               c[0], c[1], fa ? *fa : -1);
        g_snap_pos++;
    }
}

static uint32_t first_hwnd(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_windows[i].hwnd) return g_windows[i].hwnd;
    return 0;
}

void user32_inject_key(unsigned mac_keycode, int down)
{
    key_transition(first_hwnd(), mac_keycode, down, 0, down && mac_keycode == 0x24 ? @"\r" : nil);
}

static void memsnap_run(uint64_t t);

static void script_run(void)
{
    uint64_t t = now_ms() - g_t0;
    memsnap_run(t);
    while (g_script_pos < g_nscript && g_script[g_script_pos].at <= t) {
        ScriptKey *k = &g_script[g_script_pos++];
        uint32_t hwnd = 0;
        for (int i = 0; i < MAX_WINDOWS; i++) if (g_windows[i].hwnd) { hwnd = g_windows[i].hwnd; break; }
        rt_log("GTA2_KEYS: %s key %02X at %llu ms", k->down ? "press" : "release", k->mac, (unsigned long long)t);
        key_transition(hwnd, k->mac, k->down, 0, k->down && k->mac == 0x24 ? @"\r" : nil);
    }
}

/* The game hides the cursor with ShowCursor(FALSE). Hiding it app-wide
 * (NSCursor hide) also hides it over dialogs and outside the window, so
 * instead a blank cursor is shown only over the game view. */
static int g_cursor_hidden;
static NSCursor *blank_cursor(void)
{
    static NSCursor *c;
    if (!c) {
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)];
        c = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
    }
    return c;
}

@interface GTA2Actions : NSObject
@end
@implementation GTA2Actions
- (void)toggleRenderer:(id)sender { user32_action_toggle_renderer(); }
- (void)snapshot:(id)sender { user32_action_snapshot(); }
- (void)replay:(id)sender { user32_action_replay(); }
- (void)memsnap:(id)sender { user32_action_memsnap(); }
@end
static GTA2Actions *g_actions;

@implementation GTA2View
- (void)resetCursorRects
{
    if (g_cursor_hidden) [self addCursorRect:self.bounds cursor:blank_cursor()];
}
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer
{
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    return layer;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent *)e
{
    if (e.modifierFlags & NSEventModifierFlagCommand) {
        [super keyDown:e];
        return;
    }
    key_event(e, 1);
}
- (void)keyUp:(NSEvent *)e { key_event(e, 0); }
- (void)flagsChanged:(NSEvent *)e
{
    NSEventModifierFlags f = e.modifierFlags;
    int down = 0;
    switch (e.keyCode) {
    case 0x38: case 0x3C: down = (f & NSEventModifierFlagShift) != 0; break;
    case 0x3B: case 0x3E: down = (f & NSEventModifierFlagControl) != 0; break;
    case 0x3A: case 0x3D: down = (f & NSEventModifierFlagOption) != 0; break;
    case 0x39: down = (f & NSEventModifierFlagCapsLock) != 0; break;
    default: return;
    }
    key_event(e, down);
}
@end

@implementation GTA2WindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    post(self.hwnd, WM_CLOSE, 0, 0);
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification *)n
{
    post(self.hwnd, WM_ACTIVATEAPP, 1, 0);
    post(self.hwnd, WM_ACTIVATE, 1, 0);
    post(self.hwnd, WM_SETFOCUS, 0, 0);
}
- (void)windowDidResignKey:(NSNotification *)n
{
    memset(g_key_down, 0, sizeof g_key_down);
    post(self.hwnd, WM_KILLFOCUS, 0, 0);
    post(self.hwnd, WM_ACTIVATE, 0, 0);
    post(self.hwnd, WM_ACTIVATEAPP, 0, 0);
}
@end

void user32_init(void)
{
    if (g_app_ready) return;
    @autoreleasepool {
        [NSApplication sharedApplication];
        /* GTA2_HEADLESS (automated test runs): no Dock icon, no focus stealing */
        [NSApp setActivationPolicy:getenv("GTA2_HEADLESS") ? NSApplicationActivationPolicyAccessory
                                                         : NSApplicationActivationPolicyRegular];
        NSMenu *bar = [NSMenu new];
        NSMenuItem *appItem = [NSMenuItem new];
        [bar addItem:appItem];
        NSMenu *appMenu = [NSMenu new];
        [appMenu addItemWithTitle:@"Quit GTA2" action:@selector(terminate:) keyEquivalent:@"q"];
        NSMenuItem *fs = [appMenu addItemWithTitle:@"Toggle Full Screen"
                                            action:@selector(toggleFullScreen:)
                                     keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagCommand;
        appItem.submenu = appMenu;
        /* Debug menu: Cmd-1/2/3 (no fn key needed, and the game never uses Cmd) */
        g_actions = [GTA2Actions new];
        NSMenuItem *dbgItem = [NSMenuItem new];
        [bar addItem:dbgItem];
        NSMenu *dbg = [[NSMenu alloc] initWithTitle:@"Debug"];
        struct { NSString *title, *key; SEL sel; } items[] = {
            { @"Switch Renderer (Original / 3D)", @"1", @selector(toggleRenderer:) },
            { @"Snapshot (A/B Screenshots + Map Log)", @"2", @selector(snapshot:) },
            { @"Save Instant Replay (Last 3 s)", @"3", @selector(replay:) },
            { @"Save Memory Snapshot", @"4", @selector(memsnap:) },
        };
        for (size_t i = 0; i < 4; i++) {
            NSMenuItem *it = [dbg addItemWithTitle:items[i].title action:items[i].sel keyEquivalent:items[i].key];
            it.target = g_actions;
            it.keyEquivalentModifierMask = NSEventModifierFlagCommand;
        }
        dbgItem.submenu = dbg;
        NSApp.mainMenu = bar;
        [NSApp finishLaunching];
        if (!getenv("GTA2_HEADLESS")) [NSApp activateIgnoringOtherApps:YES];
    }
    script_load();
    g_app_ready = 1;
}

static void pump_appkit(void)
{
    @autoreleasepool {
        NSEvent *e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES])) {
            [NSApp sendEvent:e];
        }
    }
    script_run();
    gamepad_poll();
    uint64_t t = now_ms();
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *tm = &g_timers[i];
        if (tm->active && t >= tm->next_ms) {
            tm->next_ms = t + tm->period_ms;
            post(tm->hwnd, WM_TIMER, tm->id, tm->proc);
        }
    }
}

void user32_pump(void) { pump_appkit(); }

void user32_swallow_keys(int on)
{
    g_swallow_keys = on;
    if (!on) memset(g_key_down, 0, sizeof g_key_down);
}

unsigned user32_presses(void) { return g_presses; }

void *user32_metal_layer(uint32_t hwnd)
{
    Window *w = find_window(hwnd);
    if (!w) {
        for (int i = 0; i < MAX_WINDOWS; i++) if (g_windows[i].hwnd) { w = &g_windows[i]; break; }
    }
    return w ? (__bridge void *)((__bridge NSView *)w->view).layer : NULL;
}

void user32_set_client_size(uint32_t hwnd, int width, int height)
{
    Window *w = find_window(hwnd);
    if (!w) return;
    w->w = width;
    w->h = height;
    NSWindow *win = (__bridge NSWindow *)w->window;
    win.contentAspectRatio = NSMakeSize(width, height);
    if (!(win.styleMask & NSWindowStyleMaskFullScreen)) {
        /* fit on screen, keeping the aspect ratio */
        NSRect vis = win.screen ? win.screen.visibleFrame : NSScreen.mainScreen.visibleFrame;
        /* small resolutions open at 2x (the 640x480 menus are tiny otherwise) */
        double want = width < 1280 ? 2.0 : 1.0;
        double k = fmin(want, fmin((vis.size.width - 40) / width, (vis.size.height - 60) / height));
        [win setContentSize:NSMakeSize(width * k, height * k)];
        [win center];
        if (reg_get_dword("screen", "mac_fullscreen", 0) && width > 640) [win toggleFullScreen:nil];
    }
}

/* ---- window classes & windows ---------------------------------------------- */

/* WNDCLASSA: style, lpfnWndProc@4, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor,
 * hbrBackground, lpszMenuName@32, lpszClassName@36 */
static void RegisterClassA(void)
{
    uint32_t wc = ARG(0);
    const char *name = GSTR(MEM32(wc + 36));
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (!g_classes[i].name[0] || !strcasecmp(g_classes[i].name, name)) {
            snprintf(g_classes[i].name, sizeof g_classes[i].name, "%s", name);
            g_classes[i].wndproc = MEM32(wc + 4);
            rt_log("RegisterClassA(%s) wndproc %08X", name, g_classes[i].wndproc);
            RETURN_STDCALL(0xC000u + (uint32_t)i, 1);
        }
    }
    RETURN_STDCALL(0, 1);
}

/* CreateWindowExA(exStyle, class, name, style, x, y, w, h, parent, menu, hinst, param) */
static void CreateWindowExA(void)
{
    uint32_t exstyle = ARG(0), cls = ARG(1), title = ARG(2), style = ARG(3);
    int w = (int)ARG(6), h = (int)ARG(7);
    const char *cname = cls < 0x10000u ? "" : GSTR(cls);
    WndClass *wc = NULL;
    for (int i = 0; i < MAX_CLASSES; i++)
        if (g_classes[i].name[0] && !strcasecmp(g_classes[i].name, cname)) wc = &g_classes[i];
    if (!wc) {
        rt_log("CreateWindowExA: unknown class %s", cname);
        RETURN_STDCALL(0, 12);
    }
    if (w <= 0 || (uint32_t)w == 0x80000000u) w = 640;
    if (h <= 0 || (uint32_t)h == 0x80000000u) h = 480;
    Window *win = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_windows[i].hwnd) {
            win = &g_windows[i];
            win->hwnd = HWND_BASE + 4u * (uint32_t)i;
            break;
        }
    }
    if (!win) RETURN_STDCALL(0, 12);
    user32_init();
    win->wndproc = wc->wndproc;
    win->style = style;
    win->exstyle = exstyle;
    win->w = w;
    win->h = h;

    @autoreleasepool {
        NSWindow *nw = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, w, h)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        nw.title = [NSString stringWithCString:title ? GSTR(title) : "GTA2" encoding:NSWindowsCP1252StringEncoding];
        nw.releasedWhenClosed = NO;
        nw.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        nw.contentAspectRatio = NSMakeSize(w, h);
        GTA2View *view = [[GTA2View alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
        view.wantsLayer = YES;
        view.hwnd = win->hwnd;
        nw.contentView = view;
        GTA2WindowDelegate *del = [GTA2WindowDelegate new];
        del.hwnd = win->hwnd;
        nw.delegate = del;
        [nw makeFirstResponder:view];
        {
            NSRect vis = nw.screen ? nw.screen.visibleFrame : NSScreen.mainScreen.visibleFrame;
            double k = fmin(w < 1280 ? 2.0 : 1.0, fmin((vis.size.width - 40) / w, (vis.size.height - 60) / h));
            [nw setContentSize:NSMakeSize(w * k, h * k)];
        }
        [nw center];
        if (getenv("GTA2_HEADLESS")) { /* still renders (for captures), but invisible and click-through */
            nw.alphaValue = 0;
            nw.ignoresMouseEvents = YES;
            nw.level = NSNormalWindowLevel - 1;
        }
        win->window = (void *)CFBridgingRetain(nw);
        win->view = (__bridge void *)view;
        win->delegate = (void *)CFBridgingRetain(del);
    }
    rt_log("CreateWindowExA(%s, %dx%d) -> %08X", cname, w, h, win->hwnd);
    user32_update_title();

    /* WM_NCCREATE / WM_CREATE with a CREATESTRUCTA in guest memory */
    uint32_t cs = guest_alloc(48, 1);
    MEM32(cs + 0) = ARG(11);
    MEM32(cs + 4) = ARG(10);
    MEM32(cs + 8) = ARG(9);
    MEM32(cs + 12) = ARG(8);
    MEM32(cs + 16) = (uint32_t)h;
    MEM32(cs + 20) = (uint32_t)w;
    MEM32(cs + 24) = ARG(5);
    MEM32(cs + 28) = ARG(4);
    MEM32(cs + 32) = style;
    MEM32(cs + 36) = title;
    MEM32(cs + 40) = cls;
    MEM32(cs + 44) = exstyle;
    uint32_t hwnd = win->hwnd;
    wnd_send(hwnd, WM_NCCREATE, 0, cs);
    wnd_send(hwnd, WM_CREATE, 0, cs);
    guest_free(cs);
    RETURN_STDCALL(hwnd, 12);
}

static void ShowWindow(void)
{
    Window *w = find_window(ARG(0));
    if (w) {
        NSWindow *nw = (__bridge NSWindow *)w->window;
        if (ARG(1) && getenv("GTA2_HEADLESS")) [nw orderBack:nil];
        else if (ARG(1)) [nw makeKeyAndOrderFront:nil];
        else [nw orderOut:nil];
    }
    RETURN_STDCALL(1, 2);
}

static void DestroyWindow(void)
{
    uint32_t hwnd = ARG(0);
    Window *w = find_window(hwnd);
    if (w) {
        wnd_send(hwnd, WM_DESTROY, 0, 0);
        NSWindow *nw = CFBridgingRelease(w->window);
        [nw orderOut:nil];
        CFBridgingRelease(w->delegate);
        memset(w, 0, sizeof *w);
    }
    RETURN_STDCALL(1, 1);
}

static void UpdateWindow(void) { RETURN_STDCALL(1, 1); }
static void SetForegroundWindow(void) { RETURN_STDCALL(1, 1); }
static void EnableWindow(void) { RETURN_STDCALL(0, 2); }
static void GetParent(void) { RETURN_STDCALL(0, 1); }
static void FindWindowExA(void) { RETURN_STDCALL(0, 4); }

/* SetWindowPos(hwnd, after, x, y, cx, cy, flags) */
static void SetWindowPos(void)
{
    uint32_t flags = ARG(6);
    if (!(flags & 0x1u /* SWP_NOSIZE */) && ARG(4) && ARG(5))
        user32_set_client_size(ARG(0), (int)ARG(4), (int)ARG(5));
    RETURN_STDCALL(1, 7);
}

static void write_rect(uint32_t r, int w, int h)
{
    MEM32(r) = 0;
    MEM32(r + 4) = 0;
    MEM32(r + 8) = (uint32_t)w;
    MEM32(r + 12) = (uint32_t)h;
}

static void GetClientRect(void)
{
    Window *w = find_window(ARG(0));
    write_rect(ARG(1), w ? w->w : 640, w ? w->h : 480);
    RETURN_STDCALL(w != NULL, 2);
}

static void GetWindowRect(void)
{
    Window *w = find_window(ARG(0));
    write_rect(ARG(1), w ? w->w : 640, w ? w->h : 480);
    RETURN_STDCALL(w != NULL, 2);
}

static void SetWindowTextA(void)
{
    Window *w = find_window(ARG(0));
    if (w) ((__bridge NSWindow *)w->window).title =
        [NSString stringWithCString:GSTR(ARG(1)) encoding:NSWindowsCP1252StringEncoding];
    RETURN_STDCALL(1, 2);
}

static void GetWindowTextA(void)
{
    if (ARG(2)) MEM8(ARG(1)) = 0;
    RETURN_STDCALL(0, 3);
}

#define GWL_WNDPROC (-4)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)

static void GetWindowLongA(void)
{
    Window *w = find_window(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    uint32_t v = 0;
    if (w) {
        if (idx == GWL_WNDPROC) v = w->wndproc;
        else if (idx == GWL_STYLE) v = w->style;
        else if (idx == GWL_EXSTYLE) v = w->exstyle;
        else if (idx == GWL_USERDATA) v = w->userdata;
    }
    RETURN_STDCALL(v, 2);
}

static void SetWindowLongA(void)
{
    Window *w = find_window(ARG(0));
    int32_t idx = (int32_t)ARG(1);
    uint32_t nv = ARG(2), old = 0;
    if (w) {
        uint32_t *slot = idx == GWL_WNDPROC ? &w->wndproc : idx == GWL_STYLE ? &w->style
                       : idx == GWL_EXSTYLE ? &w->exstyle : idx == GWL_USERDATA ? &w->userdata : NULL;
        if (slot) { old = *slot; *slot = nv; }
    }
    RETURN_STDCALL(old, 3);
}

static void CallWindowProcA(void)
{
    uint32_t args[4] = { ARG(1), ARG(2), ARG(3), ARG(4) };
    uint32_t proc = ARG(0);
    uint32_t r = guest_call(proc, 4, args);
    RETURN_STDCALL(r, 5);
}

static void DefWindowProcA(void)
{
    uint32_t hwnd = ARG(0), msg = ARG(1);
    uint32_t r = 0;
    if (msg == WM_NCCREATE) r = 1;
    else if (msg == WM_CLOSE) {
        Window *w = find_window(hwnd);
        if (w) {
            wnd_send(hwnd, WM_DESTROY, 0, 0);
            NSWindow *nw = CFBridgingRelease(w->window);
            [nw orderOut:nil];
            CFBridgingRelease(w->delegate);
            memset(w, 0, sizeof *w);
        }
    }
    RETURN_STDCALL(r, 4);
}

/* ---- messages ------------------------------------------------------------------ */

/* MSG: hwnd, message, wParam, lParam, time, pt.x, pt.y */
static void write_msg(uint32_t p, const Msg *m)
{
    MEM32(p + 0) = m->hwnd;
    MEM32(p + 4) = m->msg;
    MEM32(p + 8) = m->wparam;
    MEM32(p + 12) = m->lparam;
    MEM32(p + 16) = m->time;
    MEM32(p + 20) = 0;
    MEM32(p + 24) = 0;
}

static void PeekMessageA(void)
{
    uint32_t out = ARG(0), remove = ARG(4) & 1u;
    if (g_app_ready) pump_appkit();
    if (g_qhead == g_qtail && g_quit_posted) {
        Msg q = { 0, WM_QUIT, (uint32_t)g_quit_code, 0, (uint32_t)now_ms() };
        write_msg(out, &q);
        if (remove) g_quit_posted = 0;
        RETURN_STDCALL(1, 5);
    }
    if (g_qhead == g_qtail) RETURN_STDCALL(0, 5);
    write_msg(out, &g_queue[g_qhead]);
    if (remove) g_qhead = (g_qhead + 1) % QUEUE_SIZE;
    RETURN_STDCALL(1, 5);
}

static void TranslateMessage(void) { RETURN_STDCALL(0, 1); } /* WM_CHAR is posted with the key */

static void DispatchMessageA(void)
{
    uint32_t m = ARG(0);
    uint32_t hwnd = MEM32(m), msg = MEM32(m + 4), w = MEM32(m + 8), l = MEM32(m + 12);
    uint32_t r = 0;
    if (msg == WM_TIMER && l) {
        uint32_t args[4] = { hwnd, msg, w, (uint32_t)now_ms() };
        r = guest_call(l, 4, args);
    } else {
        r = wnd_send(hwnd, msg, w, l);
    }
    RETURN_STDCALL(r, 1);
}

static void PostMessageA(void)
{
    post(ARG(0), ARG(1), ARG(2), ARG(3));
    RETURN_STDCALL(1, 4);
}

static void SendMessageA(void)
{
    uint32_t r = wnd_send(ARG(0), ARG(1), ARG(2), ARG(3));
    RETURN_STDCALL(r, 4);
}

static void PostQuitMessage(void)
{
    g_quit_posted = 1;
    g_quit_code = (int)ARG(0);
    RETURN_STDCALL(0, 1);
}

/* SetTimer(hwnd, id, elapse, proc) */
static void SetTimer(void)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timers[i];
        if (!t->active || (t->hwnd == ARG(0) && t->id == ARG(1))) {
            *t = (Timer){ ARG(0), ARG(1) ? ARG(1) : 0x100u + (uint32_t)i, ARG(2) ? ARG(2) : 1u, ARG(3),
                          now_ms() + ARG(2), 1 };
            RETURN_STDCALL(t->id, 4);
        }
    }
    RETURN_STDCALL(0, 4);
}

static void KillTimer(void)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (g_timers[i].active && g_timers[i].hwnd == ARG(0) && g_timers[i].id == ARG(1))
            g_timers[i].active = 0;
    RETURN_STDCALL(1, 2);
}

/* ---- misc ---------------------------------------------------------------------- */

static void LoadIconA(void) { RETURN_STDCALL(0x00030001u, 2); }
static void LoadCursorA(void) { RETURN_STDCALL(0x00030002u, 2); }

static void ShowCursor(void)
{
    static int count = 0;
    count += ARG(0) ? 1 : -1;
    int hidden = count < 0;
    if (hidden != g_cursor_hidden) {
        g_cursor_hidden = hidden;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!g_windows[i].hwnd || !g_windows[i].window) continue;
            NSWindow *nw = (__bridge NSWindow *)g_windows[i].window;
            [nw invalidateCursorRectsForView:nw.contentView];
        }
        if (!hidden) [NSCursor.arrowCursor set];
    }
    RETURN_STDCALL((uint32_t)count, 1);
}

static void GetSystemMetrics(void)
{
    NSRect r = NSScreen.mainScreen.frame;
    int32_t idx = (int32_t)ARG(0);
    uint32_t v = 0;
    if (idx == 0) v = (uint32_t)r.size.width;      /* SM_CXSCREEN */
    else if (idx == 1) v = (uint32_t)r.size.height; /* SM_CYSCREEN */
    RETURN_STDCALL(v, 1);
}

static void GetKeyboardLayoutNameA(void)
{
    strcpy((char *)GPTR(ARG(0)), "00000409");
    RETURN_STDCALL(1, 1);
}

static void GetDC(void) { RETURN_STDCALL(0x00040001u, 1); }
static void ReleaseDC(void) { RETURN_STDCALL(1, 2); }

static void GetDeviceCaps(void)
{
    uint32_t idx = ARG(1), v = 0;
    NSRect r = NSScreen.mainScreen.frame;
    if (idx == 12) v = 16;                           /* BITSPIXEL: the game wants a 16-bit desktop */
    else if (idx == 14) v = 1;                       /* PLANES */
    else if (idx == 8) v = (uint32_t)r.size.width;   /* HORZRES */
    else if (idx == 10) v = (uint32_t)r.size.height; /* VERTRES */
    RETURN_STDCALL(v, 2);
}

static void MessageBoxA(void)
{
    const char *text = ARG(1) ? GSTR(ARG(1)) : "", *cap = ARG(2) ? GSTR(ARG(2)) : "";
    rt_log("MessageBoxA [%s] %s", cap, text);
    if (!getenv("GTA2_HEADLESS")) {
        user32_init();
        @autoreleasepool {
            NSAlert *a = [NSAlert new];
            a.messageText = [NSString stringWithCString:cap encoding:NSWindowsCP1252StringEncoding];
            a.informativeText = [NSString stringWithCString:text encoding:NSWindowsCP1252StringEncoding];
            [a runModal];
        }
    }
    RETURN_STDCALL(1 /* IDOK */, 4);
}

/* Dialogs: the game only uses these for its multiplayer/setup boxes. */
static void DialogBoxParamA(void)
{
    rt_log("DialogBoxParamA(template %08X) not supported", ARG(1));
    RETURN_STDCALL(2 /* IDCANCEL */, 5);
}
static void EndDialog(void) { RETURN_STDCALL(1, 2); }
static void GetDlgItem(void) { RETURN_STDCALL(0, 2); }
static void GetDlgCtrlID(void) { RETURN_STDCALL(0, 1); }
static void SetDlgItemTextA(void) { RETURN_STDCALL(1, 3); }
static void GetDlgItemTextA(void)
{
    if (ARG(3)) MEM8(ARG(2)) = 0;
    RETURN_STDCALL(0, 4);
}
static void SendDlgItemMessageA(void) { RETURN_STDCALL(0, 5); }

/* wsprintfA(buf, fmt, ...) -- cdecl; arguments follow on the guest stack */
static void wsprintfA(void)
{
    uint32_t buf = ARG(0);
    const char *f = GSTR(ARG(1));
    unsigned ai = 2;
    char out[1025];
    size_t n = 0;
    while (*f && n < 1024) {
        if (*f != '%') { out[n++] = *f++; continue; }
        const char *start = f++;
        if (*f == '%') { out[n++] = '%'; f++; continue; }
        while (*f && strchr("-+ #0", *f)) f++;
        while (*f >= '0' && *f <= '9') f++;
        if (*f == '.') { f++; while (*f >= '0' && *f <= '9') f++; }
        while (*f == 'l' || *f == 'h') f++;
        char conv = *f ? *f++ : 0;
        char spec[32];
        size_t sl = (size_t)(f - start);
        if (sl >= sizeof spec - 1) sl = sizeof spec - 2;
        memcpy(spec, start, sl);
        spec[sl] = 0;
        /* drop length modifiers; we pass 32-bit values */
        char clean[32];
        size_t c = 0;
        for (size_t i = 0; i < sl; i++) if (spec[i] != 'l' && spec[i] != 'h') clean[c++] = spec[i];
        clean[c] = 0;
        uint32_t v = ARG(ai++);
        int w;
        if (conv == 's') w = snprintf(out + n, sizeof out - n, clean, v ? GSTR(v) : "(null)");
        else if (conv == 'c') w = snprintf(out + n, sizeof out - n, clean, (int)v);
        else if (conv == 'd' || conv == 'i') w = snprintf(out + n, sizeof out - n, clean, (int32_t)v);
        else w = snprintf(out + n, sizeof out - n, clean, v);
        if (w > 0) n += (size_t)w < sizeof out - n ? (size_t)w : sizeof out - n - 1;
    }
    out[n] = 0;
    memcpy(GPTR(buf), out, n + 1);
    RETURN_CDECL(n);
}


/* ---- settings (replaces GTA2 Manager) ----------------------------------------
 * In v9.6 the main menu's OPTIONS launches the separate GTA2 Manager program
 * and the game quits. We show a native settings dialog instead, save to the
 * registry, and relaunch the app with the new settings. */

static const struct { int w, h; } g_res[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 },
};

static int show_settings(void)
{
    user32_init();
    int cur_w = (int)reg_get_dword("screen", "window_width", 640);
    int cur_h = (int)reg_get_dword("screen", "window_height", 480);
    @autoreleasepool {
        NSAlert *a = [NSAlert new];
        a.messageText = @"GTA2 Settings";
        a.informativeText = @"Gameplay resolution (menus stay 640 × 480). The game restarts to apply.";
        NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 260, 58)];
        NSPopUpButton *pop = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 30, 260, 26) pullsDown:NO];
        for (size_t i = 0; i < sizeof g_res / sizeof *g_res; i++) {
            NSString *t = [NSString stringWithFormat:@"%d × %d%@", g_res[i].w, g_res[i].h,
                           g_res[i].w * 3 == g_res[i].h * 4 ? @"" : @"  (widescreen)"];
            [pop addItemWithTitle:t];
            if (g_res[i].w == cur_w && g_res[i].h == cur_h) [pop selectItemAtIndex:(NSInteger)i];
        }
        NSButton *fs = [NSButton checkboxWithTitle:@"Full screen" target:nil action:nil];
        fs.frame = NSMakeRect(0, 0, 260, 22);
        fs.state = reg_get_dword("screen", "mac_fullscreen", 0) ? NSControlStateValueOn : NSControlStateValueOff;
        [v addSubview:pop];
        [v addSubview:fs];
        a.accessoryView = v;
        [a addButtonWithTitle:@"Apply & Restart"];
        [a addButtonWithTitle:@"Cancel"];
        if ([a runModal] != NSAlertFirstButtonReturn) return 0;
        NSInteger i = pop.indexOfSelectedItem;
        reg_set_dword("screen", "window_width", (uint32_t)g_res[i].w);
        reg_set_dword("screen", "window_height", (uint32_t)g_res[i].h);
        reg_set_dword("screen", "full_width", (uint32_t)g_res[i].w);
        reg_set_dword("screen", "full_height", (uint32_t)g_res[i].h);
        reg_set_dword("screen", "mac_fullscreen", fs.state == NSControlStateValueOn);
        rt_log("settings: %dx%d fullscreen %d", g_res[i].w, g_res[i].h, fs.state == NSControlStateValueOn);
    }
    return 1;
}

/* ShellExecuteA(hwnd, verb, file, params, dir, show) -> > 32 on success */
static void ShellExecuteA(void)
{
    const char *file = ARG(2) ? GSTR(ARG(2)) : "";
    if (!strcasecmp(file, "GTA2 Manager.EXE")) {
        if (show_settings()) {
            app_request_relaunch();
            RETURN_STDCALL(42, 6);
        }
        /* Cancelled: the game still quits after a "successful" launch, so
         * relaunch unchanged rather than leaving the user at the desktop. */
        app_request_relaunch();
        RETURN_STDCALL(42, 6);
    }
    rt_log("ShellExecuteA(%s): not supported", file);
    RETURN_STDCALL(2, 6);
}

const NativeExport g_user32_exports[] = {
    { "ShellExecuteA", ShellExecuteA },
    { "RegisterClassA", RegisterClassA },
    { "CreateWindowExA", CreateWindowExA },
    { "ShowWindow", ShowWindow },
    { "DestroyWindow", DestroyWindow },
    { "UpdateWindow", UpdateWindow },
    { "SetForegroundWindow", SetForegroundWindow },
    { "EnableWindow", EnableWindow },
    { "GetParent", GetParent },
    { "FindWindowExA", FindWindowExA },
    { "SetWindowPos", SetWindowPos },
    { "GetClientRect", GetClientRect },
    { "GetWindowRect", GetWindowRect },
    { "SetWindowTextA", SetWindowTextA },
    { "GetWindowTextA", GetWindowTextA },
    { "GetWindowLongA", GetWindowLongA },
    { "SetWindowLongA", SetWindowLongA },
    { "CallWindowProcA", CallWindowProcA },
    { "DefWindowProcA", DefWindowProcA },
    { "PeekMessageA", PeekMessageA },
    { "TranslateMessage", TranslateMessage },
    { "DispatchMessageA", DispatchMessageA },
    { "PostMessageA", PostMessageA },
    { "SendMessageA", SendMessageA },
    { "PostQuitMessage", PostQuitMessage },
    { "SetTimer", SetTimer },
    { "KillTimer", KillTimer },
    { "LoadIconA", LoadIconA },
    { "LoadCursorA", LoadCursorA },
    { "ShowCursor", ShowCursor },
    { "GetSystemMetrics", GetSystemMetrics },
    { "GetKeyboardLayoutNameA", GetKeyboardLayoutNameA },
    { "GetDC", GetDC },
    { "ReleaseDC", ReleaseDC },
    { "GetDeviceCaps", GetDeviceCaps },
    { "MessageBoxA", MessageBoxA },
    { "DialogBoxParamA", DialogBoxParamA },
    { "EndDialog", EndDialog },
    { "GetDlgItem", GetDlgItem },
    { "GetDlgCtrlID", GetDlgCtrlID },
    { "SetDlgItemTextA", SetDlgItemTextA },
    { "GetDlgItemTextA", GetDlgItemTextA },
    { "SendDlgItemMessageA", SendDlgItemMessageA },
    { "wsprintfA", wsprintfA },
    { NULL, NULL },
};
