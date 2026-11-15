// DDTestDecoy.dll: looks like the mods freeze tools wrongly blame. It hooks the main loop (so it
// sits below every wait the main thread makes) and keeps a pool of idle worker threads that
// wait in its own code. Dump Diver must never suspect it.

#include <windows.h>

namespace
{
    HANDLE g_work = nullptr;

    __declspec(noinline) DWORD WINAPI IdleWorker(LPVOID)
    {
        for (;;)
            WaitForSingleObject(g_work, INFINITE);
    }
}

extern "C" __declspec(dllexport) void Decoy_Init()
{
    g_work = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (int i = 0; i < 6; ++i)
        CloseHandle(CreateThread(nullptr, 0, &IdleWorker, nullptr, 0, nullptr));
}

volatile unsigned g_frames = 0;

// The write after the call keeps this frame on the stack (no tail jump), like a real hook.
extern "C" __declspec(dllexport) __declspec(noinline) void Decoy_OnFrame(void (*body)())
{
    body();
    g_frames = g_frames + 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
