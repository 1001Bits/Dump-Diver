// DumpDiverFreezeTest.dll: freezes the real game on request, for the in-game lab tests.
// Never shipped. It waits for a trigger file in %MODTESTLAB_DIR% (the lab run's folder):
//   ddfreeze.task-spin  queue a task through the script extender that never returns
//   ddfreeze.dialog     queue a task that shows a message box on the game's task thread

#include "plugin/XseAbi.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{
    HMODULE             g_module = nullptr;
    xse::TaskInterface* g_tasks = nullptr;
    FILE*               g_log = nullptr;
    volatile std::uint64_t g_sink = 0;

    void Log(const char* text)
    {
        if (!g_log)
            return;
        SYSTEMTIME time{};
        GetLocalTime(&time);
        fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, text);
        fflush(g_log);
    }

    __declspec(noinline) void SpinForever()
    {
        std::uint64_t x = g_sink;
        for (;;) {
            for (int i = 0; i < 100000; ++i)
                x = x * 6364136223846793005ull + 1442695040888963407ull;
            g_sink = x;
        }
    }

    __declspec(noinline) void ShowDialog()
    {
        MessageBoxW(nullptr, L"DumpDiverFreezeTest: this message box blocks the game on purpose.", L"DumpDiverFreezeTest",
                    MB_OK | MB_ICONWARNING);
        g_sink = g_sink + 1;
    }

    // A stall that ends by itself: the main thread sleeps (or burns a core) for a while, then the game goes on.
    __declspec(noinline) void Stall(DWORD milliseconds, bool spin)
    {
        const ULONGLONG end = GetTickCount64() + milliseconds;
        std::uint64_t   x = g_sink;
        while (GetTickCount64() < end) {
            if (spin) {
                for (int i = 0; i < 100000; ++i)
                    x = x * 6364136223846793005ull + 1442695040888963407ull;
            } else {
                Sleep(50);
            }
        }
        g_sink = x;
    }

    class TestTask final : public xse::Task
    {
    public:
        explicit TestTask(bool dialog, DWORD stallMs = 0, bool spin = false) :
            dialog_(dialog), stallMs_(stallMs), spin_(spin)
        {}
        void Run() override
        {
            if (stallMs_) {
                Log(spin_ ? "task running: busy stall" : "task running: sleeping stall");
                Stall(stallMs_, spin_);
                Log("stall over");
                return;
            }
            Log(dialog_ ? "task running: showing the message box" : "task running: spinning forever");
            if (dialog_)
                ShowDialog();
            else
                SpinForever();
        }
#if defined(DD_GAME_SKYRIM)
        void Dispose() override { delete this; }
#endif

    private:
        bool  dialog_;
        DWORD stallMs_;
        bool  spin_;
    };

    void Watch(fs::path folder)
    {
        Log(("waiting for a trigger file in " + folder.string()).c_str());
        for (;;) {
            Sleep(250);
            // ddfreeze.stall / ddfreeze.stall-spin hold "<seconds>": a stall that ends; keep watching afterwards
            for (const char* mode : { "stall", "stall-spin" }) {
                const fs::path trigger = folder / (std::string("ddfreeze.") + mode);
                std::error_code ec;
                if (!fs::exists(trigger, ec))
                    continue;
                double seconds = 8;
                FILE*  f = nullptr;
                if (_wfopen_s(&f, trigger.c_str(), L"r") == 0 && f) {
                    if (fscanf_s(f, "%lf", &seconds) != 1)
                        seconds = 8;
                    fclose(f);
                }
                fs::remove(trigger, ec);
                Log((std::string("trigger: ") + mode).c_str());
                g_tasks->AddTask(new TestTask(false, static_cast<DWORD>(seconds * 1000), std::string(mode) == "stall-spin"));
            }
            for (const char* mode : { "task-spin", "dialog" }) {
                const fs::path trigger = folder / (std::string("ddfreeze.") + mode);
                std::error_code ec;
                if (!fs::exists(trigger, ec))
                    continue;
                fs::remove(trigger, ec);
                Log((std::string("trigger: ") + mode).c_str());
                // Through the interface table, like any plugin (so Dump Diver's task tracking sees it).
                g_tasks->AddTask(new TestTask(std::string(mode) == "dialog"));
                Log("task queued");
                return;
            }
        }
    }

    bool Load(const xse::Interface* loader)
    {
        PWSTR documents = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
#if defined(DD_GAME_SKYRIM)
            const fs::path dir = fs::path(documents) / L"My Games" / L"Skyrim Special Edition" / L"SKSE";
#else
            const fs::path dir = fs::path(documents) / L"My Games" / L"Fallout4" / L"F4SE";
#endif
            CoTaskMemFree(documents);
            std::error_code ec;
            fs::create_directories(dir, ec);
            g_log = _wfsopen((dir / L"DumpDiverFreezeTest.log").c_str(), L"w", _SH_DENYWR);
        }
        Log("DumpDiverFreezeTest loaded");
        if (!loader || loader->isEditor)
            return true;
        g_tasks = static_cast<xse::TaskInterface*>(loader->QueryInterface(xse::kInterface_Task));
        wchar_t     folder[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"MODTESTLAB_DIR", folder, MAX_PATH);
        if (!g_tasks || !n || n >= MAX_PATH) {
            Log("no task interface or no MODTESTLAB_DIR: idle");
            return true;
        }
        std::thread(Watch, fs::path(folder)).detach();
        return true;
    }

    constexpr std::uint32_t kVersion = 1 << 24;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

#if defined(DD_GAME_SKYRIM)
struct SKSEPluginVersionData
{
    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char          name[256];
    char          author[256];
    char          supportEmail[252];
    std::uint32_t versionIndependenceEx;
    std::uint32_t versionIndependence;
    std::uint32_t compatibleVersions[16];
    std::uint32_t seVersionRequired;
};
extern "C" __declspec(dllexport) SKSEPluginVersionData SKSEPlugin_Version = {
    1, kVersion, "DumpDiverFreezeTest", "Asciimov", "", 1 << 0, 1 << 1, { 0 }, 0,
};
extern "C" __declspec(dllexport) bool SKSEPlugin_Query(const xse::Interface*, xse::PluginInfo* info)
{
    info->infoVersion = 1;
    info->name = "DumpDiverFreezeTest";
    info->version = kVersion;
    return true;
}
extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const xse::Interface* skse) { return Load(skse); }
#else
struct F4SEPluginVersionData
{
    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char          name[256];
    char          author[256];
    std::uint32_t addressIndependence;
    std::uint32_t structureIndependence;
    std::uint32_t compatibleVersions[16];
    std::uint32_t xseMinimum;
    std::uint32_t reservedNonBreaking;
    std::uint32_t reservedBreaking;
    std::uint8_t  reserved[512];
};
extern "C" __declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
    1, kVersion, "DumpDiverFreezeTest", "Asciimov", 1 << 0, 1 << 0, { 0 }, 0, 0, 0, { 0 },
};
extern "C" __declspec(dllexport) bool F4SEPlugin_Query(const xse::Interface*, xse::PluginInfo* info)
{
    info->infoVersion = 1;
    info->name = "DumpDiverFreezeTest";
    info->version = kVersion;
    return true;
}
extern "C" __declspec(dllexport) bool F4SEPlugin_Load(const xse::Interface* f4se) { return Load(f4se); }
#endif
