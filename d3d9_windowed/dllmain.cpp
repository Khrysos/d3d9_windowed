#include <windows.h>

void StartBorderlessHooks();
extern void TryChainLoad_Entry();

static DWORD WINAPI InitThread(LPVOID)
{
    // Chain-load optional DLL first (e.g., popww_ap.dll)
    TryChainLoad_Entry();

    StartBorderlessHooks();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
