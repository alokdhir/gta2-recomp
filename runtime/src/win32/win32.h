/* Shared Win32 emulation state: handles, last-error, paths. */
#pragma once
#include <stdint.h>

typedef enum {
    WIN32_H_FREE = 0,
    WIN32_H_FILE,
    WIN32_H_FIND,
    WIN32_H_MUTEX,
    WIN32_H_EVENT,
    WIN32_H_STD,
    WIN32_H_REGKEY,
    WIN32_H_MODULE,
} Win32HandleKind;

typedef struct {
    Win32HandleKind kind;
    intptr_t data;   /* fd, DIR*, registry key index, ... */
    char *path;      /* host path for files / find pattern dir */
    char *pattern;   /* FindFirstFile wildcard */
} Win32Handle;

uint32_t win32_new_handle(Win32HandleKind kind, intptr_t data);
Win32Handle *win32_get_handle(uint32_t h, Win32HandleKind kind);
void win32_close_handle(uint32_t h);
uint32_t win32_std_handle(int which); /* 0 in, 1 out, 2 err */

void win32_set_last_error(uint32_t e);

/* Map a guest Windows path to a host path under the game dir. Returns a
 * malloc'd string; resolves each component case-insensitively. */
char *win32_host_path(const char *winpath);

/* Registry access for native code; key is relative to Software\DMA Design Ltd\GTA2. */
uint32_t reg_get_dword(const char *key, const char *name, uint32_t def);
void reg_set_dword(const char *key, const char *name, uint32_t value);

/* Restart the app when the game exits (after the settings dialog). */
void app_request_relaunch(void);
void app_exit_hook(void);
