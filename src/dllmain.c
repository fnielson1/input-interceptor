/*
 * Entry point for the standalone MSVC build (build-msvc.cmd), which links with
 * /NODEFAULTLIB so the DLL depends on kernel32 alone.
 *
 * interceptor.c uses no CRT function, so nothing here needs to initialise one;
 * this stub exists only to give the linker an entry point. The WDK build driven
 * by buildit.cmd does not compile this file -- `sources` does not list it, and
 * DLLENTRY=_DllMainCRTStartup supplies its own default.
 */

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    /* No per-thread state, so thread attach/detach callbacks are pure overhead
       on a DLL that sits in the input path of every process that loads it. */
    if(reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);

    return TRUE;
}
