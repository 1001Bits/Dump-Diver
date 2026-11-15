// Dump Diver - game plugin.
//
// The plugin only records; the helper process does all the work. It keeps a heartbeat
// (a task the game runs every 100 ms), the main thread id, which plugin queued each task
// that is running, and recent load/save events, all in shared memory that the helper can
// read even when every game thread is stuck. It touches no game code and no game
// structures, so one build loads on every runtime its script extender supports.

#include "plugin/Plugin.h"

#include "plugin/LoadingScreen.h"
#include "plugin/LockWatch.h"
#include "plugin/Log.h"
#include "plugin/Menu.h"
#include "plugin/TaskTracking.h"

#include <intrin.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <new>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace dd::plugin
{
    namespace
    {
#if defined(DD_GAME_SKYRIM)
        constexpr Game           kGame = Game::kSkyrim;
        constexpr const wchar_t* kGameArg = L"skyrim";
#else
        constexpr Game           kGame = Game::kFallout4;
        constexpr const wchar_t* kGameArg = L"fallout4";
#endif

        struct Settings
        {
            bool          startHelper = true;
            bool          trackTasks = true;
            bool          trackLocks = true;
            std::uint32_t heartbeatMs = 100;
        };

        HANDLE               g_mapping = nullptr;
        Header*              g_shared = nullptr;
        Settings             g_settings;
        std::atomic<bool>    g_heartbeatQueued{ false };
        std::atomic<bool>    g_firstBeat{ true };
        xse::TaskInterface*  g_tasks = nullptr;
        fs::path             g_ini;

        fs::path ModulePath(HMODULE module)
        {
            std::wstring buffer(32768, L'\0');
            const DWORD  n = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
            buffer.resize(n);
            return buffer;
        }

        bool EqualsI(std::wstring_view a, std::wstring_view b)
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (std::towlower(a[i]) != std::towlower(b[i]))
                    return false;
            }
            return true;
        }

        // Documents\My Games\<game>\<SKSE|F4SE>, where the script extender keeps its logs.
        fs::path LoaderLogDir(const fs::path& exe)
        {
            wchar_t     overrideDir[32768];
            const DWORD n = GetEnvironmentVariableW(L"DUMPDIVER_LOGDIR", overrideDir, 32768);
            if (n && n < 32768)
                return overrideDir;  // the tests keep everything in their own folder

            const fs::path  gameDir = exe.parent_path();
            const wchar_t*  game = nullptr;
            std::error_code ec;
#if defined(DD_GAME_SKYRIM)
            if (EqualsI(exe.stem().native(), L"SkyrimVR"))
                game = L"Skyrim VR";
            else if (fs::exists(gameDir / L"Galaxy64.dll", ec))
                game = L"Skyrim Special Edition GOG";
            else
                game = L"Skyrim Special Edition";
            const wchar_t* loader = L"SKSE";
#else
            game = EqualsI(exe.stem().native(), L"Fallout4VR") ? L"Fallout4VR" : L"Fallout4";
            const wchar_t* loader = L"F4SE";
#endif
            PWSTR documents = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)))
                return gameDir / L"Data" / loader / L"Plugins";
            fs::path dir = fs::path(documents) / L"My Games" / game / loader;
            CoTaskMemFree(documents);
            return dir;
        }

        std::uint32_t IniInt(const fs::path& ini, const wchar_t* section, const wchar_t* key, std::uint32_t fallback)
        {
            return GetPrivateProfileIntW(section, key, static_cast<INT>(fallback), ini.c_str());
        }

        std::wstring IniString(const fs::path& ini, const wchar_t* section, const wchar_t* key)
        {
            wchar_t     buffer[1024]{};
            const DWORD n = GetPrivateProfileStringW(section, key, L"", buffer, 1024, ini.c_str());
            std::wstring value(buffer, n);
            while (!value.empty() && (value.back() == L' ' || value.back() == L'\t'))
                value.pop_back();
            if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
                value = value.substr(1, value.size() - 2);
            return value;
        }

        // "Ctrl+Shift+F10" -> MOD_CONTROL | MOD_SHIFT, VK_F10. Returns false for an empty or
        // unknown text.
        bool ParseHotkey(std::wstring text, std::uint32_t& modifiers, std::uint32_t& key)
        {
            modifiers = 0;
            key = 0;
            for (auto& c : text)
                c = static_cast<wchar_t>(std::towlower(c));
            std::size_t start = 0;
            while (start <= text.size()) {
                std::size_t end = text.find(L'+', start);
                if (end == std::wstring::npos)
                    end = text.size();
                std::wstring token = text.substr(start, end - start);
                while (!token.empty() && token.front() == L' ')
                    token.erase(token.begin());
                while (!token.empty() && token.back() == L' ')
                    token.pop_back();
                start = end + 1;
                if (token.empty())
                    continue;
                if (token == L"ctrl" || token == L"control")
                    modifiers |= MOD_CONTROL;
                else if (token == L"shift")
                    modifiers |= MOD_SHIFT;
                else if (token == L"alt")
                    modifiers |= MOD_ALT;
                else if (token == L"win")
                    modifiers |= MOD_WIN;
                else if (token.size() >= 2 && token[0] == L'f' && std::iswdigit(token[1])) {
                    const int n = _wtoi(token.c_str() + 1);
                    if (n < 1 || n > 24)
                        return false;
                    key = VK_F1 + static_cast<std::uint32_t>(n - 1);
                } else if (token.size() == 1 && (std::iswalpha(token[0]) || std::iswdigit(token[0])))
                    key = static_cast<std::uint32_t>(std::towupper(token[0]));
                else if (token == L"pause")
                    key = VK_PAUSE;
                else if (token == L"insert")
                    key = VK_INSERT;
                else if (token == L"delete")
                    key = VK_DELETE;
                else if (token == L"home")
                    key = VK_HOME;
                else if (token == L"end")
                    key = VK_END;
                else if (token == L"pageup")
                    key = VK_PRIOR;
                else if (token == L"pagedown")
                    key = VK_NEXT;
                else if (token == L"backspace")
                    key = VK_BACK;
                else
                    return false;
            }
            return key != 0;
        }

        void LoadSettings(const fs::path& ini, HelperSettings& helper, const fs::path& defaultOutput)
        {
            g_settings.startHelper = IniInt(ini, L"General", L"StartHelper", 1) != 0;
            g_settings.trackTasks = IniInt(ini, L"General", L"TrackTasks", 1) != 0;
            g_settings.trackLocks = IniInt(ini, L"General", L"TrackLocks", 1) != 0;
            g_settings.heartbeatMs = std::clamp<std::uint32_t>(IniInt(ini, L"General", L"HeartbeatMs", 100), 20, 1000);

            helper.freezeSeconds = std::clamp<std::uint32_t>(IniInt(ini, L"Freeze", L"FreezeSeconds", 10), 3, 600);
            helper.loadingSeconds = std::clamp<std::uint32_t>(IniInt(ini, L"Freeze", L"LoadingSeconds", 120), 10, 3600);
            helper.startupSeconds = std::clamp<std::uint32_t>(IniInt(ini, L"Freeze", L"StartupSeconds", 600), 60, 7200);
            helper.sampleMilliseconds =
                std::clamp<std::uint32_t>(IniInt(ini, L"Freeze", L"SampleMilliseconds", 3000), 500, 20000);
            helper.sampleHz = std::clamp<std::uint32_t>(IniInt(ini, L"Freeze", L"SampleHz", 40), 5, 200);
            helper.writeDump = IniInt(ini, L"Freeze", L"WriteDump", 1);
            helper.fullMemoryDump = IniInt(ini, L"Freeze", L"FullMemoryDump", 0);
            helper.keepRecovered = IniInt(ini, L"Freeze", L"KeepRecoveredStalls", 0);
            helper.freezePrompt = IniInt(ini, L"Freeze", L"FreezePrompt", 1);
            helper.maxReports = std::clamp<std::uint32_t>(IniInt(ini, L"Freeze", L"MaxReports", 20), 1, 1000);
            helper.loadingScreenSeconds =
                std::clamp<std::uint32_t>(IniInt(ini, L"Freeze", L"LoadingScreenSeconds", 180), 1, 3600);
            helper.loadingScreenAdaptive = IniInt(ini, L"Freeze", L"LoadingScreenAdaptive", 1);

            std::wstring hotkey = IniString(ini, L"Freeze", L"ManualHotkey");
            if (hotkey.empty())
                hotkey = L"Ctrl+Shift+F10";
            if (!(hotkey == L"0" || hotkey == L"none" || hotkey == L"None")) {
                std::uint32_t modifiers = 0;
                std::uint32_t key = 0;
                if (ParseHotkey(hotkey, modifiers, key)) {
                    helper.hotkeyModifiers = modifiers;
                    helper.hotkeyKey = key;
                } else {
                    DD_LOG_WARN("ManualHotkey '%ls' is not understood; the manual capture key is off", hotkey.c_str());
                }
            }

            fs::path output = IniString(ini, L"General", L"OutputDir");
            if (output.empty())
                output = defaultOutput;
            else if (output.is_relative())
                output = ini.parent_path() / output;
            const std::wstring outputText = output.lexically_normal().wstring();
            wcsncpy_s(helper.outputDir, outputText.c_str(), _TRUNCATE);
        }

        bool CreateShared()
        {
            const std::wstring name = std::wstring(kMappingPrefix) + std::to_wstring(GetCurrentProcessId());
            g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Header), name.c_str());
            if (!g_mapping) {
                DD_LOG_ERROR("cannot create the shared memory (error %lu)", GetLastError());
                return false;
            }
            g_shared = static_cast<Header*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Header)));
            if (!g_shared) {
                DD_LOG_ERROR("cannot map the shared memory (error %lu)", GetLastError());
                return false;
            }
            std::memset(g_shared, 0, sizeof(Header));
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            g_shared->version = kProtocolVersion;
            g_shared->size = sizeof(Header);
            g_shared->game = static_cast<std::uint32_t>(kGame);
            g_shared->pluginVersion = PackVersion(DD_VERSION_MAJOR, DD_VERSION_MINOR, DD_VERSION_PATCH);
            g_shared->processId = GetCurrentProcessId();
            g_shared->mainThreadId = GetCurrentThreadId();
            g_shared->qpcFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
            g_shared->startQpc = Qpc();
            return true;
        }

        void Beat()
        {
            if (!g_shared)
                return;
            const std::uint64_t now = Qpc();
            CheckLoadingScreenClosed(now);
            g_shared->heartbeatQpc = now;
            g_shared->heartbeatThreadId = GetCurrentThreadId();
            g_shared->heartbeatCount = g_shared->heartbeatCount + 1;
            if (g_firstBeat.exchange(false))
                PushEvent(EventType::kFirstHeartbeat);
            g_heartbeatQueued.store(false);
        }

        class HeartbeatTask final : public xse::Task
        {
        public:
            void Run() override { Beat(); }
#if defined(DD_GAME_SKYRIM)
            void Dispose() override { delete this; }
#endif
        };

        void HeartbeatLoop()
        {
            for (;;) {
                if (!g_heartbeatQueued.exchange(true)) {
                    auto* task = new (std::nothrow) HeartbeatTask();
                    if (!task || !QueueUntracked(task)) {
                        delete task;
                        g_heartbeatQueued.store(false);
                    }
                }
                LogLockRecords();
                Sleep(g_settings.heartbeatMs);
            }
        }

        void SetState(std::uint32_t flag, bool on)
        {
            if (!g_shared)
                return;
            auto* flags = reinterpret_cast<volatile LONG*>(&g_shared->stateFlags);
            if (on)
                InterlockedOr(flags, static_cast<LONG>(flag));
            else
                InterlockedAnd(flags, ~static_cast<LONG>(flag));
        }

        void OnMessage(xse::Message* message)
        {
            if (!message)
                return;
#if defined(DD_GAME_SKYRIM)
            namespace m = xse::skse;
            switch (message->type) {
            case m::kPostLoad:
                PushEvent(EventType::kPostLoad);
                menu::Register(g_ini);  // after every plugin loaded, so SKSE Menu Framework is there if installed
                break;
            case m::kInputLoaded:
                PushEvent(EventType::kInputLoaded);
                break;
            case m::kDataLoaded:
                SetState(kState_DataLoaded, true);
                PushEvent(EventType::kDataLoaded);
                break;
            case m::kNewGame:
                PushEvent(EventType::kNewGame);
                break;
            case m::kPreLoadGame:
                g_shared->loadStartQpc = Qpc();
                SetState(kState_LoadingSave, true);
                PushEvent(EventType::kPreLoadGame);
                break;
            case m::kPostLoadGame:
                SetState(kState_LoadingSave, false);
                PushEvent(EventType::kPostLoadGame, reinterpret_cast<std::uint64_t>(message->data) != 0);
                break;
            case m::kSaveGame:
                PushEvent(EventType::kSaveGame);
                break;
            case m::kDeleteGame:
                PushEvent(EventType::kDeleteGame);
                break;
            default:
                break;
            }
#else
            namespace m = xse::f4se;
            switch (message->type) {
            case m::kPostLoad:
                PushEvent(EventType::kPostLoad);
                break;
            case m::kInputLoaded:
                PushEvent(EventType::kInputLoaded);
                break;
            case m::kGameDataReady:
                if (message->data && *static_cast<const bool*>(message->data)) {
                    SetState(kState_DataLoaded, true);
                    PushEvent(EventType::kDataLoaded);
                }
                break;
            case m::kNewGame:
                PushEvent(EventType::kNewGame);
                break;
            case m::kPreLoadGame:
                g_shared->loadStartQpc = Qpc();
                SetState(kState_LoadingSave, true);
                PushEvent(EventType::kPreLoadGame);
                break;
            case m::kPostLoadGame:
                SetState(kState_LoadingSave, false);
                PushEvent(EventType::kPostLoadGame, reinterpret_cast<std::uint64_t>(message->data) != 0);
                break;
            case m::kPreSaveGame:
                SetState(kState_Saving, true);
                PushEvent(EventType::kSaveGame);
                break;
            case m::kPostSaveGame:
                SetState(kState_Saving, false);
                PushEvent(EventType::kPostSaveGame);
                break;
            case m::kDeleteGame:
                PushEvent(EventType::kDeleteGame);
                break;
            default:
                break;
            }
#endif
        }

        bool StartHelper(const fs::path& helperExe)
        {
            std::error_code ec;
            if (!fs::exists(helperExe, ec)) {
                DD_LOG_ERROR("helper not found: %ls", helperExe.c_str());
                return false;
            }
            std::wstring command = L"\"" + helperExe.wstring() + L"\" --pid " + std::to_wstring(GetCurrentProcessId()) +
                                   L" --game " + kGameArg;
            STARTUPINFOW        startup{ sizeof(startup) };
            PROCESS_INFORMATION process{};
            const fs::path      workDir = helperExe.parent_path();
            // Break away from the game's job (if it has one) so the helper outlives a crash.
            DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;
            BOOL  ok = CreateProcessW(helperExe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                                      workDir.c_str(), &startup, &process);
            if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
                flags &= ~static_cast<DWORD>(CREATE_BREAKAWAY_FROM_JOB);
                ok = CreateProcessW(helperExe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                                    workDir.c_str(), &startup, &process);
            }
            if (!ok) {
                DD_LOG_ERROR("cannot start the helper (error %lu)", GetLastError());
                return false;
            }
            g_shared->helperProcessId = process.dwProcessId;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            PushEvent(EventType::kHelperStarted, process.dwProcessId);
            DD_LOG_INFO("helper started (pid %lu)", process.dwProcessId);
            return true;
        }
    }

    namespace
    {
        // Test-only (DiagnoseCrashes=1): logs where access violations happen. Dump Diver is not a
        // crash logger; this exists to find faults in its own hooks during lab runs.
        std::string AddressText(std::uint64_t address)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &module) ||
                !module) {
                char text[32];
                snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
                return text;
            }
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(module, path, MAX_PATH);
            char text[400];
            snprintf(text, sizeof(text), "%ls+0x%llx", fs::path(path).filename().c_str(),
                     static_cast<unsigned long long>(address - reinterpret_cast<std::uint64_t>(module)));
            return text;
        }

        LONG CALLBACK CrashProbe(EXCEPTION_POINTERS* info)
        {
            static volatile LONG count = 0;
            const DWORD code = info->ExceptionRecord->ExceptionCode;
            if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION)
                return EXCEPTION_CONTINUE_SEARCH;
            if (InterlockedIncrement(&count) > 6)
                return EXCEPTION_CONTINUE_SEARCH;
            const CONTEXT& c = *info->ContextRecord;
            DD_LOG_ERROR("exception 0x%08lx at %s (thread %lu), %s 0x%llx", code, AddressText(c.Rip).c_str(), GetCurrentThreadId(),
                         info->ExceptionRecord->NumberParameters >= 2 && info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                         info->ExceptionRecord->NumberParameters >= 2 ? static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]) : 0ull);
            DD_LOG_ERROR("  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx rsp=%llx",
                         c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.Rsp);
            const auto* stack = reinterpret_cast<const std::uint64_t*>(c.Rsp);
            int         shown = 0;
            for (int i = 0; i < 96 && shown < 14; ++i) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (!VirtualQuery(&stack[i], &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
                    break;
                const std::uint64_t value = stack[i];
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(value), &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
                    !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
                    continue;
                DD_LOG_ERROR("  [rsp+%03x] %s", i * 8, AddressText(value).c_str());
                ++shown;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    Header* Shared() { return g_shared; }

    void PushEvent(EventType type, std::uint64_t a, std::uint64_t b)
    {
        if (!g_shared)
            return;
        const auto index =
            static_cast<std::uint64_t>(InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_shared->eventIndex))) - 1;
        Event& event = g_shared->events[index % kEventCount];
        event.qpc = Qpc();
        event.type = static_cast<std::uint32_t>(type);
        event.threadId = GetCurrentThreadId();
        event.a = a;
        event.b = b;
    }

    bool Load(HMODULE module, const xse::Interface* loader)
    {
        const fs::path dll = ModulePath(module);
        const fs::path exe = ModulePath(nullptr);
        const fs::path logDir = LoaderLogDir(exe);
        LogOpen(logDir / L"DumpDiver.log");
        DD_LOG_INFO("Dump Diver " DD_VERSION_STRING " (%s)", xse::kLoaderName);
        DD_LOG_INFO("game: %ls", exe.c_str());

        if (!loader || loader->isEditor) {
            DD_LOG_INFO("not the game; nothing to do");
            return true;
        }
        const std::uint32_t runtime = loader->runtimeVersion;
        DD_LOG_INFO("runtime %u.%u.%u", runtime >> 24, (runtime >> 16) & 0xFF, (runtime >> 4) & 0xFFF);

        if (!CreateShared())
            return true;
        g_shared->runtimeVersion = runtime;

        const fs::path ini = dll.parent_path() / L"DumpDiver.ini";
        g_ini = ini;
        LoadSettings(ini, g_shared->settings, logDir / L"DumpDiver");
        DD_LOG_INFO("reports go to %ls", g_shared->settings.outputDir);

        g_tasks = static_cast<xse::TaskInterface*>(loader->QueryInterface(xse::kInterface_Task));
        if (!InitTasks(g_tasks)) {
            DD_LOG_ERROR("no task interface; freezes cannot be detected");
            return true;
        }
        if (g_settings.trackTasks) {
            if (InstallTaskTracking(g_tasks)) {
                g_shared->taskTracking = 1;
                DD_LOG_INFO("tracking which plugin queued each running task");
            }
        }

        if (g_settings.trackLocks && InitLockWatch())
            DD_LOG_INFO("checking threads that exit for engine locks they still hold");
        if (auto* messaging =
                static_cast<xse::MessagingInterface*>(loader->QueryInterface(xse::kInterface_Messaging))) {
            if (g_settings.trackLocks && InstallListenerTracking(messaging))
                DD_LOG_INFO("checking message handlers for engine locks they leave held");
            RegisterOwnListener(messaging, loader->GetPluginHandle(), xse::kLoaderName, &OnMessage);
        }

        if (IniInt(ini, L"General", L"DiagnoseCrashes", 0))
            AddVectoredExceptionHandler(1, &CrashProbe);
        if (IniInt(ini, L"General", L"WatchLoadingScreens", 1))
            InstallLoadingScreenWatch();

        // The shared memory is complete; the helper may map it from here on.
        _WriteBarrier();
        g_shared->magic = kMagic;
        PushEvent(EventType::kPluginLoaded);

        std::thread(HeartbeatLoop).detach();

        if (g_settings.startHelper)
            StartHelper(dll.parent_path() / L"DumpDiver" / L"DumpDiverHelper.exe");
        else
            DD_LOG_INFO("StartHelper=0: start DumpDiverHelper.exe yourself to watch this game");
        return true;
    }
}
