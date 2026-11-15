// Dump Diver helper. Started by the game plugin with --pid; watches the heartbeat in shared
// memory and captures the game when it stops. Runs outside the game, so it keeps working when
// every game thread is stuck.

#include "helper/Analyze.h"
#include "helper/Capture.h"
#include "helper/Process.h"
#include "helper/Prompt.h"
#include "helper/Report.h"
#include "helper/SnapshotFile.h"
#include "helper/Util.h"

#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace dd;
using namespace dd::helper;

namespace
{
    struct Args
    {
        std::uint32_t pid = 0;
        Game          game = Game::kUnknown;
        bool          captureNow = false;  // one manual capture, then exit
        std::wstring  replay;              // analyze a saved capture again, then exit
        std::wstring  out;                 // where the replayed reports go
        std::wstring  promptPreview;       // draw the freeze prompt into this PNG, then exit
        std::wstring  image;               // picture for the preview
    };

    Args ParseArgs()
    {
        Args    args;
        int     argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--pid" && i + 1 < argc)
                args.pid = static_cast<std::uint32_t>(wcstoul(argv[++i], nullptr, 10));
            else if (arg == L"--game" && i + 1 < argc) {
                const std::wstring game = argv[++i];
                args.game = EqualsI(game, L"skyrim") ? Game::kSkyrim : EqualsI(game, L"fallout4") ? Game::kFallout4 : Game::kUnknown;
            } else if (arg == L"--capture-now")
                args.captureNow = true;
            else if (arg == L"--replay" && i + 1 < argc)
                args.replay = argv[++i];
            else if (arg == L"--out" && i + 1 < argc)
                args.out = argv[++i];
            else if (arg == L"--prompt-preview" && i + 1 < argc)
                args.promptPreview = argv[++i];
            else if (arg == L"--image" && i + 1 < argc)
                args.image = argv[++i];
        }
        if (argv)
            LocalFree(argv);
        return args;
    }

    BOOL CALLBACK FindDialog(HWND hwnd, LPARAM param)
    {
        auto* pid = reinterpret_cast<DWORD*>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner != *pid || !IsWindowVisible(hwnd))
            return TRUE;
        wchar_t className[16]{};
        GetClassNameW(hwnd, className, 16);
        if (wcscmp(className, L"#32770") != 0)
            return TRUE;
        *pid = 0;  // found
        return FALSE;
    }

    bool HasDialog(std::uint32_t pid)
    {
        DWORD value = pid;
        EnumWindows(&FindDialog, reinterpret_cast<LPARAM>(&value));
        return value == 0;
    }

    // Heartbeats that count as the game being back: a single one can be one slow frame of a game
    // that is still stuck.
    constexpr std::uint64_t kRecoveredBeats = 10;

    // A freeze, or a start that never reached the main loop: stuck until the game answers again.
    StillStuck UntilAnswering(const Header& shared, std::uint64_t countAtCapture)
    {
        return [&shared, countAtCapture] { return shared.heartbeatCount - countAtCapture < kRecoveredBeats; };
    }

    // An endless loading screen: the game keeps answering, so it is stuck while that screen is up.
    StillStuck WhileLoadingScreen(const Header& shared, std::uint64_t openQpc)
    {
        return [&shared, openQpc] { return shared.loadingOpenQpc == openQpc; };
    }

    // Offer to quit and open the report while the game is still stuck after the capture; the prompt
    // closes by itself once stillStuck says the game is back.
    void OfferReport(const Header& shared, std::uint32_t pid, const fs::path& report, StillStuck stillStuck)
    {
        if (!shared.settings.freezePrompt || report.empty() || !stillStuck())
            return;
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        ShowFreezePrompt(pid, report, fs::path(self).parent_path() / L"DumpDiver.png", std::move(stillStuck));
    }

    const Header* MapShared(std::uint32_t pid, HANDLE process, HANDLE& mapping)
    {
        const std::wstring name = std::wstring(kMappingPrefix) + std::to_wstring(pid);
        for (int attempt = 0; attempt < 300; ++attempt) {
            mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
            if (mapping) {
                auto* view = static_cast<const Header*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Header)));
                if (view) {
                    for (int wait = 0; wait < 100 && view->magic != kMagic; ++wait)
                        Sleep(100);
                    if (view->magic == kMagic && view->version == kProtocolVersion && view->size == sizeof(Header))
                        return view;
                    UnmapViewOfFile(view);
                }
                CloseHandle(mapping);
                mapping = nullptr;
                return nullptr;
            }
            if (WaitForSingleObject(process, 100) == WAIT_OBJECT_0)
                return nullptr;
        }
        return nullptr;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const Args args = ParseArgs();
    if (!args.promptPreview.empty()) {
        // DumpDiverHelper --prompt-preview <out.png> [--image <picture.png>]
        PreviewFreezePrompt(args.image, args.promptPreview);
        return 0;
    }
    if (!args.replay.empty()) {
        // DumpDiverHelper --replay <capture.ddsnap> [--out <folder>]: the reports this version writes for it.
        Snapshot snapshot;
        if (!LoadSnapshot(args.replay, snapshot))
            return 5;
        const fs::path source(args.replay);
        const fs::path dir = args.out.empty() ? source.parent_path() : fs::path(args.out);
        std::error_code ec;
        fs::create_directories(dir, ec);
        WriteReports(snapshot, Analyze(snapshot), dir / (source.stem().wstring() + L"_replay"));
        return 0;
    }
    if (!args.pid)
        return 2;

    Target target;
    target.pid = args.pid;
    target.process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, args.pid);
    if (!target.process)
        return 3;

    HANDLE mapping = nullptr;
    target.shared = MapShared(args.pid, target.process, mapping);
    if (!target.shared)
        return 4;
    const Header& shared = *target.shared;
    target.game = args.game != Game::kUnknown ? args.game : static_cast<Game>(shared.game);
    target.exePath = ProcessImagePath(target.process);

    const fs::path outputDir = shared.settings.outputDir;
    LogOpen(outputDir / L"DumpDiverHelper.log");
    Log(L"Dump Diver helper %s watching pid %u (%s)", Wide(DD_VERSION_STRING).c_str(), args.pid, target.exePath.c_str());
    Log(L"freeze after %u s without a heartbeat (%u s while loading, %u s at startup); loading screens over %u s are endless",
        shared.settings.freezeSeconds, shared.settings.loadingSeconds, shared.settings.startupSeconds,
        shared.settings.loadingScreenSeconds);
    Log(shared.loadingWatch ? L"the game reports its loading screens" : L"the game does not report loading screens (see the plugin log)");

    if (args.captureNow) {
        target.window = FindGameWindow(args.pid);
        Capture(target, CaptureReason::kManual, shared.heartbeatCount);
        return 0;
    }

    constexpr int kHotkeyId = 1;
    if (shared.settings.hotkeyKey) {
        if (RegisterHotKey(nullptr, kHotkeyId, shared.settings.hotkeyModifiers | MOD_NOREPEAT, shared.settings.hotkeyKey))
            Log(L"manual capture key registered");
        else
            Log(L"manual capture key could not be registered (error %lu); another program may use it", GetLastError());
    }

    std::uint64_t lastCount = shared.heartbeatCount;
    std::uint64_t loadingCaptured = 0;  // the loading screen (by its open time) already captured
    bool          stallCaptured = false;
    bool          startupCaptured = false;
    bool          pausedLogged = false;
    std::uint32_t captureRequests = shared.captureRequests;
    const double  frequency = static_cast<double>(shared.qpcFrequency ? shared.qpcFrequency : 1);

    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1, &target.process, FALSE, 250, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0)
            break;  // the game exited

        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_HOTKEY && message.wParam == kHotkeyId) {
                if (!target.window || !IsWindow(target.window))
                    target.window = FindGameWindow(args.pid);
                Capture(target, CaptureReason::kManual, shared.heartbeatCount);
            }
        }
        if (shared.captureRequests != captureRequests) {
            // "Write a report now" on the settings page
            captureRequests = shared.captureRequests;
            if (!target.window || !IsWindow(target.window))
                target.window = FindGameWindow(args.pid);
            Capture(target, CaptureReason::kManual, shared.heartbeatCount);
        }

        const std::uint64_t count = shared.heartbeatCount;

        // An endless loading screen: the game keeps running (the heartbeat is fresh), but the
        // loading screen has been up far longer than loading takes. The limit grows with the
        // longest loading screen seen this session.
        const std::uint64_t loadingOpen = shared.loadingOpenQpc;
        if (loadingOpen && loadingOpen != loadingCaptured && shared.heartbeatQpc) {
            const std::uint64_t nowQpc = Qpc();
            const double        up = static_cast<double>(nowQpc - loadingOpen) / frequency;
            const double        heartbeatAge = static_cast<double>(nowQpc - shared.heartbeatQpc) / frequency;
            const double        grown = shared.settings.loadingScreenAdaptive ? std::min(900.0, 3.0 * shared.loadingLongestMs / 1000.0) : 0.0;
            const double        limit = std::max<double>(shared.settings.loadingScreenSeconds, grown);
            if (up >= limit && heartbeatAge < shared.settings.freezeSeconds) {
                if (!target.window || !IsWindow(target.window))
                    target.window = FindGameWindow(args.pid);
                Log(L"the loading screen has been up for %.0f s (limit %.0f s)", up, limit);
                OfferReport(shared, args.pid, Capture(target, CaptureReason::kEndlessLoading, count), WhileLoadingScreen(shared, loadingOpen));
                loadingCaptured = loadingOpen;
            }
        }

        if (count != lastCount) {
            if (stallCaptured)
                Log(L"the game recovered");
            lastCount = count;
            stallCaptured = false;
            pausedLogged = false;
            continue;
        }
        if (stallCaptured)
            continue;

        const std::uint64_t now = Qpc();
        if (!shared.heartbeatQpc) {
            // Never reached the main loop.
            const double since = static_cast<double>(now - shared.startQpc) / frequency;
            if (!startupCaptured && since >= shared.settings.startupSeconds) {
                target.window = FindGameWindow(args.pid);
                const fs::path report = Capture(target, CaptureReason::kStartup, count);
                if (shared.heartbeatCount == count)  // not one answer during the capture either
                    OfferReport(shared, args.pid, report, UntilAnswering(shared, count));
                startupCaptured = true;
            }
            continue;
        }

        const double   age = static_cast<double>(now - shared.heartbeatQpc) / frequency;
        const bool     loading = (shared.stateFlags & (kState_LoadingSave | kState_Saving)) != 0 || shared.loadingOpenQpc != 0;
        const unsigned threshold = loading ? shared.settings.loadingSeconds : shared.settings.freezeSeconds;
        if (age < threshold)
            continue;

        if (!target.window || !IsWindow(target.window))
            target.window = FindGameWindow(args.pid);
        if (!target.window)
            continue;  // no window: the game is closing or not up yet

        // A game in the background that still answers Windows is paused, not frozen, unless it
        // shows a dialog (an error message box keeps the window answering).
        const bool hung = IsHungAppWindow(target.window) != FALSE;
        DWORD      foregroundPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
        const bool foreground = foregroundPid == args.pid;
        if (!hung && !foreground && !HasDialog(args.pid)) {
            if (!pausedLogged)
                Log(L"no heartbeat for %.0f s, but the game is in the background and responding: treated as paused", age);
            pausedLogged = true;
            continue;
        }

        const fs::path report = Capture(target, CaptureReason::kFreeze, count);
        if (shared.heartbeatCount == count)  // not one answer during the capture either
            OfferReport(shared, args.pid, report, UntilAnswering(shared, count));
        stallCaptured = true;
    }

    Log(L"the game exited");
    // The view stays mapped until the helper ends: a freeze prompt still closing reads it.
    CloseHandle(mapping);
    CloseHandle(target.process);
    return 0;
}
