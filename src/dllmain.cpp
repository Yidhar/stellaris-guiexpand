// DllMain does nothing but start the plugin's thread: it runs under the loader lock (and on the launcher's remote thread), where waiting or loading
// libraries can dead-lock the game.
#include "internal.h"

namespace {
DWORD WINAPI Worker(LPVOID) {
    guidll::Start();
    return 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        guidll::g_module = module;
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
