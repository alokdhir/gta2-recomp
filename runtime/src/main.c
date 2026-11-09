/* Boot: map the guest, load gta2.exe, bind imports, run the CRT entry point. */
#include <limits.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "guest_memory.h"
#include "recomp_types.h"
#include "video/video.h"
void d3ddll_register(void);
#include "win32/imports.h"

recomp_func_t recomp_lookup(uint32_t va);
int recomp_dispatch_init(void);

#define RETURN_SENTINEL 0xFFFFFFF0u

/* Report faults in guest terms: which guest address, which registers. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    /* The host PC names the translated guest function (sub_XXXXXXXX). */
    void *pc = (void *)((ucontext_t *)uc)->uc_mcontext->__ss.__pc;
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_sname)
        fprintf(stderr, "gta2: faulting in %s+0x%lx\n", di.dli_sname, (unsigned long)((char *)pc - (char *)di.dli_saddr));
    void *bt[24];
    int n = backtrace(bt, 24);
    fputs("gta2: call chain:", stderr);
    for (int i = 0; i < n; i++)
        if (dladdr(bt[i], &di) && di.dli_sname && !strncmp(di.dli_sname, "sub_", 4))
            fprintf(stderr, " %s", di.dli_sname);
    fputc('\n', stderr);
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    if (a >= base && a < base + (1ull << 32))
        fprintf(stderr, "gta2: %s at guest address %08lX\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV",
                (unsigned long)(a - base));
    else
        fprintf(stderr, "gta2: %s at host address %p\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV", si->si_addr);
    rt_fatal("crashed");
}

/* Where the game files are, in order: $GTA2_GAME_DIR; inside the app bundle
 * (Contents/Resources/game_files); beside the .app or the binary (also one
 * and two levels up, so build/gta2 and build/GTA2.app find the repository's
 * game_files/); the current directory. */
static const char *find_game_dir(const char *argv0)
{
    const char *env = getenv("GTA2_GAME_DIR");
    if (env) return env;
    static char buf[PATH_MAX];
    char exe[PATH_MAX], dir[PATH_MAX];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) != 0 || !realpath(exe, dir)) snprintf(dir, sizeof dir, "%s", argv0);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    const char *rel[] = { "../Resources/game_files", "game_files", "../game_files", "../../game_files",
                          "../../../game_files", "../../../../game_files" };
    for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
        char probe[PATH_MAX];
        snprintf(probe, sizeof probe, "%s/%s/gta2.exe", dir, rel[i]);
        if (access(probe, R_OK) == 0) {
            snprintf(probe, sizeof probe, "%s/%s", dir, rel[i]);
            if (realpath(probe, buf)) return buf;
        }
    }
    if (access("game_files/gta2.exe", R_OK) == 0 && realpath("game_files", buf)) return buf;
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    const char *dir = find_game_dir(argv[0]);
    if (!dir) {
        fprintf(stderr, "gta2: game files not found; run scripts/setup.sh or set GTA2_GAME_DIR\n");
        return 1;
    }
    rt_log("game dir: %s", dir); /* opens GTA2_LOG relative to the launch dir */
    if (chdir(dir) != 0) rt_fatal("cannot enter %s", dir);

    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    guest_memory_init();
    GuestImage img;
    guest_load_pe("gta2.exe", &img);
    rt_log("image: entry %08X code %08X-%08X", img.entry, img.code_lo, img.code_hi);
    dmavideo_register();
    d3ddll_register();
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
