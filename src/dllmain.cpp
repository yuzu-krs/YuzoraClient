// YuzoraClient.dll entry point.
//
// DllMain stays intentionally minimal so it never runs anything heavy while
// the loader lock is held. All real work is delegated to yuzora::Client
// through the exported lifecycle API below, which the host process calls
// explicitly (LoaderTest now, the game later).

#include <Windows.h>

#include "core/Client.hpp"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // A failure here is not fatal: it only means this DLL keeps receiving
        // DLL_THREAD_ATTACH / DLL_THREAD_DETACH notifications, which it ignores.
        DisableThreadLibraryCalls(module);
    }

    return TRUE;
}

// Exported lifecycle API. extern "C" keeps the exported names stable and
// undecorated ("YuzoraInitialize" / "YuzoraShutdown"), so hosts can resolve
// them with GetProcAddress without depending on a C++ toolchain.
//
// Both functions return whether the client reached the requested state.

extern "C" __declspec(dllexport) bool YuzoraInitialize() {
    return yuzora::Client::instance().initialize();
}

extern "C" __declspec(dllexport) bool YuzoraShutdown() {
    return yuzora::Client::instance().shutdown();
}
