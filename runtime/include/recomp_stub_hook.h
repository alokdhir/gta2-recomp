/* Force-included into the generated code: any direct-call target the
 * disassembler still missed reports itself when hit (see cpu_state.c). */
#pragma once
#include <stdint.h>
void recomp_unresolved_stub_hit(uint32_t va);
#define RECOMP_UNRESOLVED_STUB_OBSERVE(va) recomp_unresolved_stub_hit(va)
