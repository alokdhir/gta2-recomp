/* Boot: map the guest, load gta2.exe, bind imports, run the CRT entry point. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest_memory.h"
#include "recomp_types.h"
#include "win32/imports.h"

recomp_func_t recomp_lookup(uint32_t va);
int recomp_dispatch_init(void);

#define RETURN_SENTINEL 0xFFFFFFF0u

static const char *find_game_dir(const char *argv0)
{
    const char *env = getenv("GTA2_GAME_DIR");
    if (env) return env;
    static char buf[PATH_MAX];
    /* Default: game_files/ beside the repository's build dir, or the cwd. */
    const char *candidates[] = { "game_files", "../game_files", "../../game_files" };
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        snprintf(buf, sizeof buf, "%s/gta2.exe", candidates[i]);
        if (access(buf, R_OK) == 0) {
            realpath(candidates[i], buf);
            return buf;
        }
    }
    (void)argv0;
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    const char *dir = find_game_dir(argv[0]);
    if (!dir) {
        fprintf(stderr, "gta2: game files not found; set GTA2_GAME_DIR to a GTA2 v9.6 install\n");
        return 1;
    }
    rt_log("game dir: %s", dir); /* opens GTA2_LOG relative to the launch dir */
    if (chdir(dir) != 0) rt_fatal("cannot enter %s", dir);

    guest_memory_init();
    GuestImage img;
    guest_load_pe("gta2.exe", &img);
    rt_log("image: entry %08X code %08X-%08X", img.entry, img.code_lo, img.code_hi);
    imports_bind();
    recomp_dispatch_init();
    guest_tib_init();

    recomp_func_t entry = recomp_lookup(img.entry);
    if (!entry) rt_fatal("no translated function at entry point %08X", img.entry);
    PUSH32(g_esp, RETURN_SENTINEL);
    entry();
    rt_log("entry point returned (eax=%08X)", g_eax);
    return (int)g_eax;
}
