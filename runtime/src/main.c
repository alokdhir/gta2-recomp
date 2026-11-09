/* Boot: map the guest, load gta2.exe, bind imports, run the CRT entry point. */
#include <limits.h>
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
