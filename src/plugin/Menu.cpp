// Dump Diver - the settings page in SKSE Menu Framework's Mod Control Panel.
//
// SKSE Menu Framework (Nexus SE 120352) draws its panel with Dear ImGui and exports the cimgui functions next to its
// own AddSectionItem. The few used here are looked up by name: nothing is linked, and a missing DLL or export only
// leaves the page out. The framework calls the page once per frame on the game's render thread, and only while the
// page is open, so the page only reads and writes the settings in shared memory and never touches a file; the INI
// is written by a background thread a moment after the last change.
//
// The freeze limits and the report options live in shared memory, which the helper reads on every pass, so a change
// applies at once. What the plugin sets up while the game starts (task, lock and loading screen watching) applies
// after a restart, and the page says so.

#include "plugin/Menu.h"

#include "plugin/Log.h"
#include "plugin/Plugin.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>

namespace dd::plugin::menu
{
#if defined(DD_GAME_SKYRIM)
    namespace
    {
        struct ImVec2
        {
            float x, y;
        };
        using RenderFunction = void(__stdcall*)();
        using TextV = void (*)(const char*, va_list);

        // Every flags argument is 0: flag values differ between ImGui versions.
        struct Api
        {
            void (*AddSectionItem)(const char* path, RenderFunction render) = nullptr;
            bool (*Checkbox)(const char* label, bool* value) = nullptr;
            bool (*SliderInt)(const char* label, int* value, int min, int max, const char* format, int flags) = nullptr;
            bool (*Button)(const char* label, ImVec2 size) = nullptr;
            TextV SetItemTooltipV = nullptr;
            TextV TextWrappedV = nullptr;
            TextV TextDisabledV = nullptr;
            void (*SeparatorText)(const char* label) = nullptr;
            void (*Spacing)() = nullptr;
        };
        Api api;  // complete before the page is added, then only read

        std::wstring      g_ini;
        std::string       g_outputDir;  // UTF-8, for the page
        std::string       g_hotkey;
        HANDLE            g_saveEvent = nullptr;
        std::atomic<bool> g_saveFailed{ false };
        std::atomic<bool> g_restartPending{ false };

        // Options read once at startup: the page edits a copy, written to the INI, used after a restart.
        struct StartupOption
        {
            const wchar_t* section;
            const wchar_t* key;
            bool           value;
        };
        StartupOption g_trackTasks{ L"General", L"TrackTasks", true };
        StartupOption g_trackLocks{ L"General", L"TrackLocks", true };
        StartupOption g_watchLoading{ L"General", L"WatchLoadingScreens", true };
        std::atomic<bool> g_startupValues[3];

        template <class F>
        bool Resolve(HMODULE module, const char* name, F& out)
        {
            out = reinterpret_cast<F>(GetProcAddress(module, name));
            if (!out)
                DD_LOG_INFO("SKSE Menu Framework has no %s; no menu page", name);
            return out != nullptr;
        }

        // ImGui's text functions take a printf format; text always goes in as its argument.
        void CallV(TextV f, const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            f(format, args);
            va_end(args);
        }
        void Tooltip(const char* help) { CallV(api.SetItemTooltipV, "%s", help); }

        std::string Utf8(const std::wstring& text)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        void Changed() { SetEvent(g_saveEvent); }

        void Store(std::uint32_t& field, std::uint32_t value)
        {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&field), static_cast<LONG>(value));
            Changed();
        }

        void Slider(const char* label, std::uint32_t& field, int min, int max, const char* format, const char* help)
        {
            int value = static_cast<int>(field);
            if (api.SliderInt(label, &value, min, max, format, 0))
                Store(field, static_cast<std::uint32_t>(value < min ? min : value > max ? max : value));
            Tooltip(help);
        }

        void Check(const char* label, std::uint32_t& field, const char* help)
        {
            bool value = field != 0;
            if (api.Checkbox(label, &value))
                Store(field, value ? 1 : 0);
            Tooltip(help);
        }

        void CheckAtStartup(const char* label, std::atomic<bool>& value, const char* help)
        {
            bool current = value.load();
            if (api.Checkbox(label, &current)) {
                value = current;
                g_restartPending = true;
                Changed();
            }
            Tooltip(help);
        }

        void WriteInt(const wchar_t* section, const wchar_t* key, std::uint32_t value, bool& ok)
        {
            wchar_t text[16];
            swprintf_s(text, L"%u", value);
            ok &= WritePrivateProfileStringW(section, key, text, g_ini.c_str()) != FALSE;
        }

        // Writes every option the page shows, 300 ms after the last change; WritePrivateProfileString keeps the
        // file's comments and order.
        void SaveLoop()
        {
            for (;;) {
                WaitForSingleObject(g_saveEvent, INFINITE);
                while (WaitForSingleObject(g_saveEvent, 300) == WAIT_OBJECT_0) {
                }
                const Header* shared = Shared();
                if (!shared)
                    continue;
                const HelperSettings& h = shared->settings;
                bool ok = true;
                WriteInt(L"Freeze", L"FreezeSeconds", h.freezeSeconds, ok);
                WriteInt(L"Freeze", L"LoadingSeconds", h.loadingSeconds, ok);
                WriteInt(L"Freeze", L"LoadingScreenSeconds", h.loadingScreenSeconds, ok);
                WriteInt(L"Freeze", L"LoadingScreenAdaptive", h.loadingScreenAdaptive, ok);
                WriteInt(L"Freeze", L"FreezePrompt", h.freezePrompt, ok);
                WriteInt(L"Freeze", L"WriteDump", h.writeDump, ok);
                WriteInt(L"Freeze", L"KeepRecoveredStalls", h.keepRecovered, ok);
                WriteInt(L"Freeze", L"MaxReports", h.maxReports, ok);
                const StartupOption* options[] = { &g_trackTasks, &g_trackLocks, &g_watchLoading };
                for (int i = 0; i < 3; ++i)
                    WriteInt(options[i]->section, options[i]->key, g_startupValues[i].load() ? 1 : 0, ok);
                if (ok != !g_saveFailed.load()) {
                    if (ok)
                        DD_LOG_INFO("menu: settings saved to DumpDiver.ini again");
                    else
                        DD_LOG_WARN("menu: DumpDiver.ini could not be written (error %lu)", GetLastError());
                }
                g_saveFailed = !ok;
            }
        }

        void Failed(const char* what)
        {
            static bool logged = false;  // the render thread only
            if (!logged)
                DD_LOG_WARN("menu page: %s; the rest of the page is left out while this happens", what);
            logged = true;
        }

        void __stdcall RenderSettings()
        {
            try {
                Header* shared = Shared();
                if (!shared)
                    return;
                HelperSettings& h = shared->settings;
                CallV(api.TextDisabledV, "%s", "Dump Diver " DD_VERSION_STRING ". Changes are saved to Data\\SKSE\\Plugins\\DumpDiver.ini.");
                if (g_saveFailed)
                    CallV(api.TextWrappedV, "%s", "Not saved: DumpDiver.ini could not be written (see DumpDiver.log). The changes last until you quit; the next change tries again.");

                api.SeparatorText("Freeze detection");
                Slider("Frozen after##FreezeSeconds", h.freezeSeconds, 5, 60, "%d s",
                       "Seconds without a new frame before the game counts as frozen. Applies at once.");
                Slider("Frozen after, while loading##LoadingSeconds", h.loadingSeconds, 30, 600, "%d s",
                       "The same while a save or a loading screen loads, which can take a while on a big load order. Applies at once.");
                Slider("Endless loading screen after##LoadingScreenSeconds", h.loadingScreenSeconds, 30, 900, "%d s",
                       "A loading screen up longer than this, while the game still runs, is reported as endless. Applies at once.");
                Check("Grow with your longest loading screen##LoadingScreenAdaptive", h.loadingScreenAdaptive,
                      "Raises the endless loading screen limit to three times the longest loading screen of this session (up to 15 minutes), "
                      "so a slow load order is not reported. Applies at once.");

                api.SeparatorText("Reports");
                Check("Ask to quit and open the report##FreezePrompt", h.freezePrompt,
                      "After a freeze report, ask \"It looks like your game is frozen. Quit game and open freeze report?\" The question closes "
                      "by itself if the game comes back. Applies at once.");
                Check("Write a .dmp file for mod authors##WriteDump", h.writeDump,
                      "Writes a .dmp (about 1 MB) next to each freeze report; mod authors can open it in a debugger. The newest 5 are kept. "
                      "Applies at once.");
                Check("Also for stalls the game recovered from##KeepRecoveredStalls", h.keepRecovered,
                      "A stall the game came back from by itself always gets a report; this also writes its .dmp. Applies at once.");
                Slider("Reports kept##MaxReports", h.maxReports, 5, 100, "%d",
                       "The number of reports kept; older ones are deleted. Applies to the next report.");
                api.Spacing();
                if (shared->helperProcessId) {
                    if (api.Button("Write a report now", ImVec2{ 0, 0 }))
                        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shared->captureRequests));
                    Tooltip("A report of what the game is doing right now, the same as the manual report key. Useful when something "
                            "is wrong that the automatic checks do not catch.");
                } else {
                    CallV(api.TextDisabledV, "%s", "The helper is not running, so no report can be written (see DumpDiver.log).");
                }
                CallV(api.TextDisabledV, "Reports go to %s", g_outputDir.c_str());
                if (!g_hotkey.empty())
                    CallV(api.TextDisabledV, "Manual report key: %s (ManualHotkey in DumpDiver.ini, restart)", g_hotkey.c_str());

                api.SeparatorText("Watching (restart)");
                CheckAtStartup("Name the mod behind a stuck task (restart)##TrackTasks", g_startupValues[0],
                               "Records which plugin queued each task the game runs, so a freeze inside a task names that plugin. "
                               "Applies after you restart the game.");
                CheckAtStartup("Notice locks a mod leaves locked (restart)##TrackLocks", g_startupValues[1],
                               "Notices game locks a mod leaves locked, so a later freeze on that lock names the mod. Applies after you "
                               "restart the game.");
                CheckAtStartup("Watch loading screens (restart)##WatchLoadingScreens", g_startupValues[2],
                               "Catches loading screens that never end. Applies after you restart the game.");
                api.Spacing();
                if (g_restartPending)
                    CallV(api.TextWrappedV, "%s", "Restart the game to apply the changes marked (restart).");
            } catch (...) {
                Failed("unknown error");
            }
        }
    }

    void Register(const std::filesystem::path& ini)
    {
        const HMODULE module = GetModuleHandleW(L"SKSEMenuFramework.dll");
        if (!module) {
            DD_LOG_INFO("SKSE Menu Framework not loaded; no menu page");
            return;
        }
        Api found;
        const bool complete =
            Resolve(module, "AddSectionItem", found.AddSectionItem) && Resolve(module, "igCheckbox", found.Checkbox) &&
            Resolve(module, "igSliderInt", found.SliderInt) && Resolve(module, "igButton", found.Button) &&
            Resolve(module, "igSetItemTooltipV", found.SetItemTooltipV) && Resolve(module, "igTextWrappedV", found.TextWrappedV) &&
            Resolve(module, "igTextDisabledV", found.TextDisabledV) && Resolve(module, "igSeparatorText", found.SeparatorText) &&
            Resolve(module, "igSpacing", found.Spacing);
        const Header* shared = Shared();
        if (!complete || !shared)
            return;

        g_ini = ini.wstring();
        g_outputDir = Utf8(shared->settings.outputDir);
        wchar_t hotkey[128]{};
        GetPrivateProfileStringW(L"Freeze", L"ManualHotkey", L"Ctrl+Shift+F10", hotkey, 128, g_ini.c_str());
        g_hotkey = shared->settings.hotkeyKey ? Utf8(hotkey) : std::string();
        const StartupOption* options[] = { &g_trackTasks, &g_trackLocks, &g_watchLoading };
        for (int i = 0; i < 3; ++i)
            g_startupValues[i] = GetPrivateProfileIntW(options[i]->section, options[i]->key, options[i]->value ? 1 : 0, g_ini.c_str()) != 0;

        g_saveEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_saveEvent)
            return;
        std::thread(SaveLoop).detach();
        api = found;
        // Once, at kPostLoad: nothing renders yet. The framework splits the path at '/' (section, then page).
        api.AddSectionItem("Dump Diver/Settings", RenderSettings);
        DD_LOG_INFO("SKSE Menu Framework: Dump Diver page added");
    }
#else
    void Register(const std::filesystem::path&) {}
#endif
}
