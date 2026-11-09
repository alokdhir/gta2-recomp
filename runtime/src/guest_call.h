/* Call a guest (translated) function from native code. */
#pragma once
#include <stdint.h>
#include "recomp_types.h"

recomp_func_t recomp_lookup(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va);
recomp_func_t recomp_lookup_kernel(uint32_t va);

#define GUEST_CALL_RETURN 0xFFFFFFF4u /* fake return address pushed for native->guest calls */

/* Push args (args[0] is the first C argument), call the function at va and
 * return eax. esp is restored afterwards, so this works for both stdcall and
 * cdecl callees. Callee-saved registers are preserved by the callee. */
static inline uint32_t guest_call(uint32_t va, unsigned nargs, const uint32_t *args)
{
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (!fn) {
        recomp_icall_fail_log(va);
        return 0;
    }
    uint32_t saved_esp = g_esp;
    uint32_t saved_ecx = g_ecx, saved_edx = g_edx;
    for (unsigned i = nargs; i-- > 0;) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, GUEST_CALL_RETURN);
    fn();
    g_esp = saved_esp;
    g_ecx = saved_ecx;
    g_edx = saved_edx;
    return g_eax;
}
