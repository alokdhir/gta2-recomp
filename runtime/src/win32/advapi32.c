/* ADVAPI32 registry, backed by a CoreFoundation property list.
 *
 * The game keeps its settings under HKLM/HKCU\Software\DMA Design Ltd\GTA2\*.
 * Hives are merged (the game reads some values from HKLM and some from HKCU),
 * "Wow6432Node" is ignored, and key paths compare case-insensitively.
 * Stored at ~/Library/Application Support/GTA2/registry.plist as
 *   { "software\dma design ltd\gta2\screen": { "rendername": <data|number|string> } }
 * where REG_DWORD -> CFNumber, REG_SZ -> CFString, anything else -> CFData.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "guest_memory.h"
#include "win32/imports.h"
#include "win32/win32.h"

#define REG_SZ 1u
#define REG_BINARY 3u
#define REG_DWORD 4u
#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_MORE_DATA 234u

static CFMutableDictionaryRef g_reg; /* key path (lowercase) -> CFMutableDictionary of values */
static char g_reg_path[1024];

static const struct { const char *key, *name; uint32_t type; uint32_t dword; const char *sz; } g_defaults[] = {
    /* Defaults as GTA2 Manager writes them on first run (v9.6) */
    { "screen", "rendername", REG_SZ, 0, "d3ddll.dll" },
    { "screen", "videoname", REG_SZ, 0, "dmavideo.dll" },
    { "screen", "renderdevice", REG_DWORD, 2, NULL },
    { "screen", "videodevice", REG_DWORD, 1, NULL },
    { "screen", "curr_dll_id", REG_DWORD, 1, NULL },
    { "screen", "do_play_movie", REG_DWORD, 0, NULL }, /* no Bink yet */
    { "screen", "exploding_on", REG_DWORD, 1, NULL },
    { "screen", "gamma", REG_DWORD, 10, NULL },
    { "screen", "lighting", REG_DWORD, 1, NULL },
    { "screen", "max_frame_rate", REG_DWORD, 1, NULL },
    { "screen", "min_frame_rate", REG_DWORD, 0, NULL },
    { "screen", "special_recognition", REG_DWORD, 1, NULL },
    { "screen", "start_mode", REG_DWORD, 1, NULL },
    { "screen", "tripple_buffer", REG_DWORD, 0, NULL },
    { "option", "language", REG_SZ, 0, "e" },
    { "option", "text_speed", REG_DWORD, 3, NULL },
    { "sound", "cdvol", REG_DWORD, 0x7F, NULL },
    { "sound", "sfxvol", REG_DWORD, 0x7F, NULL },
    { "network", "playername", REG_SZ, 0, "PLAYER" },
    { "network", "protocol_selected", REG_DWORD, 0, NULL },
    { "network", "show_player_names", REG_DWORD, 1, NULL },
};

static CFStringRef cfstr(const char *s)
{
    return CFStringCreateWithCString(NULL, s, kCFStringEncodingWindowsLatin1);
}

/* "HKLM" + "SOFTWARE\Wow6432Node\DMA Design Ltd\GTA2\Screen" -> "software\dma design ltd\gta2\screen" */
static char *normalise(const char *base, const char *sub)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s%s%s", base, *base && sub && *sub ? "\\" : "", sub ? sub : "");
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    char *w;
    while ((w = strstr(buf, "wow6432node\\"))) memmove(w, w + 12, strlen(w + 12) + 1);
    size_t n = strlen(buf);
    while (n && buf[n - 1] == '\\') buf[--n] = 0;
    return strdup(buf);
}

static void reg_save(void)
{
    CFDataRef data = CFPropertyListCreateData(NULL, g_reg, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    if (!data) return;
    FILE *f = fopen(g_reg_path, "wb");
    if (f) {
        fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), f);
        fclose(f);
    }
    CFRelease(data);
}

static CFMutableDictionaryRef key_dict(const char *path, int create)
{
    CFStringRef k = cfstr(path);
    CFMutableDictionaryRef d = (CFMutableDictionaryRef)CFDictionaryGetValue(g_reg, k);
    if (!d && create) {
        d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(g_reg, k, d);
        CFRelease(d);
    }
    CFRelease(k);
    return d;
}

static void reg_init(void)
{
    if (g_reg) return;
    const char *home = getenv("HOME");
    char dir[900];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/GTA2", home ? home : ".");
    mkdir(dir, 0755);
    snprintf(g_reg_path, sizeof g_reg_path, "%s/registry.plist", dir);

    FILE *f = fopen(g_reg_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)n);
        if (fread(buf, 1, (size_t)n, f) == (size_t)n) {
            CFDataRef data = CFDataCreate(NULL, buf, n);
            g_reg = (CFMutableDictionaryRef)CFPropertyListCreateWithData(
                NULL, data, kCFPropertyListMutableContainers, NULL, NULL);
            CFRelease(data);
        }
        free(buf);
        fclose(f);
        if (g_reg && CFGetTypeID(g_reg) != CFDictionaryGetTypeID()) { CFRelease(g_reg); g_reg = NULL; }
    }
    if (g_reg) return;

    g_reg = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    for (size_t i = 0; i < sizeof g_defaults / sizeof *g_defaults; i++) {
        char path[256];
        snprintf(path, sizeof path, "software\\dma design ltd\\gta2\\%s", g_defaults[i].key);
        CFMutableDictionaryRef d = key_dict(path, 1);
        CFStringRef name = cfstr(g_defaults[i].name);
        CFTypeRef v;
        if (g_defaults[i].type == REG_DWORD) {
            int32_t x = (int32_t)g_defaults[i].dword;
            v = CFNumberCreate(NULL, kCFNumberSInt32Type, &x);
        } else {
            v = cfstr(g_defaults[i].sz);
        }
        CFDictionarySetValue(d, name, v);
        CFRelease(v);
        CFRelease(name);
    }
    reg_save();
    rt_log("registry: created %s with defaults", g_reg_path);
}

static const char *hive_path(uint32_t hkey)
{
    if (hkey >= 0x80000000u && hkey <= 0x80000006u) return ""; /* predefined hives merge */
    Win32Handle *h = win32_get_handle(hkey, WIN32_H_REGKEY);
    return h ? h->path : NULL;
}

static uint32_t open_key(uint32_t hkey, uint32_t subkey, uint32_t out, int create)
{
    reg_init();
    const char *base = hive_path(hkey);
    if (!base) return 6; /* ERROR_INVALID_HANDLE */
    char *path = normalise(base, subkey ? GSTR(subkey) : "");
    if (!key_dict(path, create)) {
        free(path);
        return ERROR_FILE_NOT_FOUND;
    }
    uint32_t h = win32_new_handle(WIN32_H_REGKEY, 0);
    win32_get_handle(h, WIN32_H_REGKEY)->path = path;
    MEM32(out) = h;
    return ERROR_SUCCESS;
}

/* RegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult) */
static void RegOpenKeyExA(void) { RETURN_STDCALL(open_key(ARG(0), ARG(1), ARG(4), 0), 5); }

/* RegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSA, phkResult, lpdwDisposition) */
static void RegCreateKeyExA(void)
{
    uint32_t r = open_key(ARG(0), ARG(1), ARG(7), 1);
    if (ARG(8)) MEM32(ARG(8)) = 2; /* REG_OPENED_EXISTING_KEY */
    RETURN_STDCALL(r, 9);
}

static void RegCloseKey(void)
{
    if (win32_get_handle(ARG(0), WIN32_H_REGKEY)) win32_close_handle(ARG(0));
    RETURN_STDCALL(ERROR_SUCCESS, 1);
}

static CFStringRef value_name(uint32_t va)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", va ? GSTR(va) : "");
    for (char *p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    return cfstr(buf);
}

/* RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData) */
static void RegQueryValueExA(void)
{
    Win32Handle *h = win32_get_handle(ARG(0), WIN32_H_REGKEY);
    uint32_t typep = ARG(3), data = ARG(4), cbp = ARG(5);
    if (!h) RETURN_STDCALL(6, 6);
    CFMutableDictionaryRef d = key_dict(h->path, 0);
    CFStringRef name = value_name(ARG(1));
    CFTypeRef v = d ? CFDictionaryGetValue(d, name) : NULL;
    if (!v) {
        char nb[128];
        CFStringGetCString(name, nb, sizeof nb, kCFStringEncodingWindowsLatin1);
        rt_log("registry: %s\\%s not set", h->path, nb);
    }
    CFRelease(name);
    if (!v) RETURN_STDCALL(ERROR_FILE_NOT_FOUND, 6);

    uint8_t tmp[2048];
    uint32_t type, n;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        int32_t x;
        CFNumberGetValue(v, kCFNumberSInt32Type, &x);
        memcpy(tmp, &x, 4);
        type = REG_DWORD;
        n = 4;
    } else if (CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString(v, (char *)tmp, sizeof tmp, kCFStringEncodingWindowsLatin1);
        type = REG_SZ;
        n = (uint32_t)strlen((char *)tmp) + 1;
    } else {
        n = (uint32_t)CFDataGetLength(v);
        if (n > sizeof tmp) n = sizeof tmp;
        CFDataGetBytes(v, CFRangeMake(0, n), tmp);
        type = REG_BINARY;
    }
    if (typep) MEM32(typep) = type;
    uint32_t r = ERROR_SUCCESS;
    if (data) {
        if (!cbp || MEM32(cbp) < n) r = ERROR_MORE_DATA;
        else memcpy(GPTR(data), tmp, n);
    }
    if (cbp) MEM32(cbp) = n;
    RETURN_STDCALL(r, 6);
}

/* RegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData) */
static void RegSetValueExA(void)
{
    Win32Handle *h = win32_get_handle(ARG(0), WIN32_H_REGKEY);
    uint32_t type = ARG(3), data = ARG(4), n = ARG(5);
    if (!h) RETURN_STDCALL(6, 6);
    CFMutableDictionaryRef d = key_dict(h->path, 1);
    CFStringRef name = value_name(ARG(1));
    CFTypeRef v;
    if (type == REG_DWORD && n >= 4) {
        int32_t x = (int32_t)MEM32(data);
        v = CFNumberCreate(NULL, kCFNumberSInt32Type, &x);
    } else if (type == REG_SZ) {
        v = cfstr(GSTR(data));
    } else {
        v = CFDataCreate(NULL, GPTR(data), n);
    }
    CFDictionarySetValue(d, name, v);
    CFRelease(v);
    CFRelease(name);
    reg_save();
    RETURN_STDCALL(ERROR_SUCCESS, 6);
}

static void RegDeleteValueA(void)
{
    Win32Handle *h = win32_get_handle(ARG(0), WIN32_H_REGKEY);
    CFMutableDictionaryRef d = h ? key_dict(h->path, 0) : NULL;
    if (d) {
        CFStringRef name = value_name(ARG(1));
        CFDictionaryRemoveValue(d, name);
        CFRelease(name);
        reg_save();
    }
    RETURN_STDCALL(ERROR_SUCCESS, 2);
}

static void GetUserNameA(void)
{
    const char *u = "PLAYER";
    uint32_t buf = ARG(0), cbp = ARG(1), n = (uint32_t)strlen(u) + 1;
    if (MEM32(cbp) < n) { MEM32(cbp) = n; RETURN_STDCALL(0, 2); }
    memcpy(GPTR(buf), u, n);
    MEM32(cbp) = n;
    RETURN_STDCALL(1, 2);
}

const NativeExport g_advapi32_exports[] = {
    { "RegOpenKeyExA", RegOpenKeyExA },
    { "RegCreateKeyExA", RegCreateKeyExA },
    { "RegCloseKey", RegCloseKey },
    { "RegQueryValueExA", RegQueryValueExA },
    { "RegSetValueExA", RegSetValueExA },
    { "RegDeleteValueA", RegDeleteValueA },
    { "GetUserNameA", GetUserNameA },
    { NULL, NULL },
};

/* Native access for our own settings UI. key is relative to
 * Software\DMA Design Ltd\GTA2 (e.g. "screen"). */
uint32_t reg_get_dword(const char *key, const char *name, uint32_t def)
{
    reg_init();
    char path[256];
    snprintf(path, sizeof path, "software\\dma design ltd\\gta2\\%s", key);
    CFMutableDictionaryRef d = key_dict(path, 0);
    CFStringRef n = cfstr(name);
    CFTypeRef v = d ? CFDictionaryGetValue(d, n) : NULL;
    CFRelease(n);
    int32_t x = (int32_t)def;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberSInt32Type, &x);
    return (uint32_t)x;
}

void reg_set_dword(const char *key, const char *name, uint32_t value)
{
    reg_init();
    char path[256];
    snprintf(path, sizeof path, "software\\dma design ltd\\gta2\\%s", key);
    CFMutableDictionaryRef d = key_dict(path, 1);
    CFStringRef n = cfstr(name);
    int32_t x = (int32_t)value;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &x);
    CFDictionarySetValue(d, n, v);
    CFRelease(v);
    CFRelease(n);
    reg_save();
}
