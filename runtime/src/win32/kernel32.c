/* KERNEL32 for the recompiled game: process, heap, memory, time, strings.
 * File I/O lives in kernel32_file.c. */
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "guest_memory.h"
#include "win32/imports.h"
#include "win32/win32.h"

static uint32_t g_last_error;
void win32_set_last_error(uint32_t e) { g_last_error = e; }

/* ---- process -------------------------------------------------------------- */

static void GetVersionExA(void)
{
    uint32_t p = ARG(0); /* OSVERSIONINFOA: report Windows XP SP3, NT platform */
    MEM32(p + 4) = 5;
    MEM32(p + 8) = 1;
    MEM32(p + 12) = 2600;
    MEM32(p + 16) = 2; /* VER_PLATFORM_WIN32_NT */
    strcpy((char *)GPTR(p + 20), "Service Pack 3");
    RETURN_STDCALL(1, 1);
}

static uint32_t g_cmdline, g_env_a, g_env_w;

static void GetCommandLineA(void)
{
    if (!g_cmdline) g_cmdline = guest_strdup("gta2.exe");
    RETURN_STDCALL(g_cmdline, 0);
}

static void GetEnvironmentStrings(void)
{
    if (!g_env_a) {
        g_env_a = guest_alloc(2, 1); /* empty block: "\0\0" */
    }
    RETURN_STDCALL(g_env_a, 0);
}

static void GetEnvironmentStringsW(void)
{
    if (!g_env_w) g_env_w = guest_alloc(4, 1);
    RETURN_STDCALL(g_env_w, 0);
}

static void FreeEnvironmentStrings(void) { RETURN_STDCALL(1, 1); }

static void GetStartupInfoA(void)
{
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 68);
    MEM32(p) = 68;
    RETURN_STDCALL(0, 1);
}

static void GetStdHandle(void)
{
    int32_t which = (int32_t)ARG(0); /* -10 in, -11 out, -12 err */
    RETURN_STDCALL(which >= -12 && which <= -10 ? win32_std_handle(-10 - which) : 0xFFFFFFFFu, 1);
}

static void SetStdHandle(void) { RETURN_STDCALL(1, 2); }
static void SetHandleCount(void) { RETURN_STDCALL(ARG(0), 1); }

static void GetModuleFileNameA(void)
{
    uint32_t buf = ARG(1), size = ARG(2);
    const char *name = "C:\\GTA2\\gta2.exe";
    uint32_t n = (uint32_t)strlen(name);
    if (n >= size) n = size ? size - 1 : 0;
    memcpy(GPTR(buf), name, n);
    if (size) MEM8(buf + n) = 0;
    RETURN_STDCALL(n, 3);
}

static void GetModuleHandleA(void)
{
    /* NULL = the exe itself. Named modules: none are "already loaded". */
    RETURN_STDCALL(ARG(0) ? 0 : GUEST_IMAGE_BASE, 1);
}

static void GetLastError(void) { RETURN_STDCALL(g_last_error, 0); }
static void GetCurrentProcess(void) { RETURN_STDCALL(0xFFFFFFFFu, 0); }
static void GetCurrentProcessId(void) { RETURN_STDCALL(0x1000, 0); }
static void GetCurrentThreadId(void) { RETURN_STDCALL(0x1004, 0); }

static void ExitProcess(void)
{
    rt_log("ExitProcess(%u)", ARG(0));
    app_exit_hook();
    exit((int)ARG(0));
}

static void TerminateProcess(void)
{
    rt_log("TerminateProcess(%u)", ARG(1));
    exit((int)ARG(1));
}

static void SetUnhandledExceptionFilter(void) { RETURN_STDCALL(0, 1); }
static void UnhandledExceptionFilter(void) { RETURN_STDCALL(0, 1); }  /* EXCEPTION_CONTINUE_SEARCH */
static void SetConsoleCtrlHandler(void) { RETURN_STDCALL(1, 2); }

static void RtlUnwind(void)
{
    /* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): we never
     * raise SEH exceptions, so there is nothing to unwind; pop the chain to
     * the target frame as Windows would. */
    uint32_t target = ARG(0);
    if (target) MEM32(g_fs_base) = target;
    RETURN_STDCALL(0, 4);
}

static void OutputDebugStringA(void)
{
    rt_log("debug: %s", GSTR(ARG(0)));
    RETURN_STDCALL(0, 1);
}

static void IsBadPtr2(void) { RETURN_STDCALL(ARG(0) < 0x10000u, 2); }
static void IsBadCodePtr(void) { RETURN_STDCALL(ARG(0) < 0x10000u, 1); }

/* ---- heap & virtual memory ------------------------------------------------ */

#define HEAP_ZERO_MEMORY 0x8u

static void HeapCreate(void) { RETURN_STDCALL(0x00480000u /* any nonzero token */, 3); }
static void HeapDestroy(void) { RETURN_STDCALL(1, 1); }

static void HeapAlloc(void)
{
    RETURN_STDCALL(guest_alloc(ARG(2), (ARG(1) & HEAP_ZERO_MEMORY) != 0), 3);
}

static void HeapFree(void)
{
    guest_free(ARG(2));
    RETURN_STDCALL(1, 3);
}

static void HeapReAlloc(void)
{
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3);
    uint32_t old = guest_alloc_size(p);
    if (flags & 0x10u /* HEAP_REALLOC_IN_PLACE_ONLY */)
        RETURN_STDCALL(size <= old ? p : 0, 4);
    uint32_t n = guest_realloc(p, size);
    if (n && (flags & HEAP_ZERO_MEMORY) && size > old)
        memset(GPTR(n + old), 0, size - old);
    RETURN_STDCALL(n, 4);
}

static void HeapSize(void)
{
    uint32_t p = ARG(2);
    RETURN_STDCALL(p ? guest_alloc_size(p) : 0xFFFFFFFFu, 3);
}

static void VirtualAlloc(void)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    /* MEM_COMMIT of an already reserved range: the whole guest space is RW */
    if (addr && (type & 0x1000u) && !(type & 0x2000u)) RETURN_STDCALL(addr, 4);
    uint32_t p = guest_alloc((size + 0xFFFu) & ~0xFFFu, 1);
    RETURN_STDCALL(p, 4);
}

static void VirtualFree(void)
{
    if (ARG(2) & 0x8000u /* MEM_RELEASE */) guest_free(ARG(0));
    RETURN_STDCALL(1, 3);
}

static void VirtualProtect(void)
{
    if (ARG(3)) MEM32(ARG(3)) = 0x04; /* PAGE_READWRITE */
    RETURN_STDCALL(1, 4);
}

static void VirtualQuery(void)
{
    uint32_t addr = ARG(0), info = ARG(1);
    memset(GPTR(info), 0, 28);
    MEM32(info + 0) = addr & ~0xFFFu;
    MEM32(info + 4) = addr & ~0xFFFu;
    MEM32(info + 8) = 0x04;
    MEM32(info + 12) = 0x1000;
    MEM32(info + 16) = 0x1000; /* MEM_COMMIT */
    MEM32(info + 20) = 0x04;
    MEM32(info + 24) = 0x20000; /* MEM_PRIVATE */
    RETURN_STDCALL(28, 3);
}

static void GetSystemInfo(void)
{
    uint32_t p = ARG(0);
    memset(GPTR(p), 0, 36);
    MEM32(p + 4) = 0x1000;          /* page size */
    MEM32(p + 8) = 0x00010000;      /* min app address */
    MEM32(p + 12) = 0x7FFEFFFF;     /* max app address */
    MEM32(p + 16) = 1;              /* active processor mask */
    MEM32(p + 20) = 1;              /* processors */
    MEM32(p + 24) = 586;            /* PROCESSOR_INTEL_PENTIUM */
    MEM32(p + 28) = 0x10000;        /* allocation granularity */
    MEM16(p + 32) = 6;
    RETURN_STDCALL(0, 1);
}

static void LocalFree(void)
{
    guest_free(ARG(0));
    RETURN_STDCALL(0, 1);
}

/* ---- time ----------------------------------------------------------------- */

static uint64_t now_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

static void GetTickCount(void) { RETURN_STDCALL((uint32_t)(now_ns() / 1000000u), 0); }
static void timeGetTime(void) { RETURN_STDCALL((uint32_t)(now_ns() / 1000000u), 0); }

static void QueryPerformanceCounter(void)
{
    uint64_t t = now_ns() / 100u; /* 10 MHz, like modern Windows */
    MEM32(ARG(0)) = (uint32_t)t;
    MEM32(ARG(0) + 4) = (uint32_t)(t >> 32);
    RETURN_STDCALL(1, 1);
}

static void Sleep(void)
{
    usleep(ARG(0) * 1000u);
    RETURN_STDCALL(0, 1);
}

static uint64_t filetime_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)tv.tv_usec * 10u;
}

static void GetSystemTimeAsFileTime(void)
{
    uint64_t ft = filetime_now();
    MEM32(ARG(0)) = (uint32_t)ft;
    MEM32(ARG(0) + 4) = (uint32_t)(ft >> 32);
    RETURN_STDCALL(0, 1);
}

static void FileTimeToLocalFileTime(void)
{
    uint64_t ft = MEM32(ARG(0)) | (uint64_t)MEM32(ARG(0) + 4) << 32;
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    ft += (uint64_t)((int64_t)lt.tm_gmtoff * 10000000);
    MEM32(ARG(1)) = (uint32_t)ft;
    MEM32(ARG(1) + 4) = (uint32_t)(ft >> 32);
    RETURN_STDCALL(1, 2);
}

static void FileTimeToSystemTime(void)
{
    uint64_t ft = MEM32(ARG(0)) | (uint64_t)MEM32(ARG(0) + 4) << 32;
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm g;
    gmtime_r(&t, &g);
    uint32_t s = ARG(1);
    MEM16(s + 0) = (uint16_t)(g.tm_year + 1900);
    MEM16(s + 2) = (uint16_t)(g.tm_mon + 1);
    MEM16(s + 4) = (uint16_t)g.tm_wday;
    MEM16(s + 6) = (uint16_t)g.tm_mday;
    MEM16(s + 8) = (uint16_t)g.tm_hour;
    MEM16(s + 10) = (uint16_t)g.tm_min;
    MEM16(s + 12) = (uint16_t)g.tm_sec;
    MEM16(s + 14) = (uint16_t)((ft / 10000u) % 1000u);
    RETURN_STDCALL(1, 2);
}

static void GetTimeZoneInformation(void)
{
    memset(GPTR(ARG(0)), 0, 172);
    RETURN_STDCALL(0, 1); /* TIME_ZONE_ID_UNKNOWN */
}

/* ---- code pages & strings (the game is single-byte, code page 1252) -------- */

static void GetACP(void) { RETURN_STDCALL(1252, 0); }
static void GetOEMCP(void) { RETURN_STDCALL(437, 0); }

static void GetCPInfo(void)
{
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 20);
    MEM32(p) = 1;        /* MaxCharSize */
    MEM8(p + 4) = '?';   /* DefaultChar */
    RETURN_STDCALL(1, 2);
}

static void lstrlenA(void)
{
    RETURN_STDCALL(ARG(0) ? strlen(GSTR(ARG(0))) : 0, 1);
}

static void MultiByteToWideChar(void)
{
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (!cap) RETURN_STDCALL(n, 6);
    if ((uint32_t)n > cap) n = (int32_t)cap;
    for (int32_t i = 0; i < n; i++) MEM16(dst + 2u * (uint32_t)i) = MEM8(src + (uint32_t)i);
    RETURN_STDCALL(n, 6);
}

static void WideCharToMultiByte(void)
{
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) {
        n = 0;
        while (MEM16(src + 2u * (uint32_t)n)) n++;
        n++;
    }
    if (!cap) RETURN_STDCALL(n, 8);
    if ((uint32_t)n > cap) n = (int32_t)cap;
    for (int32_t i = 0; i < n; i++) {
        uint16_t c = MEM16(src + 2u * (uint32_t)i);
        MEM8(dst + (uint32_t)i) = c < 256 ? (uint8_t)c : '?';
    }
    RETURN_STDCALL(n, 8);
}

/* Character type bits for CT_CTYPE1 */
static uint16_t ctype1(unsigned c)
{
    uint16_t t = 0;
    if (c >= 'A' && c <= 'Z') t |= 0x001 | 0x100;
    if (c >= 'a' && c <= 'z') t |= 0x002 | 0x100;
    if (c >= '0' && c <= '9') t |= 0x004;
    if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x008;
    if (c == ' ' || c == '\t') t |= 0x040;
    if (c < 32 || c == 127) t |= 0x020;
    if ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~'))
        t |= 0x010;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= 0x080;
    return t;
}

static void GetStringTypeA(void)
{
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    for (int32_t i = 0; i < n; i++) MEM16(out + 2u * (uint32_t)i) = ctype1(MEM8(src + (uint32_t)i));
    RETURN_STDCALL(1, 5);
}

static void GetStringTypeW(void)
{
    uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) {
        n = 0;
        while (MEM16(src + 2u * (uint32_t)n)) n++;
        n++;
    }
    for (int32_t i = 0; i < n; i++) {
        uint16_t c = MEM16(src + 2u * (uint32_t)i);
        MEM16(out + 2u * (uint32_t)i) = c < 128 ? ctype1(c) : 0;
    }
    RETURN_STDCALL(1, 4);
}

static unsigned map_case(unsigned c, uint32_t flags)
{
    if ((flags & 0x100u) && c >= 'A' && c <= 'Z') return c + 32; /* LCMAP_LOWERCASE */
    if ((flags & 0x200u) && c >= 'a' && c <= 'z') return c - 32; /* LCMAP_UPPERCASE */
    return c;
}

static void LCMapStringA(void)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (!cap) RETURN_STDCALL(n, 6);
    if ((uint32_t)n > cap) RETURN_STDCALL(0, 6);
    for (int32_t i = 0; i < n; i++) MEM8(dst + (uint32_t)i) = (uint8_t)map_case(MEM8(src + (uint32_t)i), flags);
    RETURN_STDCALL(n, 6);
}

static void LCMapStringW(void)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) {
        n = 0;
        while (MEM16(src + 2u * (uint32_t)n)) n++;
        n++;
    }
    if (!cap) RETURN_STDCALL(n, 6);
    if ((uint32_t)n > cap) RETURN_STDCALL(0, 6);
    for (int32_t i = 0; i < n; i++)
        MEM16(dst + 2u * (uint32_t)i) = (uint16_t)map_case(MEM16(src + 2u * (uint32_t)i), flags);
    RETURN_STDCALL(n, 6);
}

static int cmp_ci(unsigned a, unsigned b, int ci)
{
    if (ci) { a = map_case(a, 0x100); b = map_case(b, 0x100); }
    return (int)a - (int)b;
}

static void CompareStringA(void)
{
    int ci = (ARG(1) & 1u) != 0; /* NORM_IGNORECASE */
    uint32_t a = ARG(2), b = ARG(4);
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) na = (int32_t)strlen(GSTR(a));
    if (nb < 0) nb = (int32_t)strlen(GSTR(b));
    int32_t n = na < nb ? na : nb, r = 0;
    for (int32_t i = 0; i < n && !r; i++) r = cmp_ci(MEM8(a + (uint32_t)i), MEM8(b + (uint32_t)i), ci);
    if (!r) r = na - nb;
    RETURN_STDCALL(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}

static void CompareStringW(void)
{
    int ci = (ARG(1) & 1u) != 0;
    uint32_t a = ARG(2), b = ARG(4);
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) for (na = 0; MEM16(a + 2u * (uint32_t)na); na++) {}
    if (nb < 0) for (nb = 0; MEM16(b + 2u * (uint32_t)nb); nb++) {}
    int32_t n = na < nb ? na : nb, r = 0;
    for (int32_t i = 0; i < n && !r; i++)
        r = cmp_ci(MEM16(a + 2u * (uint32_t)i), MEM16(b + 2u * (uint32_t)i), ci);
    if (!r) r = na - nb;
    RETURN_STDCALL(r < 0 ? 1 : r > 0 ? 3 : 2, 6);
}

static void GetLocaleInfoA(void)
{
    uint32_t type = ARG(1) & 0xFFFFu, dst = ARG(2), cap = ARG(3);
    const char *v = "";
    switch (type) {
    case 0x0B: v = "437"; break;   /* LOCALE_IDEFAULTCODEPAGE */
    case 0x1004: v = "1252"; break; /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x0E: v = "."; break;     /* LOCALE_SDECIMAL */
    case 0x0F: v = ","; break;     /* LOCALE_STHOUSAND */
    case 0x03: v = "ENU"; break;   /* LOCALE_SABBREVLANGNAME */
    case 0x07: v = "USA"; break;   /* LOCALE_SABBREVCTRYNAME */
    default: break;
    }
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (!cap) RETURN_STDCALL(n, 4);
    if (n > cap) RETURN_STDCALL(0, 4);
    memcpy(GPTR(dst), v, n);
    RETURN_STDCALL(n, 4);
}

static void IsTextUnicode(void) { RETURN_STDCALL(0, 3); }

static void SetEnvironmentVariableA(void) { RETURN_STDCALL(1, 2); }

/* ---- synchronisation (single-threaded game: these are bookkeeping only) --- */

static void CreateMutexA(void) { RETURN_STDCALL(win32_new_handle(WIN32_H_MUTEX, 0), 3); }
static void OpenMutexA(void) { win32_set_last_error(2); RETURN_STDCALL(0, 3); } /* not already running */
static void ReleaseMutex(void) { RETURN_STDCALL(1, 1); }
static void CreateEventA(void) { RETURN_STDCALL(win32_new_handle(WIN32_H_EVENT, 0), 4); }

static void FormatMessageA(void)
{
    /* Only used to describe errors; give an empty message. */
    uint32_t flags = ARG(0), buf = ARG(4);
    if (flags & 0x100u) { /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        MEM32(buf) = guest_strdup("");
    } else if (ARG(5)) {
        MEM8(buf) = 0;
    }
    RETURN_STDCALL(0, 7);
}

const NativeExport g_kernel32_exports[] = {
    { "GetVersionExA", GetVersionExA },
    { "GetCommandLineA", GetCommandLineA },
    { "GetEnvironmentStrings", GetEnvironmentStrings },
    { "GetEnvironmentStringsW", GetEnvironmentStringsW },
    { "FreeEnvironmentStringsA", FreeEnvironmentStrings },
    { "FreeEnvironmentStringsW", FreeEnvironmentStrings },
    { "GetStartupInfoA", GetStartupInfoA },
    { "GetStdHandle", GetStdHandle },
    { "SetStdHandle", SetStdHandle },
    { "SetHandleCount", SetHandleCount },
    { "GetModuleFileNameA", GetModuleFileNameA },
    { "GetModuleHandleA", GetModuleHandleA },
    { "GetLastError", GetLastError },
    { "GetCurrentProcess", GetCurrentProcess },
    { "GetCurrentProcessId", GetCurrentProcessId },
    { "GetCurrentThreadId", GetCurrentThreadId },
    { "ExitProcess", ExitProcess },
    { "TerminateProcess", TerminateProcess },
    { "SetUnhandledExceptionFilter", SetUnhandledExceptionFilter },
    { "UnhandledExceptionFilter", UnhandledExceptionFilter },
    { "SetConsoleCtrlHandler", SetConsoleCtrlHandler },
    { "RtlUnwind", RtlUnwind },
    { "OutputDebugStringA", OutputDebugStringA },
    { "IsBadReadPtr", IsBadPtr2 },
    { "IsBadWritePtr", IsBadPtr2 },
    { "IsBadCodePtr", IsBadCodePtr },
    { "HeapCreate", HeapCreate },
    { "HeapDestroy", HeapDestroy },
    { "HeapAlloc", HeapAlloc },
    { "HeapFree", HeapFree },
    { "HeapReAlloc", HeapReAlloc },
    { "HeapSize", HeapSize },
    { "VirtualAlloc", VirtualAlloc },
    { "VirtualFree", VirtualFree },
    { "VirtualProtect", VirtualProtect },
    { "VirtualQuery", VirtualQuery },
    { "GetSystemInfo", GetSystemInfo },
    { "LocalFree", LocalFree },
    { "GetTickCount", GetTickCount },
    { "timeGetTime", timeGetTime },
    { "QueryPerformanceCounter", QueryPerformanceCounter },
    { "Sleep", Sleep },
    { "GetSystemTimeAsFileTime", GetSystemTimeAsFileTime },
    { "FileTimeToLocalFileTime", FileTimeToLocalFileTime },
    { "FileTimeToSystemTime", FileTimeToSystemTime },
    { "GetTimeZoneInformation", GetTimeZoneInformation },
    { "GetACP", GetACP },
    { "GetOEMCP", GetOEMCP },
    { "GetCPInfo", GetCPInfo },
    { "lstrlenA", lstrlenA },
    { "MultiByteToWideChar", MultiByteToWideChar },
    { "WideCharToMultiByte", WideCharToMultiByte },
    { "GetStringTypeA", GetStringTypeA },
    { "GetStringTypeW", GetStringTypeW },
    { "LCMapStringA", LCMapStringA },
    { "LCMapStringW", LCMapStringW },
    { "CompareStringA", CompareStringA },
    { "CompareStringW", CompareStringW },
    { "GetLocaleInfoA", GetLocaleInfoA },
    { "IsTextUnicode", IsTextUnicode },
    { "SetEnvironmentVariableA", SetEnvironmentVariableA },
    { "CreateMutexA", CreateMutexA },
    { "OpenMutexA", OpenMutexA },
    { "ReleaseMutex", ReleaseMutex },
    { "CreateEventA", CreateEventA },
    { "FormatMessageA", FormatMessageA },
    { NULL, NULL },
};
