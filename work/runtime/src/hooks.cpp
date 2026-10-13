// Host replacements for game functions, wired in through [[midasm_hook]] entries in itc.toml.
// The generated function calls the hook at its entry and, with return = true, returns right after.
#include "guest.h"

// Function skipped entirely; reports 0 ("nothing / failure / not ready").
void HookReturn0(PPCRegister& r3) { r3.u64 = 0; }
