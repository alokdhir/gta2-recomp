/* Hardware data watchpoint (Apple Silicon debug registers) for render
 * debugging: logs the translated guest function that stores to an 8-byte
 * guest address. One-shot: the trap disarms it so the store completes;
 * watch_rearm() (called once per frame) arms it again. */
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>

#include "debug/watch.h"
#include "recomp_types.h"
#include "win32/imports.h"

static uint32_t g_watch_va;
static int g_hits, g_max_hits = 400, g_armed;

static int set_hw(uint64_t host)
{
    arm_debug_state64_t ds;
    memset(&ds, 0, sizeof ds);
    if (host) {
        ds.__wvr[0] = host & ~7ull;
        /* BAS=0xFF (8 bytes), LSC=2 (store), PAC=2 (EL0), enable */
        ds.__wcr[0] = (0xFFu << 5) | (2u << 3) | (2u << 1) | 1u;
    }
    return thread_set_state(mach_thread_self(), ARM_DEBUG_STATE64, (thread_state_t)&ds,
                            ARM_DEBUG_STATE64_COUNT) == KERN_SUCCESS;
}

static void on_trap(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    ucontext_t *uc = ucv;
    set_hw(0);
    g_armed = 0;
    void *pc = (void *)uc->uc_mcontext->__ss.__pc;
    Dl_info di;
    char where[96] = "?";
    if (dladdr(pc, &di) && di.dli_sname) snprintf(where, sizeof where, "%s", di.dli_sname);
    char chain[512] = "";
    void *bt[32];
    int n = backtrace(bt, 32);
    for (int i = 0, k = 0; i < n && k < 480; i++)
        if (dladdr(bt[i], &di) && di.dli_sname && !strncmp(di.dli_sname, "sub_", 4))
            k += snprintf(chain + k, sizeof chain - (size_t)k, " %s", di.dli_sname);
    uint32_t b = g_watch_va;
    rt_log("WATCH %08X written in %s | before: %08X %08X %08X %08X | chain:%s", g_watch_va, where,
           MEM32(b), MEM32(b + 4), MEM32(b + 8), MEM32(b + 12), chain);
    g_hits++;
}

void watch_arm(uint32_t guest_va)
{
    static int installed;
    if (!installed) {
        struct sigaction sa = { 0 };
        sa.sa_sigaction = on_trap;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGTRAP, &sa, NULL);
        installed = 1;
    }
    g_watch_va = guest_va;
    g_armed = set_hw((uint64_t)XBOX_PTR(guest_va));
    rt_log("WATCH armed on %08X: %s", guest_va, g_armed ? "ok" : "FAILED");
}

void watch_rearm(void)
{
    static int env_done;
    if (!env_done) {
        env_done = 1;
        const char *e = getenv("GTA2_WATCH"); /* hex guest address to watch from startup */
        if (e) watch_arm((uint32_t)strtoul(e, NULL, 16));
    }
    if (g_watch_va && !g_armed && g_hits < g_max_hits) g_armed = set_hw((uint64_t)XBOX_PTR(g_watch_va));
}
