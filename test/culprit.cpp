// DDTestCulprit.dll: causes each kind of freeze the scenarios check. Dump Diver must name it.

#include "TestApi.h"

#include <windows.h>

#include <cstdint>

namespace
{
    const HostApi*    g_api = nullptr;
    CRITICAL_SECTION  g_lockA;
    volatile std::uint64_t g_sink = 0;

    __declspec(noinline) void BusyWork()
    {
        std::uint64_t x = g_sink;
        for (int i = 0; i < 100000; ++i)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        g_sink = x;
    }

    class LoopingTask final : public TestTask
    {
    public:
        void Run() override { g_api->EngineLoopForever(); }
        void Dispose() override { delete this; }
    };

    DWORD WINAPI DeadlockWorker(LPVOID param)
    {
        EnterCriticalSection(&g_lockA);
        SetEvent(static_cast<HANDLE>(param));
        g_api->EngineEnterCriticalSection();  // the main thread holds it
        LeaveCriticalSection(&g_lockA);
        return 0;
    }
}

extern "C" __declspec(dllexport) void Culprit_Init(const HostApi* api)
{
    g_api = api;
    InitializeCriticalSection(&g_lockA);
}

extern "C" __declspec(dllexport) __declspec(noinline) void Culprit_SpinForever()
{
    for (;;)
        BusyWork();
}

extern "C" __declspec(dllexport) void Culprit_QueueLoopingTask()
{
    // Through the interface table at call time, like every plugin.
    g_api->tasks->AddTask(new LoopingTask());
}

// Runs on the game's loading thread and never finishes.
extern "C" __declspec(dllexport) __declspec(noinline) void Culprit_LoaderSpin()
{
    for (;;)
        BusyWork();
}

// SkyPatcher 7.1.2's shape: an endless loop in this DLL that calls a game function on every pass, so
// most samples land in the game's code.
__forceinline void* AllocateNode() { return g_api->EngineAllocate(32); }
__forceinline void* NewNode() { return AllocateNode(); }
__forceinline std::uint64_t PushFront() { return reinterpret_cast<std::uint64_t>(NewNode()); }

extern "C" __declspec(dllexport) __declspec(noinline) void Culprit_LoopAroundEngine()
{
    for (;;) {
        std::uint64_t x = PushFront();
        for (int i = 0; i < 3000; ++i)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        g_sink = g_sink + x;
    }
}

extern "C" __declspec(dllexport) __declspec(noinline) void Culprit_JobSpin()
{
    for (;;)
        BusyWork();
}

// The calls below are followed by a write so the compiler cannot turn them into tail jumps,
// which would drop this DLL's frame from the stack.
extern "C" __declspec(dllexport) __declspec(noinline) void Culprit_TakeEngineLockAndHang()
{
    g_api->EngineLockAcquire(g_api->engineLock);
    for (;;) {
        Sleep(INFINITE);
        g_sink = g_sink + 1;
    }
}

// Its data-loaded handler takes the engine lock and returns without releasing it (Dynamic Gamma 1.0.9
// released a game lock in the wrong order on the startup thread).
__declspec(noinline) void LeakyListener(void* message)
{
    struct Message
    {
        const char*   sender;
        std::uint32_t type;
    };
    if (static_cast<Message*>(message)->type == 8)  // kDataLoaded
        g_api->EngineLockAcquire(g_api->engineLock);
}

extern "C" __declspec(dllexport) void Culprit_RegisterLeakyListener()
{
    g_api->messaging->RegisterListener(2, "SKSE", &LeakyListener);
}

// Called by the main thread while it holds the engine's critical section.
extern "C" __declspec(dllexport) __declspec(noinline) void Culprit_Deadlock()
{
    HANDLE held = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, &DeadlockWorker, held, 0, nullptr));
    WaitForSingleObject(held, INFINITE);
    Sleep(200);  // let the worker block on the engine lock
    EnterCriticalSection(&g_lockA);
    LeaveCriticalSection(&g_lockA);
}

extern "C" __declspec(dllexport) __declspec(noinline) void Culprit_ShowError()
{
    MessageBoxW(nullptr, L"DDTestCulprit requires version 2 of something.", L"DDTestCulprit", MB_OK | MB_ICONERROR);
    g_sink = g_sink + 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
