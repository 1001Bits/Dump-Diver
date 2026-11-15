// The freeze prompt, shown on a desktop of its own (never the user's). It must close by itself when
// the game is back or gone, and Enter or Esc must keep waiting, never quit the game. The "game" is
// this program started again with --sleep.

#include "helper/Prompt.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace dd::helper;
using Clock = std::chrono::steady_clock;

namespace
{
    int   g_failures = 0;
    HDESK g_desktop = nullptr;

    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    // A stand-in for the game process.
    struct Game
    {
        PROCESS_INFORMATION info{};

        explicit Game(DWORD sleepMs)
        {
            wchar_t self[MAX_PATH]{};
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring command = L"\"" + std::wstring(self) + L"\" --sleep " + std::to_wstring(sleepMs);
            STARTUPINFOW startup{ sizeof(startup) };
            CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
            if (info.hThread)
                CloseHandle(info.hThread);
        }
        ~Game()
        {
            if (info.hProcess) {
                TerminateProcess(info.hProcess, 0);
                CloseHandle(info.hProcess);
            }
        }
        Game(const Game&) = delete;
        Game& operator=(const Game&) = delete;

        bool Alive() const { return info.hProcess && WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT; }
    };

    BOOL CALLBACK FindPrompt(HWND hwnd, LPARAM param)
    {
        wchar_t name[64]{};
        GetClassNameW(hwnd, name, 64);
        if (wcscmp(name, L"DumpDiverFreezePrompt") != 0)
            return TRUE;
        *reinterpret_cast<HWND*>(param) = hwnd;
        return FALSE;
    }

    HWND WaitForPrompt(int timeoutMs)
    {
        for (int waited = 0;; waited += 50) {
            HWND found = nullptr;
            EnumDesktopWindows(g_desktop, &FindPrompt, reinterpret_cast<LPARAM>(&found));
            if (found)
                return found;
            if (waited >= timeoutMs)
                return nullptr;
            Sleep(50);
        }
    }

    BOOL CALLBACK FindDefaultButton(HWND hwnd, LPARAM param)
    {
        if ((GetWindowLongW(hwnd, GWL_STYLE) & BS_TYPEMASK) != BS_DEFPUSHBUTTON)
            return TRUE;
        *reinterpret_cast<HWND*>(param) = hwnd;
        return FALSE;
    }

    std::wstring Text(HWND hwnd)
    {
        wchar_t text[128]{};
        GetWindowTextW(hwnd, text, 128);
        return text;
    }

    // The prompt on a thread of its own on the test desktop.
    class PromptRun
    {
    public:
        PromptRun(DWORD pid, const fs::path& report, StillStuck stillStuck)
        {
            m_thread = std::thread([this, pid, report, stillStuck = std::move(stillStuck)] {
                m_threadId = GetCurrentThreadId();
                if (SetThreadDesktop(g_desktop))
                    RunFreezePrompt(pid, report, fs::path(), stillStuck);
                m_done = true;
            });
        }
        ~PromptRun()
        {
            // A prompt that did not close as it should is closed here, so the next test starts clean.
            if (!m_done) {
                if (HWND window = WaitForPrompt(0))
                    PostMessageW(window, WM_CLOSE, 0, 0);
                WaitDone(3000);
            }
            if (m_done)
                m_thread.join();
            else
                m_thread.detach();
        }
        PromptRun(const PromptRun&) = delete;
        PromptRun& operator=(const PromptRun&) = delete;

        bool WaitDone(int timeoutMs) const
        {
            for (int waited = 0; waited < timeoutMs && !m_done; waited += 20)
                Sleep(20);
            return m_done;
        }
        bool  Done() const { return m_done; }
        DWORD ThreadId() const { return m_threadId; }

    private:
        std::thread        m_thread;
        std::atomic<bool>  m_done{ false };
        std::atomic<DWORD> m_threadId{ 0 };
    };

    void PressKey(HWND window, WPARAM key)
    {
        PostMessageW(window, WM_KEYDOWN, key, 0);
        PostMessageW(window, WM_KEYUP, key, 0xC0000000);
    }

    void RunTests(const fs::path& report)
    {
        {
            Game              game(60000);
            const Clock::time_point start = Clock::now();
            PromptRun         run(game.info.dwProcessId, report, [start] { return Clock::now() - start < std::chrono::milliseconds(800); });
            Check(WaitForPrompt(5000) != nullptr, "the prompt opens while the game is stuck");
            Check(run.WaitDone(5000), "the prompt closes by itself once the game is back");
            Check(game.Alive(), "closing it by itself leaves the game running");
        }
        {
            Game      game(60000);
            PromptRun run(game.info.dwProcessId, report, [] { return true; });
            HWND      window = WaitForPrompt(5000);
            Check(window != nullptr, "the prompt opens");
            HWND defaultButton = nullptr;
            if (window)
                EnumChildWindows(window, &FindDefaultButton, reinterpret_cast<LPARAM>(&defaultButton));
            Check(defaultButton && Text(defaultButton) == L"Keep waiting", "Keep waiting is the default button");
            GUITHREADINFO gui{ sizeof(gui) };
            const bool    focus = GetGUIThreadInfo(run.ThreadId(), &gui) && gui.hwndFocus && gui.hwndFocus == defaultButton;
            Check(focus, "Keep waiting has the keyboard focus");
            Sleep(600);  // a few recovery checks go by
            Check(window && IsWindow(window) && !run.Done(), "the prompt stays up while the game is still stuck");
            PressKey(gui.hwndFocus ? gui.hwndFocus : window, VK_RETURN);
            Check(run.WaitDone(3000), "Enter closes the prompt");
            Check(game.Alive(), "Enter does not quit the game");
        }
        {
            Game      game(60000);
            PromptRun run(game.info.dwProcessId, report, [] { return true; });
            HWND      window = WaitForPrompt(5000);
            GUITHREADINFO gui{ sizeof(gui) };
            GetGUIThreadInfo(run.ThreadId(), &gui);
            PressKey(gui.hwndFocus ? gui.hwndFocus : window, VK_ESCAPE);
            Check(window && run.WaitDone(3000), "Esc closes the prompt");
            Check(game.Alive(), "Esc does not quit the game");
        }
        {
            Game      game(1500);
            PromptRun run(game.info.dwProcessId, report, [] { return true; });
            Check(WaitForPrompt(5000) != nullptr, "the prompt opens for a game that is about to exit");
            Check(run.WaitDone(8000), "the prompt closes when the game exits");
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && wcscmp(argv[1], L"--sleep") == 0) {
        Sleep(wcstoul(argv[2], nullptr, 10));
        return 0;
    }

    const std::wstring name = L"DumpDiverPromptTest" + std::to_wstring(GetCurrentProcessId());
    g_desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!g_desktop) {
        std::printf("FAIL no test desktop (error %lu)\n", GetLastError());
        return 1;
    }
    // A report that does not exist: a wrong Quit would only fail to open it, never start a program.
    const fs::path report = fs::temp_directory_path() / L"dd-prompt-test-no-such-report.txt";

    // The checks run on the test desktop too, where they can see the prompt's focus.
    std::thread checks([&report] {
        if (!SetThreadDesktop(g_desktop)) {
            std::printf("FAIL could not join the test desktop (error %lu)\n", GetLastError());
            ++g_failures;
            return;
        }
        RunTests(report);
    });
    checks.join();
    CloseDesktop(g_desktop);

    if (g_failures)
        std::printf("%d prompt check(s) failed\n", g_failures);
    else
        std::printf("all prompt checks passed\n");
    return g_failures ? 1 : 0;
}
