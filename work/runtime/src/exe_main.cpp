// Thin launcher: links the recompiled game code and hands it to the host DLL (itc_host.dll),
// which contains all hand-written code (loader, kernel, GPU...). Rebuilding the host DLL is fast.
#include "ppc_config.h"
#include "ppc_context.h"

const char* g_traceRing[256];   // written by the generated functions (see gcc_compat.h)
unsigned g_tracePos = 0;

extern "C" __declspec(dllimport) int HostRun(int argc, char** argv, PPCFuncMapping* mappings, const char** traceRing, unsigned* tracePos);

int main(int argc, char** argv)
{
    return HostRun(argc, argv, PPCFuncMappings, g_traceRing, &g_tracePos);
}
