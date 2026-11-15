// DumpDiverTestHost.exe: a stand-in game. It loads Data\SKSE\Plugins\DumpDiver.dll the way SKSE
// does (same exports, same task and messaging interfaces), runs a main loop with a window, and
// freezes in the way the scenario asks for, about two seconds after the first heartbeat.
//
//   DumpDiverTestHost.exe <scenario>
//
// Scenarios: busy-plugin, task-engine-loop, worker-busy, engine-lock, deadlock, dialog, gpu-decoy,
//            loading-stall, loading-normal, engine-loop-record, plugin-loop-engine, lock-left-held

#define DD_GAME_SKYRIM
#include "../test/TestApi.h"
#include "common/Protocol.h"
#include "plugin/XseAbi.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // --- script extender stand-in --------------------------------------------------------

    std::mutex          g_taskLock;
    std::vector<void*>  g_queued;
    std::vector<xse::EventCallback> g_listeners;

    void HostAddTask(void* task)
    {
        std::lock_guard lock(g_taskLock);
        g_queued.push_back(task);
    }

    bool HostRegisterListener(xse::PluginHandle, const char*, xse::EventCallback callback)
    {
        g_listeners.push_back(callback);
        return true;
    }

    TestTaskInterface       g_taskInterface{ 2, &HostAddTask, &HostAddTask };
    xse::MessagingInterface g_messaging{ 2, &HostRegisterListener };

    void* HostQueryInterface(std::uint32_t id)
    {
        if (id == xse::skse::kInterface_Task)
            return &g_taskInterface;
        if (id == xse::skse::kInterface_Messaging)
            return &g_messaging;
        return nullptr;
    }

    xse::PluginHandle HostPluginHandle() { return 1; }

    void Send(std::uint32_t type)
    {
        for (const auto listener : g_listeners) {
            xse::Message message{ "SKSE", type, 0, nullptr };
            listener(&message);
        }
    }

    // Skyrim sends the startup messages from its startup thread, which then exits.
    DWORD WINAPI StartupThread(LPVOID)
    {
        Send(xse::skse::kPostLoad);
        Send(xse::skse::kInputLoaded);
        Send(xse::skse::kDataLoaded);
        return 0;
    }

    void RunTasks()
    {
        std::vector<void*> tasks;
        {
            std::lock_guard lock(g_taskLock);
            tasks.swap(g_queued);
        }
        for (void* p : tasks) {
            auto* task = static_cast<xse::SkseTask*>(p);
            task->Run();
            task->Dispose();
        }
    }

    // --- "engine" code (lives in the host executable, like the game's) ------------------

    EngineLock        g_engineLock{ 0, 0 };
    CRITICAL_SECTION  g_engineCs;
    volatile std::uint64_t g_engineSink = 0;

    __declspec(noinline) void EngineLoopForever()
    {
        std::uint64_t x = g_engineSink;
        for (;;) {
            x = x * 2862933555777941757ull + 3037000493ull;
            g_engineSink = x;
        }
    }

    // Shaped like the game's BSSpinLock::Lock: owner check, then a compare-exchange on the count.
    __declspec(noinline) void EngineLockAcquire(EngineLock* lock)
    {
        const long self = static_cast<long>(GetCurrentThreadId());
        if (lock->owner == self) {
            InterlockedIncrement(&lock->count);
            return;
        }
        while (InterlockedCompareExchange(&lock->count, 1, 0) != 0)
            Sleep(1);
        lock->owner = self;
    }

    void EngineEnterCriticalSection() { EnterCriticalSection(&g_engineCs); }

    // A game helper that does real work per call, like the game's memory manager.
    __declspec(noinline) void* EngineAllocate(std::size_t size)
    {
        std::uint64_t x = g_engineSink;
        for (int i = 0; i < 60000; ++i)
            x = x * 2862933555777941757ull + 3037000493ull;
        g_engineSink = x;
        static std::uint8_t pool[4096];
        return pool + (x % (sizeof(pool) - size));
    }

    // A job thread the game starts, and a wait for its job.
    HANDLE g_jobReady = nullptr;
    HANDLE g_jobDone = nullptr;
    void (*volatile g_job)() = nullptr;

    __declspec(noinline) DWORD WINAPI JobThread(LPVOID)
    {
        for (;;) {
            WaitForSingleObject(g_jobReady, INFINITE);
            if (g_job)
                g_job();
            SetEvent(g_jobDone);
        }
    }

    __declspec(noinline) void WaitForJob(void (*job)())
    {
        g_job = job;
        SetEvent(g_jobReady);
        WaitForSingleObject(g_jobDone, INFINITE);
    }

    // A GPU query poll: ID3D11DeviceContext::GetData is vtable slot 29 (+0xE8), then Sleep(1).
    struct FakeContext
    {
        void** vtable;
    };
    long  FakeGetData(FakeContext*) { return 1; }  // S_FALSE: not ready yet
    void* g_fakeVtable[32];
    FakeContext           g_fakeContext{ g_fakeVtable };
    FakeContext* volatile g_context = &g_fakeContext;

    __declspec(noinline) void FakeGpuPoll()
    {
        for (;;) {
            FakeContext* context = g_context;
            using GetDataFn = long (*)(FakeContext*);
            if (reinterpret_cast<GetDataFn>(context->vtable[29])(context) == 0)
                break;
            Sleep(1);
        }
    }

    volatile std::uint32_t g_advances = 0;
}

// The game-shaped classes live outside the anonymous namespace: their RTTI names must be the
// game's (".?AVLoadingMenu@@", ".?AVTESForm@@"), not ".?AVLoadingMenu@?A0x...@@".

// --- a loading screen, shaped like the game's (IMenu: AdvanceMovie is vtable slot 5) ---

class IMenuLike
{
public:
    virtual ~IMenuLike() = default;                                // 00
    virtual void Accept() {}                                       // 01
    virtual void PostCreate() {}                                   // 02
    virtual void Unk03() {}                                        // 03
    virtual int  ProcessMessage(void*) { return 0; }               // 04
    virtual void AdvanceMovie(float, std::uint32_t) {}             // 05
};

class LoadingMenu final : public IMenuLike
{
public:
    ~LoadingMenu() override { g_advances = 0; }
    void AdvanceMovie(float, std::uint32_t) override { g_advances = g_advances + 1; }
};

// --- game records, laid out like Skyrim's ---------------------------------------------

struct TESFile
{
    std::uint8_t pad[0x58];
    char         fileName[260];
};

struct TESFileArray
{
    TESFile**     data;
    std::uint32_t size;
    std::uint32_t pad;
};

class TESForm
{
public:
    virtual ~TESForm() = default;
    TESFileArray* files = nullptr;   // 08
    std::uint32_t formFlags = 0;     // 10
    std::uint32_t formID = 0;        // 14
    std::uint16_t inGameFlags = 0;   // 18
    std::uint8_t  formType = 0;      // 1A
    std::uint8_t  pad1B = 0;
    std::uint32_t pad1C = 0;
};
static_assert(sizeof(TESForm) == 0x20);

class TESNPC final : public TESForm
{};

class TESObjectCELL final : public TESForm
{};

class TESObjectREFR : public TESForm
{
public:
    std::uint8_t pad20[0x20]{};
    TESForm*     baseObject = nullptr;  // 40
    std::uint8_t pad48[0x18]{};
    TESForm*     parentCell = nullptr;  // 60
};

class Character final : public TESObjectREFR
{};

namespace
{
    IMenuLike* volatile g_menu = nullptr;
    ULONGLONG           g_menuCloseTick = 0;  // 0: the loading screen stays up

    TESFile       g_master{ {}, "DDTestMaster.esm" };
    TESFile       g_records{ {}, "DDTestRecords.esp" };
    TESFile*      g_npcFiles[2] = { &g_master, &g_records };
    TESFile*      g_refFiles[1] = { &g_records };
    TESFile*      g_cellFiles[1] = { &g_master };
    TESFileArray  g_npcArray{ g_npcFiles, 2, 0 };
    TESFileArray  g_refArray{ g_refFiles, 1, 0 };
    TESFileArray  g_cellArray{ g_cellFiles, 1, 0 };

    __declspec(noinline) void EngineProcessReference(TESObjectREFR* reference)
    {
        TESObjectREFR* volatile keep = reference;
        std::uint64_t           x = g_engineSink;
        for (;;) {
            x = x * 2862933555777941757ull + keep->formID;
            g_engineSink = x;
        }
    }

    __declspec(noinline) void StartEngineLoopOnRecord()
    {
        auto* npc = new TESNPC();
        npc->files = &g_npcArray;
        npc->formID = 0x0A0012F3;
        npc->formType = 43;
        auto* cell = new TESObjectCELL();
        cell->files = &g_cellArray;
        cell->formID = 0x0900003C;
        cell->formType = 60;
        auto* reference = new Character();
        reference->files = &g_refArray;
        reference->formID = 0x0A000801;
        reference->formType = 62;
        reference->baseObject = npc;
        reference->parentCell = cell;
        if (reinterpret_cast<std::uint8_t*>(&reference->baseObject) - reinterpret_cast<std::uint8_t*>(reference) != 0x40 ||
            reinterpret_cast<std::uint8_t*>(&reference->parentCell) - reinterpret_cast<std::uint8_t*>(reference) != 0x60)
            ExitProcess(7);  // the stand-in's layout must match the game's
        EngineProcessReference(reference);
    }

    // --- scenario ------------------------------------------------------------------------

    std::wstring g_scenario;
    HMODULE      g_culprit = nullptr;
    HMODULE      g_decoy = nullptr;
    int          g_frame = 0;
    bool         g_heartbeatSeen = false;
    int          g_triggerFrame = -1;
    const dd::Header* g_shared = nullptr;

    template <class T>
    T Export(HMODULE module, const char* name)
    {
        return reinterpret_cast<T>(GetProcAddress(module, name));
    }

    __declspec(noinline) void Trigger()
    {
        if (g_scenario == L"busy-plugin") {
            Export<void (*)()>(g_culprit, "Culprit_SpinForever")();
        } else if (g_scenario == L"task-engine-loop") {
            Export<void (*)()>(g_culprit, "Culprit_QueueLoopingTask")();
        } else if (g_scenario == L"worker-busy") {
            WaitForJob(Export<void (*)()>(g_culprit, "Culprit_JobSpin"));
        } else if (g_scenario == L"engine-lock") {
            g_job = Export<void (*)()>(g_culprit, "Culprit_TakeEngineLockAndHang");
            SetEvent(g_jobReady);
            Sleep(300);
            EngineLockAcquire(&g_engineLock);
        } else if (g_scenario == L"deadlock") {
            EnterCriticalSection(&g_engineCs);
            Export<void (*)()>(g_culprit, "Culprit_Deadlock")();
        } else if (g_scenario == L"dialog") {
            Export<void (*)()>(g_culprit, "Culprit_ShowError")();
        } else if (g_scenario == L"gpu-decoy") {
            FakeGpuPoll();
        } else if (g_scenario == L"loading-stall") {
            g_menu = new LoadingMenu();
            g_job = Export<void (*)()>(g_culprit, "Culprit_LoaderSpin");
            SetEvent(g_jobReady);
        } else if (g_scenario == L"loading-normal") {
            g_menu = new LoadingMenu();
            g_menuCloseTick = GetTickCount64() + 2000;  // two seconds (wall clock: frame times vary), then it closes like a normal load
        } else if (g_scenario == L"plugin-loop-engine") {
            Export<void (*)()>(g_culprit, "Culprit_LoopAroundEngine")();
        } else if (g_scenario == L"engine-loop-record") {
            StartEngineLoopOnRecord();
        } else if (g_scenario == L"lock-left-held") {
            EngineLockAcquire(&g_engineLock);  // left held by the startup thread, which has exited
        }
    }

    void FrameBody()
    {
        ++g_frame;
        if (!g_heartbeatSeen && g_shared && g_shared->heartbeatCount > 0) {
            g_heartbeatSeen = true;
            g_triggerFrame = g_frame + 120;  // about two seconds of healthy frames first
        }
        if (g_frame == g_triggerFrame)
            Trigger();
        if (g_menu) {
            g_menu->AdvanceMovie(0.016f, static_cast<std::uint32_t>(g_frame));
            if (g_menuCloseTick && GetTickCount64() >= g_menuCloseTick) {
                g_menuCloseTick = 0;
                IMenuLike* menu = g_menu;
                g_menu = nullptr;
                delete menu;
            }
        }
        Sleep(16);
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

extern "C" __declspec(dllexport) int DumpDiverTestHostMarker = 1;

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 2;
    g_scenario = argv[1];

    for (auto& slot : g_fakeVtable)
        slot = reinterpret_cast<void*>(&FakeGetData);
    InitializeCriticalSection(&g_engineCs);
    g_jobReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_jobDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, &JobThread, nullptr, 0, nullptr));

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = &WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"DumpDiverTestHost";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowW(L"DumpDiverTestHost", L"Dump Diver test host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 640,
                                360, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!window)
        return 3;

    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path plugins = fs::path(self).parent_path() / L"Data" / L"SKSE" / L"Plugins";

    // Load the plugin like SKSE.
    HMODULE plugin = LoadLibraryW((plugins / L"DumpDiver.dll").c_str());
    if (!plugin)
        return 4;
    xse::Interface skse{ 0x02020060, dd::PackVersion(1, 6, 1170), 0, 0, &HostQueryInterface, &HostPluginHandle };
    auto load = Export<bool (*)(const xse::Interface*)>(plugin, "SKSEPlugin_Load");
    if (!load || !load(&skse))
        return 5;

    const std::wstring mappingName = std::wstring(dd::kMappingPrefix) + std::to_wstring(GetCurrentProcessId());
    if (HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName.c_str()))
        g_shared = static_cast<const dd::Header*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(dd::Header)));

    // Other plugins.
    g_culprit = LoadLibraryW((plugins / L"DDTestCulprit.dll").c_str());
    g_decoy = LoadLibraryW((plugins / L"DDTestDecoy.dll").c_str());
    if (!g_culprit || !g_decoy)
        return 6;
    static HostApi api{ &g_taskInterface, &EngineLoopForever, &EngineLockAcquire, &g_engineLock, &EngineEnterCriticalSection,
                        &EngineAllocate, reinterpret_cast<TestMessagingInterface*>(&g_messaging) };
    Export<void (*)(const HostApi*)>(g_culprit, "Culprit_Init")(&api);
    Export<void (*)()>(g_decoy, "Decoy_Init")();
    auto decoyOnFrame = Export<void (*)(void (*)())>(g_decoy, "Decoy_OnFrame");

    if (g_scenario == L"lock-left-held") {
        Export<void (*)()>(g_culprit, "Culprit_RegisterLeakyListener")();
        HANDLE startup = CreateThread(nullptr, 0, &StartupThread, nullptr, 0, nullptr);
        WaitForSingleObject(startup, INFINITE);
        CloseHandle(startup);
    } else {
        StartupThread(nullptr);
    }

    for (;;) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                return 0;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        RunTasks();
        decoyOnFrame(&FrameBody);  // the decoy hooks the main loop
    }
}
