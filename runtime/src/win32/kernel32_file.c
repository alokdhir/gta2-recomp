/* KERNEL32 handles and file system: CreateFile/ReadFile/FindFirstFile/... */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "guest_memory.h"
#include "win32/imports.h"
#include "win32/win32.h"
#include "render3d/world_data.h"

/* ---- handle table --------------------------------------------------------- */

#define MAX_HANDLES 512u
#define HANDLE_BASE 0x100u
static Win32Handle g_handles[MAX_HANDLES];

uint32_t win32_new_handle(Win32HandleKind kind, intptr_t data)
{
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (g_handles[i].kind == WIN32_H_FREE) {
            g_handles[i] = (Win32Handle){ kind, data, NULL, NULL };
            return HANDLE_BASE + 4u * i;
        }
    }
    rt_fatal("out of Win32 handles");
}

Win32Handle *win32_get_handle(uint32_t h, Win32HandleKind kind)
{
    if (h < HANDLE_BASE || (h - HANDLE_BASE) % 4u) return NULL;
    uint32_t i = (h - HANDLE_BASE) / 4u;
    if (i >= MAX_HANDLES || g_handles[i].kind == WIN32_H_FREE) return NULL;
    if (kind != WIN32_H_FREE && g_handles[i].kind != kind) return NULL;
    return &g_handles[i];
}

void win32_close_handle(uint32_t h)
{
    Win32Handle *e = win32_get_handle(h, WIN32_H_FREE);
    if (!e) return;
    if (e->kind == WIN32_H_FILE) close((int)e->data);
    if (e->kind == WIN32_H_FIND && e->data) closedir((DIR *)e->data);
    free(e->path);
    free(e->pattern);
    memset(e, 0, sizeof *e);
}

uint32_t win32_std_handle(int which)
{
    static uint32_t std[3];
    if (!std[which]) std[which] = win32_new_handle(WIN32_H_STD, which);
    return std[which];
}

/* ---- paths ---------------------------------------------------------------- */

/* Find an entry of dir matching name case-insensitively; returns 1 and fills out. */
static int ci_lookup(const char *dir, const char *name, char *out, size_t cap)
{
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, cap, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

char *win32_host_path(const char *w)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", w);
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    char *s = tmp;
    /* Drive-absolute paths: the game only knows its own install dir */
    if (s[0] && s[1] == ':') {
        s += 2;
        while (*s == '/') s++;
        if (!strncasecmp(s, "gta2/", 5)) s += 5;
        else if (!strcasecmp(s, "gta2")) s += 4;
    }
    while (s[0] == '.' && s[1] == '/') s += 2;

    char out[PATH_MAX] = "";
    char *save = NULL;
    for (char *comp = strtok_r(s, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        if (!strcmp(comp, ".")) continue;
        char real[NAME_MAX + 1];
        char probe[PATH_MAX];
        snprintf(probe, sizeof probe, "%s%s%s", out, *out ? "/" : "", comp);
        struct stat st;
        if (stat(probe, &st) != 0 && ci_lookup(out, comp, real, sizeof real))
            comp = real;
        size_t n = strlen(out);
        snprintf(out + n, sizeof out - n, "%s%s", n ? "/" : "", comp);
    }
    if (!*out) strcpy(out, ".");
    return strdup(out);
}

static uint32_t errno_to_win32(int e)
{
    switch (e) {
    case ENOENT: return 2;   /* ERROR_FILE_NOT_FOUND */
    case ENOTDIR: return 3;  /* ERROR_PATH_NOT_FOUND */
    case EACCES: return 5;   /* ERROR_ACCESS_DENIED */
    case EEXIST: return 80;  /* ERROR_FILE_EXISTS */
    default: return 31;      /* ERROR_GEN_FAILURE */
    }
}

/* ---- files ---------------------------------------------------------------- */

#define INVALID_HANDLE 0xFFFFFFFFu

static void CreateFileA(void)
{
    const char *name = GSTR(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char *path = win32_host_path(name);
    int flags = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: flags |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 3: break;                              /* OPEN_EXISTING */
    case 4: flags |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: flags |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        rt_log("CreateFileA(%s) -> %s: %s", name, path, strerror(errno));
        win32_set_last_error(errno_to_win32(errno));
        free(path);
        RETURN_STDCALL(INVALID_HANDLE, 7);
    }
    size_t pl = strlen(path);
    if (pl > 4 && !strcasecmp(path + pl - 4, ".sty")) world_note_style_path(path);
    if (pl > 4 && !strcasecmp(path + pl - 4, ".gmp")) world_note_map_path(path);
    uint32_t h = win32_new_handle(WIN32_H_FILE, fd);
    win32_get_handle(h, WIN32_H_FILE)->path = path;
    win32_set_last_error(0);
    RETURN_STDCALL(h, 7);
}

static void ReadFile(void)
{
    Win32Handle *e = win32_get_handle(ARG(0), WIN32_H_FILE);
    uint32_t buf = ARG(1), n = ARG(2), outn = ARG(3);
    if (!e) { if (outn) MEM32(outn) = 0; RETURN_STDCALL(0, 5); }
    ssize_t r = read((int)e->data, GPTR(buf), n);
    if (outn) MEM32(outn) = r > 0 ? (uint32_t)r : 0;
    RETURN_STDCALL(r >= 0, 5);
}

static void WriteFile(void)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), outn = ARG(3);
    Win32Handle *e = win32_get_handle(h, WIN32_H_FREE);
    ssize_t r = -1;
    if (e && e->kind == WIN32_H_FILE) r = write((int)e->data, GPTR(buf), n);
    else if (e && e->kind == WIN32_H_STD) r = write(e->data == 2 ? 2 : 1, GPTR(buf), n);
    if (outn) MEM32(outn) = r > 0 ? (uint32_t)r : 0;
    RETURN_STDCALL(r >= 0, 5);
}

static void SetFilePointer(void)
{
    Win32Handle *e = win32_get_handle(ARG(0), WIN32_H_FILE);
    int32_t lo = (int32_t)ARG(1);
    uint32_t hiptr = ARG(2), method = ARG(3);
    if (!e) RETURN_STDCALL(0xFFFFFFFFu, 4);
    off_t off = hiptr ? (off_t)(((int64_t)(int32_t)MEM32(hiptr) << 32) | (uint32_t)lo) : lo;
    off_t r = lseek((int)e->data, off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) RETURN_STDCALL(0xFFFFFFFFu, 4);
    if (hiptr) MEM32(hiptr) = (uint32_t)((uint64_t)r >> 32);
    RETURN_STDCALL((uint32_t)r, 4);
}

static void SetEndOfFile(void)
{
    Win32Handle *e = win32_get_handle(ARG(0), WIN32_H_FILE);
    int ok = e && ftruncate((int)e->data, lseek((int)e->data, 0, SEEK_CUR)) == 0;
    RETURN_STDCALL(ok, 1);
}

static void FlushFileBuffers(void) { RETURN_STDCALL(1, 1); }

static void GetFileType(void)
{
    Win32Handle *e = win32_get_handle(ARG(0), WIN32_H_FREE);
    uint32_t t = !e ? 0 : e->kind == WIN32_H_STD ? 2 /* FILE_TYPE_CHAR */ : 1 /* DISK */;
    RETURN_STDCALL(t, 1);
}

static void CloseHandle(void)
{
    win32_close_handle(ARG(0));
    RETURN_STDCALL(1, 1);
}

static void GetFileAttributesA(void)
{
    char *p = win32_host_path(GSTR(ARG(0)));
    struct stat st;
    uint32_t a = 0xFFFFFFFFu;
    if (stat(p, &st) == 0) a = S_ISDIR(st.st_mode) ? 0x10u : 0x80u; /* DIRECTORY : NORMAL */
    else win32_set_last_error(2);
    free(p);
    RETURN_STDCALL(a, 1);
}

static void GetDriveTypeA(void) { RETURN_STDCALL(3 /* DRIVE_FIXED */, 1); }

static char g_cwd[PATH_MAX] = "C:\\GTA2";

static void GetCurrentDirectoryA(void)
{
    uint32_t cap = ARG(0), buf = ARG(1);
    uint32_t n = (uint32_t)strlen(g_cwd);
    if (cap <= n) RETURN_STDCALL(n + 1, 2);
    memcpy(GPTR(buf), g_cwd, n + 1);
    RETURN_STDCALL(n, 2);
}

static void SetCurrentDirectoryA(void)
{
    rt_log("SetCurrentDirectoryA(%s)", GSTR(ARG(0)));
    snprintf(g_cwd, sizeof g_cwd, "%s", GSTR(ARG(0)));
    RETURN_STDCALL(1, 1);
}

static void GetFullPathNameA(void)
{
    const char *in = GSTR(ARG(0));
    uint32_t cap = ARG(1), buf = ARG(2), filepart = ARG(3);
    char full[PATH_MAX];
    if (in[0] && in[1] == ':') snprintf(full, sizeof full, "%s", in);
    else snprintf(full, sizeof full, "%s\\%s", g_cwd, in);
    uint32_t n = (uint32_t)strlen(full);
    if (cap <= n) RETURN_STDCALL(n + 1, 4);
    memcpy(GPTR(buf), full, n + 1);
    if (filepart) {
        char *slash = strrchr(full, '\\');
        MEM32(filepart) = buf + (uint32_t)(slash ? slash - full + 1 : 0);
    }
    RETURN_STDCALL(n, 4);
}

/* WIN32_FIND_DATAA: attrs @0, sizes @28/@32, cFileName @44 (260), alt @304 */
static int find_fill(Win32Handle *e, uint32_t out)
{
    struct dirent *d;
    while ((d = readdir((DIR *)e->data))) {
        if (fnmatch(e->pattern, d->d_name, FNM_CASEFOLD) != 0) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", e->path, d->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        memset(GPTR(out), 0, 320);
        MEM32(out) = S_ISDIR(st.st_mode) ? 0x10u : 0x80u;
        MEM32(out + 28) = (uint32_t)((uint64_t)st.st_size >> 32);
        MEM32(out + 32) = (uint32_t)st.st_size;
        snprintf((char *)GPTR(out + 44), 260, "%s", d->d_name);
        return 1;
    }
    return 0;
}

static void FindFirstFileA(void)
{
    char *p = win32_host_path(GSTR(ARG(0)));
    char *slash = strrchr(p, '/');
    char *dir = slash ? strndup(p, (size_t)(slash - p)) : strdup(".");
    char *pat = strdup(slash ? slash + 1 : p);
    /* Normalise "*.*" to "*" (Windows matches names without dots too) */
    if (!strcmp(pat, "*.*")) pat[1] = 0;
    free(p);
    DIR *d = opendir(dir);
    if (!d) {
        free(dir); free(pat);
        win32_set_last_error(3);
        RETURN_STDCALL(INVALID_HANDLE, 2);
    }
    uint32_t h = win32_new_handle(WIN32_H_FIND, (intptr_t)d);
    Win32Handle *e = win32_get_handle(h, WIN32_H_FIND);
    e->path = dir;
    e->pattern = pat;
    if (!find_fill(e, ARG(1))) {
        win32_close_handle(h);
        win32_set_last_error(2);
        RETURN_STDCALL(INVALID_HANDLE, 2);
    }
    RETURN_STDCALL(h, 2);
}

static void FindNextFileA(void)
{
    Win32Handle *e = win32_get_handle(ARG(0), WIN32_H_FIND);
    if (!e || !find_fill(e, ARG(1))) {
        win32_set_last_error(18); /* ERROR_NO_MORE_FILES */
        RETURN_STDCALL(0, 2);
    }
    RETURN_STDCALL(1, 2);
}

static void FindClose(void)
{
    win32_close_handle(ARG(0));
    RETURN_STDCALL(1, 1);
}

/* GetPrivateProfileStringA(section, key, default, buf, size, file) */
static void GetPrivateProfileStringA(void)
{
    const char *sect = ARG(0) ? GSTR(ARG(0)) : NULL, *key = ARG(1) ? GSTR(ARG(1)) : NULL;
    const char *def = ARG(2) ? GSTR(ARG(2)) : "";
    uint32_t buf = ARG(3), cap = ARG(4);
    char *path = win32_host_path(GSTR(ARG(5)));
    FILE *f = fopen(path, "r");
    free(path);
    char value[1024];
    snprintf(value, sizeof value, "%s", def);
    if (f && sect && key) {
        char line[1024];
        int in = 0;
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            s[strcspn(s, "\r\n")] = 0;
            if (*s == '[') {
                char *end = strchr(s, ']');
                if (end) *end = 0;
                in = !strcasecmp(s + 1, sect);
            } else if (in) {
                char *eq = strchr(s, '=');
                if (!eq) continue;
                *eq = 0;
                char *k = s, *ke = eq;
                while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
                if (!strcasecmp(k, key)) {
                    char *v = eq + 1;
                    while (*v == ' ' || *v == '\t') v++;
                    snprintf(value, sizeof value, "%s", v);
                    break;
                }
            }
        }
    }
    if (f) fclose(f);
    uint32_t n = (uint32_t)strlen(value);
    if (cap && n >= cap) n = cap - 1;
    if (cap) {
        memcpy(GPTR(buf), value, n);
        MEM8(buf + n) = 0;
    }
    RETURN_STDCALL(n, 6);
}

const NativeExport g_kernel32_file_exports[] = {
    { "CreateFileA", CreateFileA },
    { "ReadFile", ReadFile },
    { "WriteFile", WriteFile },
    { "SetFilePointer", SetFilePointer },
    { "SetEndOfFile", SetEndOfFile },
    { "FlushFileBuffers", FlushFileBuffers },
    { "GetFileType", GetFileType },
    { "CloseHandle", CloseHandle },
    { "GetFileAttributesA", GetFileAttributesA },
    { "GetDriveTypeA", GetDriveTypeA },
    { "GetCurrentDirectoryA", GetCurrentDirectoryA },
    { "SetCurrentDirectoryA", SetCurrentDirectoryA },
    { "GetFullPathNameA", GetFullPathNameA },
    { "FindFirstFileA", FindFirstFileA },
    { "FindNextFileA", FindNextFileA },
    { "FindClose", FindClose },
    { "GetPrivateProfileStringA", GetPrivateProfileStringA },
    { NULL, NULL },
};
