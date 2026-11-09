/* DirectInput 8 (keyboard) as guest COM objects backed by native code.
 *
 * Each object is a guest struct whose first word points to a vtable of
 * synthetic thunk VAs; every method is stdcall with `this` first. The
 * keyboard reads g_key_down (filled from AppKit key events by user32.m) and
 * keeps a buffer of key transitions for GetDeviceData.
 */
#include <stdio.h>
#include <string.h>

#include "guest_memory.h"
#include "input/dinput.h"
#include "platform/keymap.h"
#include "win32/imports.h"

#define DI_OK 0u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOINTERFACE 0x80004002u
#define DIERR_DEVICENOTREG 0x80040154u
#define DI_BUFFEROVERFLOW 1u

/* GUID_SysKeyboard {6F1D2B61-D5A0-11CF-BFC7-444553540000} */
static const uint8_t GUID_SysKeyboard[16] = {
    0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00,
};

static uint32_t make_vtable(const char *iface, const NativeImpl *fns, const int *nargs, unsigned n)
{
    uint32_t vt = guest_alloc(4u * n, 1);
    for (unsigned i = 0; i < n; i++) {
        char name[64];
        snprintf(name, sizeof name, "%s::%u", iface, i);
        MEM32(vt + 4u * i) = imports_register_native(strdup(name), fns[i], nargs[i] * 4);
    }
    return vt;
}

/* ---- key event buffer ----------------------------------------------------- */

#define KBUF 256
static struct { uint8_t dik, down; uint32_t time; } g_kbuf[KBUF];
static unsigned g_khead, g_ktail;
static uint32_t g_kseq;
static int g_kbuf_overflow;

void dinput_key_event(uint8_t dik, int down, uint32_t time_ms)
{
    unsigned next = (g_ktail + 1) % KBUF;
    if (next == g_khead) {
        g_kbuf_overflow = 1;
        return;
    }
    g_kbuf[g_ktail].dik = dik;
    g_kbuf[g_ktail].down = (uint8_t)down;
    g_kbuf[g_ktail].time = time_ms;
    g_ktail = next;
}

/* ---- common IUnknown ------------------------------------------------------ */

static void Unk_QueryInterface(void) { if (ARG(2)) MEM32(ARG(2)) = 0; RETURN_STDCALL(DIERR_NOINTERFACE, 3); }
static void Unk_AddRef(void) { RETURN_STDCALL(1, 1); }
static void Unk_Release(void) { RETURN_STDCALL(0, 1); }
static void Ok1(void) { RETURN_STDCALL(DI_OK, 1); }
static void Ok2(void) { RETURN_STDCALL(DI_OK, 2); }
static void Ok3(void) { RETURN_STDCALL(DI_OK, 3); }
static void Ok4(void) { RETURN_STDCALL(DI_OK, 4); }
static void Ok5(void) { RETURN_STDCALL(DI_OK, 5); }
static void Ok6(void) { RETURN_STDCALL(DI_OK, 6); }
static void Unsupported2(void) { RETURN_STDCALL(DIERR_INVALIDPARAM, 2); }
static void Unsupported3(void) { RETURN_STDCALL(DIERR_INVALIDPARAM, 3); }
static void Unsupported4(void) { RETURN_STDCALL(DIERR_INVALIDPARAM, 4); }
static void Unsupported5(void) { RETURN_STDCALL(DIERR_INVALIDPARAM, 5); }

/* ---- IDirectInputDevice8A (keyboard) --------------------------------------- */

static uint32_t g_keyboard; /* guest object */

/* GetDeviceState(this, cbData, lpvData) */
static void Dev_GetDeviceState(void)
{
    uint32_t n = ARG(1), out = ARG(2);
    if (n > 256) n = 256;
    memcpy(GPTR(out), g_key_down, n);
    RETURN_STDCALL(DI_OK, 3);
}

/* GetDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags) */
static void Dev_GetDeviceData(void)
{
    uint32_t size = ARG(1), out = ARG(2), inout = ARG(3), flags = ARG(4);
    uint32_t want = MEM32(inout), got = 0;
    int peek = (flags & 1u) != 0; /* DIGDD_PEEK */
    unsigned h = g_khead;
    while (h != g_ktail && (got < want || !out)) {
        if (out) {
            uint32_t p = out + got * size;
            memset(GPTR(p), 0, size);
            MEM32(p + 0) = g_kbuf[h].dik;                /* dwOfs = DIK code */
            MEM32(p + 4) = g_kbuf[h].down ? 0x80u : 0u;  /* dwData */
            MEM32(p + 8) = g_kbuf[h].time;               /* dwTimeStamp */
            MEM32(p + 12) = g_kseq++;                    /* dwSequence */
        }
        got++;
        h = (h + 1) % KBUF;
        if (!out && got >= want) break;
    }
    if (!peek) g_khead = h;
    MEM32(inout) = got;
    uint32_t r = g_kbuf_overflow ? DI_BUFFEROVERFLOW : DI_OK;
    g_kbuf_overflow = 0;
    RETURN_STDCALL(r, 5);
}

/* GetCapabilities(this, DIDEVCAPS*) */
static void Dev_GetCapabilities(void)
{
    uint32_t caps = ARG(1);
    uint32_t sz = MEM32(caps);
    memset(GPTR(caps + 4), 0, sz > 4 ? sz - 4 : 0);
    MEM32(caps + 4) = 1;          /* DIDC_ATTACHED */
    MEM32(caps + 8) = 0x13 | 0x100; /* DI8DEVTYPE_KEYBOARD | subtype */
    MEM32(caps + 16) = 128;       /* buttons */
    RETURN_STDCALL(DI_OK, 2);
}

/* GetDeviceInfo(this, DIDEVICEINSTANCEA*) */
static void Dev_GetDeviceInfo(void)
{
    uint32_t p = ARG(1);
    uint32_t sz = MEM32(p);
    memset(GPTR(p + 4), 0, sz > 4 ? sz - 4 : 0);
    memcpy(GPTR(p + 4), GUID_SysKeyboard, 16);
    memcpy(GPTR(p + 20), GUID_SysKeyboard, 16);
    MEM32(p + 36) = 0x13;
    strcpy((char *)GPTR(p + 40), "Keyboard");
    strcpy((char *)GPTR(p + 300), "Keyboard");
    RETURN_STDCALL(DI_OK, 2);
}

static uint32_t create_keyboard(void)
{
    if (g_keyboard) return g_keyboard;
    static const NativeImpl fns[32] = {
        Unk_QueryInterface, Unk_AddRef, Unk_Release, Dev_GetCapabilities, Ok4 /* EnumObjects */,
        Unsupported3 /* GetProperty */, Ok3 /* SetProperty */, Ok1 /* Acquire */, Ok1 /* Unacquire */,
        Dev_GetDeviceState, Dev_GetDeviceData, Ok2 /* SetDataFormat */, Ok2 /* SetEventNotification */,
        Ok3 /* SetCooperativeLevel */, Unsupported4 /* GetObjectInfo */, Dev_GetDeviceInfo,
        Ok3 /* RunControlPanel */, Ok4 /* Initialize */, Unsupported5 /* CreateEffect */,
        Ok4 /* EnumEffects */, Unsupported3 /* GetEffectInfo */, Unsupported2 /* GetForceFeedbackState */,
        Unsupported2 /* SendForceFeedbackCommand */, Ok4 /* EnumCreatedEffectObjects */,
        Unsupported2 /* Escape */, Ok1 /* Poll */, Ok5 /* SendDeviceData */, Ok5 /* EnumEffectsInFile */,
        Ok5 /* WriteEffectToFile */, Ok4 /* BuildActionMap */, Ok4 /* SetActionMap */,
        Unsupported2 /* GetImageInfo */,
    };
    static const int nargs[32] = { 3, 1, 1, 2, 4, 3, 3, 1, 1, 3, 5, 2, 2, 3, 4, 2,
                                   3, 4, 5, 4, 3, 2, 2, 4, 2, 1, 5, 5, 5, 4, 4, 2 };
    g_keyboard = guest_alloc(16, 1);
    MEM32(g_keyboard) = make_vtable("IDirectInputDevice8A", fns, nargs, 32);
    return g_keyboard;
}

/* ---- IDirectInput8A --------------------------------------------------------- */

/* CreateDevice(this, rguid, lplpDevice, pUnkOuter) */
static void DI_CreateDevice(void)
{
    uint32_t guid = ARG(1), out = ARG(2);
    if (!memcmp(GPTR(guid), GUID_SysKeyboard, 16)) {
        MEM32(out) = create_keyboard();
        RETURN_STDCALL(DI_OK, 4);
    }
    rt_log("dinput: CreateDevice for an unsupported device");
    MEM32(out) = 0;
    RETURN_STDCALL(DIERR_DEVICENOTREG, 4);
}

/* EnumDevices(this, devType, callback, ref, flags): only the keyboard exists
 * for now; game controllers arrive with the GameController backend. */
static void DI_EnumDevices(void) { RETURN_STDCALL(DI_OK, 5); }
static void DI_GetDeviceStatus(void) { RETURN_STDCALL(DI_OK, 2); }

static uint32_t g_dinput;

/* DirectInput8Create(hinst, version, riid, ppvOut, punkOuter) */
static void DirectInput8Create(void)
{
    if (!g_dinput) {
        static const NativeImpl fns[11] = {
            Unk_QueryInterface, Unk_AddRef, Unk_Release, DI_CreateDevice, DI_EnumDevices,
            DI_GetDeviceStatus, Ok3 /* RunControlPanel */, Ok3 /* Initialize */,
            Unsupported4 /* FindDevice */, Ok6 /* EnumDevicesBySemantics */, Ok5 /* ConfigureDevices */,
        };
        static const int nargs[11] = { 3, 1, 1, 4, 5, 2, 3, 3, 4, 6, 5 };
        g_dinput = guest_alloc(16, 1);
        MEM32(g_dinput) = make_vtable("IDirectInput8A", fns, nargs, 11);
    }
    MEM32(ARG(3)) = g_dinput;
    RETURN_STDCALL(DI_OK, 5);
}

const NativeExport g_dinput_exports[] = {
    { "DirectInput8Create", DirectInput8Create },
    { NULL, NULL },
};
