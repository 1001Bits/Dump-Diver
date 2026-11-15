// End-to-end scenarios: runs the stand-in game with the real plugin and helper on a private
// desktop, makes it freeze in a known way and checks the report. Opt-in; it starts processes.
//
//   dd_scenario_tests <DumpDiverTestHost.exe> <DumpDiver.dll> <DumpDiverHelper.exe>
//                     <DDTestCulprit.dll> <DDTestDecoy.dll> [scenario]

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    struct Expectation
    {
        const wchar_t* scenario;
        const char*    kind;
        const char*    suspect;        // nullptr: no suspect expected
        const char*    minConfidence;  // "Low", "Medium", "High"
        const char*    record = nullptr;  // text expected under "records"
        bool           noReport = false;  // nothing may be captured
        const char*    mainStackEnd = nullptr;  // text the main thread's stack must reach (a full walk)
    };

    const Expectation kScenarios[] = {
        { L"busy-plugin", "main_thread_loop_in_plugin", "DDTestCulprit.dll", "High" },
        { L"task-engine-loop", "main_thread_loop_in_game", "DDTestCulprit.dll", "Medium" },
        { L"worker-busy", "engine_wait", "DDTestCulprit.dll", "Medium" },
        { L"engine-lock", "engine_lock_wait", "DDTestCulprit.dll", "Medium" },
        { L"lock-left-held", "lock_left_held", "DDTestCulprit.dll", "High" },
        { L"deadlock", "deadlock", "DDTestCulprit.dll", "High" },
        { L"dialog", "hidden_dialog", "DDTestCulprit.dll", "High" },
        { L"gpu-decoy", "gpu_wait", nullptr, nullptr },
        { L"loading-stall", "endless_loading", "DDTestCulprit.dll", "Medium" },
        { L"loading-normal", nullptr, nullptr, nullptr, nullptr, true },
        { L"engine-loop-record", "main_thread_loop_in_game", nullptr, nullptr, "DDTestRecords.esp" },
        { L"plugin-loop-engine", "main_thread_loop_in_plugin", "DDTestCulprit.dll", "Medium", nullptr, false,
          "RtlUserThreadStart" },
    };

    int Rank(const std::string& confidence)
    {
        return confidence == "High" ? 3 : confidence == "Medium" ? 2 : confidence == "Low" ? 1 : 0;
    }

    std::string ValueAfter(const std::string& json, const std::string& key, std::size_t from = 0)
    {
        const std::size_t at = json.find("\"" + key + "\": \"", from);
        if (at == std::string::npos)
            return {};
        const std::size_t start = at + key.size() + 5;
        const std::size_t end = json.find('"', start);
        return json.substr(start, end - start);
    }

    std::string ReadFile(const fs::path& file)
    {
        std::ifstream      in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    fs::path WaitForReport(const fs::path& dir, std::chrono::seconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end) {
            std::error_code ec;
            for (const auto& item : fs::directory_iterator(dir, ec)) {
                if (item.path().extension() == L".json")
                    return item.path();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return {};
    }

    bool RunScenario(const Expectation& e, const fs::path& host, const fs::path& plugin, const fs::path& helper,
                     const fs::path& culprit, const fs::path& decoy, const wchar_t* desktop)
    {
        wchar_t tempBuffer[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempBuffer);
        const fs::path root = fs::path(tempBuffer) / L"DumpDiverScenarios" / e.scenario;
        std::error_code ec;
        fs::remove_all(root, ec);
        const fs::path plugins = root / L"Data" / L"SKSE" / L"Plugins";
        fs::create_directories(plugins / L"DumpDiver");
        fs::create_directories(root / L"out");
        fs::create_directories(root / L"logs");
        fs::copy_file(host, root / L"DumpDiverTestHost.exe");
        fs::copy_file(plugin, plugins / L"DumpDiver.dll");
        fs::copy_file(helper, plugins / L"DumpDiver" / L"DumpDiverHelper.exe");
        fs::copy_file(culprit, plugins / L"DDTestCulprit.dll");
        std::error_code pdbError;
        fs::copy_file(fs::path(culprit).replace_extension(L".pdb"), plugins / L"DDTestCulprit.pdb", pdbError);
        fs::copy_file(decoy, plugins / L"DDTestDecoy.dll");
        {
            std::wofstream ini(plugins / L"DumpDiver.ini");
            ini << L"[General]\nOutputDir=" << (root / L"out").wstring() << L"\n"
                << L"[Freeze]\nFreezeSeconds=3\nLoadingScreenSeconds=4\nSampleMilliseconds=1500\nSampleHz=40\nWriteDump=0\n"
                << L"ManualHotkey=none\n";
        }

        SetEnvironmentVariableW(L"DUMPDIVER_LOGDIR", (root / L"logs").c_str());
        std::wstring        command = L"\"" + (root / L"DumpDiverTestHost.exe").wstring() + L"\" " + e.scenario;
        STARTUPINFOW        startup{ sizeof(startup) };
        startup.lpDesktop = const_cast<wchar_t*>(desktop);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(),
                            &startup, &process)) {
            wprintf(L"[%s] FAIL: cannot start the host (error %lu)\n", e.scenario, GetLastError());
            return false;
        }

        const fs::path report = WaitForReport(root / L"out", std::chrono::seconds(e.noReport ? 14 : 45));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the helper finish writing
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
        DWORD exitCode = 0;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (e.noReport) {
            const std::string log = ReadFile(root / L"logs" / L"DumpDiver.log");
            const bool        watched = log.find("watching loading screens") != std::string::npos;
            const bool        ok = report.empty() && watched;
            wprintf(L"[%s] %s: %s, loading screens %s\n", e.scenario, ok ? L"pass" : L"FAIL",
                    report.empty() ? L"no report" : L"unexpected report", watched ? L"watched" : L"NOT watched");
            return ok;
        }
        if (report.empty()) {
            wprintf(L"[%s] FAIL: no report (see %s)\n", e.scenario, root.c_str());
            return false;
        }
        const std::string json = ReadFile(report);
        const std::string kind = ValueAfter(json, "kind");
        const std::size_t suspects = json.find("\"suspects\": [");
        const std::size_t suspectsEnd = json.find("\n    ],", suspects);
        const std::string suspectBlock = suspects == std::string::npos ? std::string() : json.substr(suspects, suspectsEnd - suspects);
        const std::string first = ValueAfter(suspectBlock, "module");
        const std::string confidence = ValueAfter(suspectBlock, "confidence");

        const std::size_t records = json.find("\"records\": [");
        const std::string recordBlock = records == std::string::npos ? std::string() : json.substr(records, json.find("\n    ],", records) - records);

        bool ok = kind == e.kind;
        if (e.suspect)
            ok = ok && first == e.suspect && Rank(confidence) >= Rank(e.minConfidence);
        else
            ok = ok && first.empty();
        if (e.record)
            ok = ok && recordBlock.find(e.record) != std::string::npos;
        if (suspectBlock.find("DDTestDecoy") != std::string::npos)
            ok = false;  // the decoy must never be a suspect
        bool stackOk = true;
        if (e.mainStackEnd) {
            const std::string mainTid = json.substr(json.find("\"main_thread\": ") + 15, 12);
            const std::size_t entry = json.find("{\"tid\": " + std::to_string(std::stoul(mainTid)) + ",");
            const std::size_t stack = entry == std::string::npos ? entry : json.find("\"stack\": [", entry);
            const std::string frames = stack == std::string::npos ? std::string() : json.substr(stack, json.find("]}", stack) - stack);
            stackOk = frames.find(e.mainStackEnd) != std::string::npos;
            ok = ok && stackOk;
        }

        wprintf(L"[%s] %s: kind=%S suspect=%S (%S)%S\n", e.scenario, ok ? L"pass" : L"FAIL", kind.c_str(),
                first.empty() ? "none" : first.c_str(), confidence.c_str(),
                e.record ? (recordBlock.find(e.record) != std::string::npos ? " record found" : " RECORD MISSING") : "");
        if (!stackOk)
            wprintf(L"    the main thread's stack stops early (no %S)\n", e.mainStackEnd);
        if (!ok)
            wprintf(L"    report: %s\n", report.c_str());
        return ok;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 6) {
        fwprintf(stderr, L"usage: dd_scenario_tests <host> <plugin> <helper> <culprit> <decoy> [scenario]\n");
        return 2;
    }
    const wchar_t* only = argc > 6 ? argv[6] : nullptr;

    // A private desktop keeps the test windows (and the message box) off the user's screen.
    const wchar_t* desktopName = L"DumpDiverTests";
    HDESK          desktop = CreateDesktopW(desktopName, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) {
        fwprintf(stderr, L"cannot create the test desktop (error %lu)\n", GetLastError());
        return 3;
    }

    int failed = 0;
    for (const auto& e : kScenarios) {
        if (only && wcscmp(only, e.scenario) != 0)
            continue;
        if (!RunScenario(e, argv[1], argv[2], argv[3], argv[4], argv[5], desktopName))
            ++failed;
    }
    CloseDesktop(desktop);
    wprintf(L"%d scenario(s) failed\n", failed);
    return failed ? 1 : 0;
}
