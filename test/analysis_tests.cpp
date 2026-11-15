// Unit tests of the analysis on hand-made snapshots. Starts no processes.

#include "helper/Analyze.h"
#include "helper/SnapshotFile.h"
#include "helper/Capture.h"
#include "helper/Records.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace dd;
using namespace dd::helper;

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
    TESFileArray* files = nullptr;
    std::uint32_t formFlags = 0;
    std::uint32_t formID = 0;
    std::uint16_t inGameFlags = 0;
    std::uint8_t  formType = 0;
    std::uint8_t  pad1B = 0;
    std::uint32_t pad1C = 0;
};

class TESNPC final : public TESForm
{};

class TESObjectREFR : public TESForm
{
public:
    std::uint8_t pad20[0x20]{};
    TESForm*     baseObject = nullptr;
    std::uint8_t pad48[0x18]{};
    TESForm*     parentCell = nullptr;
};

class Character final : public TESObjectREFR
{};

class NotAForm
{
public:
    virtual ~NotAForm() = default;
    std::uint64_t value = 0x14;
};

namespace
{
    int g_failed = 0;

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            ++g_failed;                                                               \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
        }                                                                             \
    } while (0)

    constexpr std::uint64_t kGame = 0x140000000;
    constexpr std::uint64_t kNtdll = 0x7FF800000000;
    constexpr std::uint64_t kKernelBase = 0x7FF810000000;
    constexpr std::uint64_t kUser32 = 0x7FF820000000;
    constexpr std::uint64_t kDriver = 0x7FF830000000;
    constexpr std::uint64_t kCulprit = 0x180000000;
    constexpr std::uint64_t kDecoy = 0x190000000;
    constexpr std::uint64_t kExtender = 0x1A0000000;

    constexpr std::uint64_t kNtDelay = kNtdll + 0x1014;
    constexpr std::uint64_t kNtWait = kNtdll + 0x2014;
    constexpr std::uint64_t kSleepEx = kKernelBase + 0x3000;
    constexpr std::uint64_t kWaitSingle = kKernelBase + 0x4000;
    constexpr std::uint64_t kUserWait = kUser32 + 0x5014;
    constexpr std::uint64_t kMessageBox = kUser32 + 0x6000;
    constexpr std::uint64_t kEnterCs = kNtdll + 0x7000;
    constexpr std::uint64_t kThreadStart = kKernelBase + 0x8000;
    constexpr std::uint64_t kNtRead = kNtdll + 0x9014;
    constexpr std::uint64_t kReadFile = kKernelBase + 0xA000;

    Module MakeModule(const wchar_t* name, std::uint64_t base, ModuleKind kind)
    {
        Module m;
        m.name = name;
        m.path = name;
        m.realPath = name;
        m.base = base;
        m.size = 0x1000000;
        m.kind = kind;
        return m;
    }

    Snapshot BaseSnapshot()
    {
        Snapshot s;
        s.game = Game::kSkyrim;
        s.processId = 100;
        s.mainThreadId = 4;
        s.qpcFrequency = 10000000;
        s.captureQpc = 200000000;
        s.heartbeatQpc = 100000000;
        s.heartbeatAgeSeconds = 10;
        s.sampleSeconds = 3;
        s.taskTracking = true;
        s.modules = {
            MakeModule(L"SkyrimSE.exe", kGame, ModuleKind::kGame),
            MakeModule(L"ntdll.dll", kNtdll, ModuleKind::kSystem),
            MakeModule(L"KERNELBASE.dll", kKernelBase, ModuleKind::kSystem),
            MakeModule(L"user32.dll", kUser32, ModuleKind::kSystem),
            MakeModule(L"nvwgf2umx.dll", kDriver, ModuleKind::kGraphics),
            MakeModule(L"Culprit.dll", kCulprit, ModuleKind::kPlugin),
            MakeModule(L"Decoy.dll", kDecoy, ModuleKind::kPlugin),
            MakeModule(L"skse64_1_5_97.dll", kExtender, ModuleKind::kScriptExtender),
        };
        s.symbols[kNtDelay] = L"ntdll.dll!NtDelayExecution+0x14";
        s.symbols[kNtWait] = L"ntdll.dll!NtWaitForSingleObject+0x14";
        s.symbols[kSleepEx] = L"KERNELBASE.dll!SleepEx+0x9a";
        s.symbols[kNtRead] = L"ntdll.dll!NtReadFile+0x14";
        s.symbols[kReadFile] = L"KERNELBASE.dll!ReadFile+0x8d";
        s.symbols[kWaitSingle] = L"KERNELBASE.dll!WaitForSingleObjectEx+0x8e";
        s.symbols[kUserWait] = L"user32.dll!NtUserWaitMessage+0x14";
        s.symbols[kMessageBox] = L"user32.dll!MessageBoxW+0x40";
        s.symbols[kEnterCs] = L"ntdll.dll!RtlEnterCriticalSection+0x30";
        return s;
    }

    ThreadData MakeThread(std::uint32_t tid, std::vector<std::uint64_t> frames, double cpuShare, std::uint64_t start = kThreadStart)
    {
        ThreadData t;
        t.tid = tid;
        t.startAddress = start;
        t.cpuTime100ns = static_cast<std::uint64_t>(cpuShare * 3 * 1e7);
        t.contextSwitches = cpuShare > 0.1 ? 30 : 0;
        for (int i = 0; i < 40; ++i)
            t.samples.push_back({ frames });
        return t;
    }

    // A running thread whose top frame changes from sample to sample.
    ThreadData MakeBusyThread(std::uint32_t tid, std::uint64_t base, std::vector<std::uint64_t> below, std::uint64_t start = kThreadStart)
    {
        ThreadData t = MakeThread(tid, {}, 0.95, start);
        t.samples.clear();
        for (int i = 0; i < 40; ++i) {
            std::vector<std::uint64_t> frames{ base + 0x100 + static_cast<std::uint64_t>(i % 7) * 0x10 };
            frames.insert(frames.end(), below.begin(), below.end());
            t.samples.push_back({ frames });
        }
        return t;
    }

    // Six idle decoy workers waiting in their own code.
    void AddDecoyPool(Snapshot& s)
    {
        for (std::uint32_t i = 0; i < 6; ++i)
            s.threads.push_back(MakeThread(100 + i * 4, { kNtWait, kWaitSingle, kDecoy + 0x500 }, 0, kDecoy + 0x400));
    }

    bool HasSuspect(const Verdict& v, const Snapshot& s, const wchar_t* name)
    {
        for (const auto& suspect : v.suspects) {
            if (s.modules[static_cast<std::size_t>(suspect.module)].name == name)
                return true;
        }
        return false;
    }

    const wchar_t* Top(const Verdict& v, const Snapshot& s)
    {
        return v.suspects.empty() ? L"" : s.modules[static_cast<std::size_t>(v.suspects.front().module)].name.c_str();
    }

    void MainBusyInPlugin()
    {
        std::printf("main thread busy in a plugin\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeBusyThread(4, kCulprit, { kDecoy + 0x100, kGame + 0x2000 }));
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"main_thread_loop_in_plugin");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));
    }

    void EngineWaitWithHookOnPathBlamesNobody()
    {
        std::printf("engine wait with a hook below it blames nobody\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kNtWait, kWaitSingle, kGame + 0x3000, kDecoy + 0x100, kGame + 0x2000 }, 0));
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"engine_wait");
        CHECK(v.suspects.empty());
    }

    void EngineWaitWithBusyWorker()
    {
        std::printf("engine wait while a game thread is busy in a plugin\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kNtWait, kWaitSingle, kGame + 0x3000, kDecoy + 0x100 }, 0));
        s.threads.push_back(MakeBusyThread(8, kCulprit, { kGame + 0x4000 }, kGame + 0x9000));
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"engine_wait");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kMedium);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));
    }

    void BusyWorkerRunsOwnTask()
    {
        std::printf("a game thread runs a plugin's own unfinished task\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kNtWait, kWaitSingle, kGame + 0x3000 }, 0));
        s.threads.push_back(MakeBusyThread(8, kCulprit, { kCulprit + 0x900, kGame + 0x4000 }, kGame + 0x9000));
        TaskInFlight task;
        task.threadId = 8;
        task.startQpc = 100050000;  // 5 ms after the last heartbeat: the task that stopped it
        task.runAddress = kCulprit + 0x900;
        task.callerAddress = kCulprit + 0xA00;
        s.tasks.push_back(task);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"engine_wait");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);
        CHECK(!v.suspects.empty() && v.suspects.front().reasons.size() == 2);
    }

    void PluginLoopAroundEngine()
    {
        std::printf("a plugin loops around game functions (most samples in game code)\n");
        Snapshot   s = BaseSnapshot();
        ThreadData t = MakeThread(4, {}, 0.95);
        t.samples.clear();
        for (int i = 0; i < 100; ++i) {
            const std::uint64_t alloc = kGame + 0x5000 + static_cast<std::uint64_t>(i % 5) * 0x10;
            if (i % 20 == 0)
                t.samples.push_back({ { kCulprit + 0x2280, kGame + 0x2000 } });  // back in its own code
            else
                t.samples.push_back({ { alloc, kGame + 0x6000, kCulprit + 0x228C, kGame + 0x2000 } });
        }
        s.threads.push_back(t);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"main_thread_loop_in_plugin");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);
    }

    void StuckInExtenderCallback()
    {
        std::printf("a loader thread stuck inside a plugin's SKSE callback, main thread spin-waiting\n");
        Snapshot s = BaseSnapshot();
        s.reason = CaptureReason::kStartup;
        s.heartbeatQpc = 0;
        ThreadData main = MakeThread(4, { kNtDelay, kSleepEx, kGame + 0x3000, kGame + 0x2000 }, 0.9);
        main.contextSwitches = 3000;
        s.threads.push_back(main);
        ThreadData loader = MakeThread(8, {}, 0.95, kGame + 0x9000);
        loader.samples.clear();
        for (int i = 0; i < 100; ++i) {
            const std::uint64_t alloc = kGame + 0x5000 + static_cast<std::uint64_t>(i % 5) * 0x10;
            loader.samples.push_back({ { alloc, kGame + 0x6000, kCulprit + 0x228C, kExtender + 0x100, kGame + 0x7000 } });
        }
        s.threads.push_back(loader);
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"engine_wait");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence >= Confidence::kMedium);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));
    }

    void SpinWaitBetweenSleeps()
    {
        std::printf("the main thread spin-waits with Sleep but is caught between sleeps in half the samples\n");
        Snapshot s = BaseSnapshot();
        s.reason = CaptureReason::kStartup;
        s.heartbeatQpc = 0;
        ThreadData main = MakeThread(4, {}, 0.97);
        main.samples.clear();
        for (int i = 0; i < 100; ++i) {
            if (i % 2)
                main.samples.push_back({ { kNtDelay, kSleepEx, kGame + 0x3000, kGame + 0x2000 } });
            else
                main.samples.push_back({ { kGame + 0x3010 + static_cast<std::uint64_t>(i % 3) * 8, kGame + 0x2000 } });
        }
        main.contextSwitches = 6000;  // 2000 a second
        s.threads.push_back(main);
        ThreadData loader = MakeThread(8, {}, 0.95, kGame + 0x9000);
        loader.samples.clear();
        for (int i = 0; i < 100; ++i) {
            const std::uint64_t alloc = kGame + 0x5000 + static_cast<std::uint64_t>(i % 5) * 0x10;
            loader.samples.push_back({ { alloc, kGame + 0x6000, kCulprit + 0x228C, kExtender + 0x100, kGame + 0x7000 } });
        }
        FormInfo faction;
        faction.formId = 0x05000800;
        faction.className = L"TESFaction";
        faction.files = { L"DDTestFaction.esp" };
        s.forms = { faction };
        loader.forms = { { 0, 100, 0 } };  // in a register in every sample
        s.threads.push_back(loader);
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"engine_wait");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));
        CHECK(!v.records.empty() && v.records.front().form == 0);  // the record the stuck callback works on
        CHECK(!v.advice.empty() && v.advice.front().starts_with(L"Disable Culprit.dll"));

        // The same thread with few context switches is a real loop in the game, not a spin-wait.
        Snapshot busy = s;
        busy.threads[0].contextSwitches = 60;
        CHECK(Analyze(busy).kind == L"main_thread_loop_in_game");
    }

    void JobThreadLoopsInPlugin()
    {
        std::printf("the main thread spin-waits for a game job thread that loops in a plugin (Splashes of Skyrim 1.0)\n");
        Snapshot s = BaseSnapshot();
        ThreadData main = MakeThread(4, { kNtDelay, kSleepEx, kGame + 0x3000, kGame + 0x2000 }, 0.96);
        main.contextSwitches = 2300;
        s.threads.push_back(main);
        s.threads.push_back(MakeBusyThread(8, kCulprit, { kCulprit + 0x1339, kGame + 0x6000 }, kGame + 0x9000));
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"engine_wait");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));

        // A second busy thread in another DLL's code: no single thread to tie the wait to.
        Snapshot two = s;
        two.threads.push_back(MakeBusyThread(12, kDecoy, { kGame + 0x6000 }, kGame + 0x9000));
        const Verdict w = Analyze(two);
        CHECK(!w.suspects.empty() && w.suspects.front().confidence == Confidence::kMedium);
    }

    void CosaveReadLoop()
    {
        std::printf("a plugin's cosave load loops over file reads that return at once (Papyrus Extender 6.2.1)\n");
        Snapshot s = BaseSnapshot();
        s.stateFlags |= kState_LoadingSave;
        ThreadData main = MakeThread(4, { kNtRead, kReadFile, kExtender + 0x200, kCulprit + 0x13F, kExtender + 0x300,
                                          kGame + 0x7000, kGame + 0x2000 }, 0.94);
        main.contextSwitches = 2400;
        s.threads.push_back(main);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"main_thread_loop_in_plugin");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);

        // The same stack with the thread idle is a read the disk does not answer.
        Snapshot blocked = s;
        blocked.threads[0].cpuTime100ns = static_cast<std::uint64_t>(0.01 * 3 * 1e7);
        blocked.threads[0].contextSwitches = 20;
        CHECK(Analyze(blocked).kind == L"file_io_wait");
    }

    void SnapshotRoundTrip()
    {
        std::printf("a capture saved to .ddsnap and read back gives the same verdict\n");
        Snapshot s = BaseSnapshot();
        ThreadData main = MakeThread(4, { kNtDelay, kSleepEx, kGame + 0x3000, kGame + 0x2000 }, 0.96);
        main.contextSwitches = 2300;
        main.stackTop = { 1, 2, 3, 4 };
        s.threads.push_back(main);
        s.threads.push_back(MakeBusyThread(8, kCulprit, { kCulprit + 0x1339, kGame + 0x6000 }, kGame + 0x9000));
        FormInfo world;
        world.formId = 0x3C;
        world.className = L"TESWorldSpace";
        world.files = { L"Skyrim.esm" };
        s.forms = { world };
        s.threads[1].forms = { { 0, 40, 0 } };
        s.chains.push_back({ 4, false, { { WctObject::kThread, 100, 4, 10, 0, 0, L"x" } } });
        s.gpuPollSites[kGame + 0x10] = true;
        AddDecoyPool(s);
        const std::filesystem::path file = std::filesystem::temp_directory_path() / L"dd_roundtrip.ddsnap";
        CHECK(SaveSnapshot(s, file));
        Snapshot back;
        CHECK(LoadSnapshot(file, back));
        std::error_code ec;
        std::filesystem::remove(file, ec);
        const Verdict a = Analyze(s);
        const Verdict b = Analyze(back);
        CHECK(a.kind == b.kind && a.suspects.size() == b.suspects.size() && a.records.size() == b.records.size());
        CHECK(!b.suspects.empty() && b.suspects.front().module == a.suspects.front().module &&
              b.suspects.front().confidence == a.suspects.front().confidence);
        CHECK(back.symbols == s.symbols && back.threads.size() == s.threads.size() && back.threads[0].stackTop == s.threads[0].stackTop);
    }

    void BrokenRecordNamesPatcher()
    {
        std::printf("an engine loop on an NPC whose face NPC is itself names the runtime patcher (SkyPatcher copyVisualStyle)\n");
        Snapshot s = BaseSnapshot();
        constexpr std::uint64_t kPatcher = 0x1B0000000;
        s.modules.push_back(MakeModule(L"SkyPatcher.dll", kPatcher, ModuleKind::kPlugin));
        s.modules.push_back(MakeModule(L"ModTestProbe.dll", 0x1C0000000, ModuleKind::kPlugin));
        FormInfo npc;
        npc.formId = 0x13479;
        npc.className = L"TESNPC";
        npc.files = { L"Skyrim.esm" };
        npc.problem = L"its face NPC is itself, so loading its face never ends";
        s.forms = { npc };
        ThreadData main = MakeBusyThread(4, kGame, { kGame + 0x3000, 0x1C0000100, kGame + 0x2000 });
        main.forms = { { 0, 40, 0 } };
        s.threads.push_back(main);
        s.tasks.push_back({ 4, 1, 0x1C0000200, 0x1C0000300, 1, 0 });  // the lab probe's task started the cell load
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"main_thread_loop_in_game");
        CHECK(std::wstring(Top(v, s)) == L"SkyPatcher.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kMedium);
        CHECK(!HasSuspect(v, s, L"ModTestProbe.dll"));
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));

        // A distributor that cannot set face templates stays out, and SkyPatcher keeps Medium.
        Snapshot spid = s;
        spid.modules.push_back(MakeModule(L"po3_SpellPerkItemDistributor.dll", 0x1D0000000, ModuleKind::kPlugin));
        const Verdict u = Analyze(spid);
        CHECK(std::wstring(Top(u, spid)) == L"SkyPatcher.dll");
        CHECK(!u.suspects.empty() && u.suspects.front().confidence == Confidence::kMedium);
        CHECK(!HasSuspect(u, spid, L"po3_SpellPerkItemDistributor.dll"));

        // Two patchers that can both set face templates: both are leads, neither more than Low.
        Snapshot two = s;
        two.modules.push_back(MakeModule(L"SkyPatcherVR.dll", 0x1D0000000, ModuleKind::kPlugin));
        const Verdict w = Analyze(two);
        CHECK(!w.suspects.empty() && w.suspects.front().confidence == Confidence::kLow);
    }

    void PluginWalkingBrokenRecord()
    {
        std::printf("a hook DLL looping over an NPC whose face NPC is itself names the patcher, the hook only Low (RaceMenu)\n");
        Snapshot s = BaseSnapshot();
        constexpr std::uint64_t kHook = 0x1E0000000;
        s.modules.push_back(MakeModule(L"skee64.dll", kHook, ModuleKind::kPlugin));
        s.modules.push_back(MakeModule(L"SkyPatcher.dll", 0x1B0000000, ModuleKind::kPlugin));
        s.modules.push_back(MakeModule(L"po3_KeywordItemDistributor.dll", 0x1D0000000, ModuleKind::kPlugin));
        FormInfo npc;
        npc.formId = 0x13479;
        npc.className = L"TESNPC";
        npc.files = { L"Skyrim.esm" };
        npc.problem = L"its face NPC is itself, so loading its face never ends";
        s.forms = { npc };
        ThreadData main = MakeBusyThread(4, kHook + 0x400, { kHook + 0x300, kGame + 0x2800, kGame + 0x2000 });
        main.forms = { { 0, 40, 0 } };
        s.threads.push_back(main);
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(std::wstring(Top(v, s)) == L"SkyPatcher.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kMedium);
        for (const auto& suspect : v.suspects) {
            if (s.modules[static_cast<std::size_t>(suspect.module)].name == L"skee64.dll")
                CHECK(suspect.confidence == Confidence::kLow);
        }
        CHECK(!HasSuspect(v, s, L"po3_KeywordItemDistributor.dll"));

        // The same loop without a broken record still blames the DLL.
        Snapshot clean = s;
        clean.forms[0].problem.clear();
        const Verdict c = Analyze(clean);
        CHECK(std::wstring(Top(c, clean)) == L"skee64.dll");
        CHECK(!c.suspects.empty() && c.suspects.front().confidence == Confidence::kHigh);
    }

    void SelfDeadlockOnReentry()
    {
        std::printf("a thread re-enters a DLL and waits for a lock there: a self-deadlock (Actor Value Generator 2.2.12)\n");
        Snapshot s = BaseSnapshot();
        constexpr std::uint64_t kAlertWait = kNtdll + 0xA014;
        constexpr std::uint64_t kAcquireSrw = kNtdll + 0xB04D;
        s.symbols[kAlertWait] = L"ntdll.dll!ZwWaitForAlertByThreadId+0x14";
        s.symbols[kAcquireSrw] = L"ntdll.dll!RtlAcquireSRWLockExclusive+0x4d";
        ThreadData main = MakeThread(4, { kAlertWait, kAcquireSrw, kCulprit + 0x100, kDecoy + 0x200, kCulprit + 0x300, kGame + 0x2000 }, 0.0);
        s.threads.push_back(main);
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"main_waits_in_plugin");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));

        // No second entry, and no other thread anywhere in the DLL (Mu Facial Expression Extended 0.4.6): the
        // lock can only be held by this thread's own outer call, or it leaked.
        Snapshot alone = s;
        alone.threads[0].samples.assign(40, Sample{ { kAlertWait, kAcquireSrw, kCulprit + 0x100, kGame + 0x2000 } });
        const Verdict a = Analyze(alone);
        CHECK(!a.suspects.empty() && a.suspects.front().confidence == Confidence::kHigh);

        // The same wait while another thread is in the DLL's code: that thread may hold the lock.
        Snapshot shared = alone;
        shared.threads.push_back(MakeThread(9, { kNtWait, kCulprit + 0x500, kThreadStart }, 0.0, kCulprit + 0x10));
        const Verdict w = Analyze(shared);
        CHECK(!w.suspects.empty() && w.suspects.front().confidence == Confidence::kMedium);
    }

    void Deadlock()
    {
        std::printf("deadlock reported by Windows\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kNtWait, kEnterCs, kCulprit + 0x200, kGame + 0x3000 }, 0));
        s.threads.push_back(MakeThread(8, { kNtWait, kEnterCs, kGame + 0x5000, kCulprit + 0x300 }, 0, kCulprit + 0x380));
        WaitChain chain;
        chain.threadId = 4;
        chain.cycle = true;
        WctNode t4;
        t4.type = WctObject::kThread;
        t4.processId = 100;
        t4.threadId = 4;
        WctNode cs;
        cs.type = WctObject::kCriticalSection;
        WctNode t8 = t4;
        t8.threadId = 8;
        chain.nodes = { t4, cs, t8, cs, t4 };
        s.chains.push_back(chain);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"deadlock");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);
    }

    void HiddenDialog()
    {
        std::printf("message box behind the game\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kUserWait, kMessageBox, kCulprit + 0x600, kGame + 0x3000 }, 0));
        s.dialogs.push_back({ 4, L"Culprit", L"needs version 2", L"#32770", true });
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"hidden_dialog");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);
    }

    void EngineLockOwnedByPluginThread()
    {
        std::printf("main thread retries an engine lock held by a thread inside a plugin\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kNtDelay, kSleepEx, kGame + 0x3000, kDecoy + 0x100 }, 0.01));
        s.threads.push_back(MakeThread(8, { kNtDelay, kSleepEx, kCulprit + 0x700, kGame + 0x6000 }, 0, kGame + 0x9000));
        AddDecoyPool(s);
        LockHit lock;
        lock.waiterTid = 4;
        lock.ownerTid = 8;
        lock.count = 1;
        lock.lockAddress = kGame + 0x2F3B8E8;
        lock.inGameData = true;
        lock.foundIn = L"RBX";
        s.locks.push_back(lock);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"engine_lock_wait");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence >= Confidence::kMedium);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));
    }

    // The main thread spins on a game lock whose owner thread has exited (Dynamic Gamma 1.0.9: unlocked in the
    // wrong order on the startup thread, which then exits).
    Snapshot LeftHeldLockSnapshot()
    {
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kNtDelay, kSleepEx, kGame + 0x3000, kDecoy + 0x100 }, 0.01));
        AddDecoyPool(s);
        LockHit lock;
        lock.waiterTid = 4;
        lock.ownerTid = 0x2A10;  // no such thread
        lock.count = 0;
        lock.readWrite = true;
        lock.broken = true;
        lock.ownerAlive = false;
        lock.lockAddress = kGame + 0x2F3B8E8;
        lock.inGameData = true;
        lock.foundIn = L"RBX";
        s.locks.push_back(lock);
        s.lockWatch = 3;
        return s;
    }

    LockRecordInfo MakeLockRecord(LockRecordKind kind, std::uint64_t code)
    {
        LockRecordInfo r;
        r.kind = kind;
        r.lockAddress = kGame + 0x2F3B8E8;
        r.threadId = 0x2A10;
        r.code = code;
        r.messageType = 8;  // DataLoaded
        r.lockWord = 0x80000000;
        r.current = (0x80000000ull << 32) | 0x2A10;
        r.qpc = 20000000;
        return r;
    }

    void LockLeftHeldByHandler()
    {
        std::printf("main thread waits for a game lock a plugin's message handler left held\n");
        Snapshot s = LeftHeldLockSnapshot();
        s.lockRecords.push_back(MakeLockRecord(LockRecordKind::kHeldAtExit, kGame + 0x5000));
        s.lockRecords.push_back(MakeLockRecord(LockRecordKind::kLeftByHandler, kCulprit + 0x22CE0));
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"lock_left_held");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kHigh);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));
    }

    void LockLeftHeldUnrecorded()
    {
        std::printf("main thread waits for a game lock whose owner exited, nothing recorded: no mod is named\n");
        Snapshot s = LeftHeldLockSnapshot();
        s.lockWatch = 0;
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"lock_left_held");
        CHECK(v.suspects.empty() || v.suspects.front().confidence == Confidence::kLow);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));
    }

    void LockLeftHeldByPluginThread()
    {
        std::printf("a thread a plugin started exited holding the game lock the main thread waits for\n");
        Snapshot s = LeftHeldLockSnapshot();
        s.lockRecords.push_back(MakeLockRecord(LockRecordKind::kHeldAtExit, kCulprit + 0x900));
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"lock_left_held");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kMedium);
    }

    void LockLeftHeldFromRecordOnly()
    {
        std::printf("no lock found near the spinning main thread, but a recorded one is still held\n");
        Snapshot s = LeftHeldLockSnapshot();
        s.locks.clear();
        s.lockRecords.push_back(MakeLockRecord(LockRecordKind::kLeftByHandler, kCulprit + 0x22CE0));
        Verdict v = Analyze(s);
        CHECK(v.kind == L"lock_left_held");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kMedium);
        // released since the record: not the lock the game waits for
        s.lockRecords[0].current = 0;
        v = Analyze(s);
        CHECK(v.kind != L"lock_left_held");
        CHECK(!HasSuspect(v, s, L"Culprit.dll"));
    }

    void LockRecordsRoundTrip()
    {
        std::printf("lock records survive a .ddsnap round trip\n");
        Snapshot s = LeftHeldLockSnapshot();
        s.lockRecords.push_back(MakeLockRecord(LockRecordKind::kLeftByHandler, kCulprit + 0x22CE0));
        const std::filesystem::path file = std::filesystem::temp_directory_path() / L"dd_lockrecords.ddsnap";
        CHECK(SaveSnapshot(s, file));
        Snapshot back;
        CHECK(LoadSnapshot(file, back));
        std::error_code ec;
        std::filesystem::remove(file, ec);
        CHECK(back.lockWatch == 3 && back.lockRecords.size() == 1 && back.lockRecords[0].code == kCulprit + 0x22CE0 &&
              back.lockRecords[0].kind == LockRecordKind::kLeftByHandler && back.lockRecords[0].current == s.lockRecords[0].current);
        CHECK(back.locks.size() == 1 && !back.locks[0].ownerAlive && back.locks[0].broken);
        CHECK(Analyze(back).kind == L"lock_left_held");
    }

    void GpuWait()
    {
        std::printf("waiting in the graphics driver\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kNtWait, kWaitSingle, kDriver + 0x100, kGame + 0x3000, kDecoy + 0x100 }, 0));
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"gpu_wait");
        CHECK(v.suspects.empty());
    }

    void GpuPollInGame()
    {
        std::printf("the game polls a GPU query\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeThread(4, { kNtDelay, kSleepEx, kGame + 0x3000, kDecoy + 0x100 }, 0.01));
        s.gpuPollSites[kGame + 0x3000] = true;
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"gpu_wait");
        CHECK(v.suspects.empty());
    }

    void EngineLoopInTask()
    {
        std::printf("engine loop inside a task a plugin queued\n");
        Snapshot s = BaseSnapshot();
        s.threads.push_back(MakeBusyThread(4, kGame, { kCulprit + 0x800, kGame + 0x2000 }));
        TaskInFlight task;
        task.threadId = 4;
        task.startQpc = 90000000;
        task.runAddress = kCulprit + 0x800;
        task.callerAddress = kCulprit + 0x900;
        s.tasks.push_back(task);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"main_thread_loop_in_game");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kMedium);
    }

    void CodeShapes()
    {
        std::printf("code and lock decoding\n");
        const std::uint8_t poll[] = { 0x48, 0x8B, 0x01, 0xFF, 0x90, 0xE8, 0x00, 0x00, 0x00, 0x85, 0xC0, 0xB9, 0x01, 0x00 };
        CHECK(LooksLikeGpuPoll(poll, sizeof(poll)));
        const std::uint8_t other[] = { 0x48, 0x8B, 0x01, 0xFF, 0x90, 0xE0, 0x00, 0x00, 0x00, 0x85, 0xC0 };
        CHECK(!LooksLikeGpuPoll(other, sizeof(other)));
        // mov rax,[rcx]; mov rdx,[rax+0E8h]; call rdx
        const std::uint8_t viaRegister[] = { 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x90, 0xE8, 0x00, 0x00, 0x00, 0xFF, 0xD2, 0x85, 0xC0 };
        CHECK(LooksLikeGpuPoll(viaRegister, sizeof(viaRegister)));
        // the loaded register is not the one called
        const std::uint8_t otherRegister[] = { 0x48, 0x8B, 0x01, 0x48, 0x8B, 0x90, 0xE8, 0x00, 0x00, 0x00, 0xFF, 0xD0, 0x85, 0xC0 };
        CHECK(!LooksLikeGpuPoll(otherRegister, sizeof(otherRegister)));

        CHECK(Demangle(".?AVTESObjectREFR@@") == L"TESObjectREFR");
        CHECK(Demangle(".?AVInner@Outer@@") == L"Outer::Inner");
        CHECK(IsPluginName("Skyrim.esm") && IsPluginName("My Mod.esp") && IsPluginName("ccBGSSSE001-Fish.ESL"));
        CHECK(!IsPluginName("Skyrim.bsa") && !IsPluginName(".esp") && !IsPluginName("bad\x01name.esp"));

        std::uint32_t owner = 0, count = 0;
        bool          rw = false;
        CHECK(DecodeLock((1ull << 32) | 0x1F2C, owner, count, rw) && owner == 0x1F2C && count == 1 && !rw);
        CHECK(DecodeLock((0x80000001ull << 32) | 0x1F2C, owner, count, rw) && rw && count == 1);
        CHECK(!DecodeLock(0, owner, count, rw));
        CHECK(!DecodeLock((1ull << 32) | 0x1F2D, owner, count, rw));       // not a thread id
        CHECK(!DecodeLock((0x20000ull << 32) | 0x1F2C, owner, count, rw)); // count out of range
        CHECK(DecodeLock((0x80000000ull << 32) | 0x1F2C, owner, count, rw) && rw && count == 0);  // unlocked in the wrong order
    }

    void RecordsInOwnProcess()
    {
        std::printf("game records read through RTTI\n");
        const auto  base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + static_cast<std::uint64_t>(dos->e_lfanew));
        Module      exe = MakeModule(L"dd_analysis_tests.exe", base, ModuleKind::kGame);
        exe.size = nt->OptionalHeader.SizeOfImage;

        static TESFile master{ {}, "DDTestMaster.esm" };
        static TESFile records{ {}, "DDTestRecords.esp" };
        static TESFile broken{ {}, "not a plugin" };
        TESFile*       npcFiles[2] = { &master, &records };
        TESFile*       refFiles[1] = { &records };
        TESFile*       badFiles[1] = { &broken };
        TESFileArray   npcArray{ npcFiles, 2, 0 };
        TESFileArray   refArray{ refFiles, 1, 0 };
        TESFileArray   badArray{ badFiles, 1, 0 };

        auto* npc = new TESNPC();
        npc->files = &npcArray;
        npc->formID = 0x0A0012F3;
        auto* reference = new Character();
        reference->files = &refArray;
        reference->formID = 0x0A000801;
        reference->baseObject = npc;
        auto* odd = new TESNPC();
        odd->files = &badArray;
        odd->formID = 0x0B000001;
        auto* other = new NotAForm();

        Snapshot     s;
        FormResolver resolver(GetCurrentProcess(), exe, Game::kSkyrim);
        const int    index = resolver.Resolve(reinterpret_cast<std::uint64_t>(reference), s);
        CHECK(index >= 0);
        if (index >= 0) {
            const FormInfo& f = s.forms[static_cast<std::size_t>(index)];
            CHECK(f.className == L"Character");
            CHECK(f.formId == 0x0A000801);
            CHECK(f.files.size() == 1 && f.files[0] == L"DDTestRecords.esp");
            CHECK(f.baseForm >= 0);
            if (f.baseForm >= 0) {
                const FormInfo& b = s.forms[static_cast<std::size_t>(f.baseForm)];
                CHECK(b.className == L"TESNPC");
                CHECK(b.files.size() == 2 && b.files[0] == L"DDTestMaster.esm" && b.files[1] == L"DDTestRecords.esp");
            }
            CHECK(f.cell == -1);
        }
        const int oddIndex = resolver.Resolve(reinterpret_cast<std::uint64_t>(odd), s);
        CHECK(oddIndex >= 0 && s.forms[static_cast<std::size_t>(oddIndex)].files.empty());
        CHECK(resolver.Resolve(reinterpret_cast<std::uint64_t>(other), s) == -1);
        CHECK(resolver.Resolve(reinterpret_cast<std::uint64_t>(&master), s) == -1);
        CHECK(resolver.Resolve(0x10, s) == -1);
        delete npc;
        delete reference;
        delete odd;
        delete other;
    }

    void EndlessLoading()
    {
        std::printf("endless loading screen with a loader thread busy in a plugin\n");
        Snapshot s = BaseSnapshot();
        s.reason = CaptureReason::kEndlessLoading;
        s.loadingWatch = true;
        s.loadingScreenSeconds = 200;
        s.heartbeatAgeSeconds = 0.05;
        s.threads.push_back(MakeThread(4, { kNtDelay, kSleepEx, kGame + 0x3000, kDecoy + 0x100, kGame + 0x2000 }, 0.05));
        s.threads.push_back(MakeBusyThread(8, kCulprit, { kGame + 0x4000 }, kGame + 0x9000));
        AddDecoyPool(s);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"endless_loading");
        CHECK(std::wstring(Top(v, s)) == L"Culprit.dll");
        CHECK(!v.suspects.empty() && v.suspects.front().confidence == Confidence::kMedium);
        CHECK(!HasSuspect(v, s, L"Decoy.dll"));
    }

    void RecordLeads()
    {
        std::printf("records an engine loop works on\n");
        Snapshot s = BaseSnapshot();
        FormInfo npc;
        npc.formId = 0x0A0012F3;
        npc.className = L"TESNPC";
        npc.files = { L"DDTestMaster.esm", L"DDTestRecords.esp" };
        FormInfo player;
        player.formId = 0x14;
        player.className = L"PlayerCharacter";
        player.files = { L"Skyrim.esm" };
        FormInfo reference;
        reference.formId = 0x0A000801;
        reference.className = L"Character";
        reference.files = { L"DDTestRecords.esp" };
        reference.baseForm = 0;
        s.forms = { npc, player, reference };
        ThreadData main = MakeBusyThread(4, kGame, { kDecoy + 0x100, kGame + 0x2000 });
        main.forms = { { 1, 40, 0x20 }, { 2, 40, 0x28 }, { 0, 10, 0x300 } };
        s.threads.push_back(main);
        const Verdict v = Analyze(s);
        CHECK(v.kind == L"main_thread_loop_in_game");
        CHECK(v.suspects.empty());  // the decoy on the call path is evidence, not a suspect
        CHECK(!v.records.empty() && v.records.front().form == 2 && v.records.front().confidence == Confidence::kMedium);
        bool player_listed = false;
        bool npc_listed = false;
        for (const auto& lead : v.records) {
            player_listed |= lead.form == 1;
            npc_listed |= lead.form == 0;
        }
        CHECK(!player_listed);  // the player is on every stack
        CHECK(!npc_listed);     // seen in 25% of samples only
    }
}

int main()
{
    MainBusyInPlugin();
    EngineWaitWithHookOnPathBlamesNobody();
    EngineWaitWithBusyWorker();
    BusyWorkerRunsOwnTask();
    PluginLoopAroundEngine();
    StuckInExtenderCallback();
    SpinWaitBetweenSleeps();
    CosaveReadLoop();
    JobThreadLoopsInPlugin();
    SnapshotRoundTrip();
    BrokenRecordNamesPatcher();
    PluginWalkingBrokenRecord();
    SelfDeadlockOnReentry();
    Deadlock();
    HiddenDialog();
    EngineLockOwnedByPluginThread();
    LockLeftHeldByHandler();
    LockLeftHeldUnrecorded();
    LockLeftHeldByPluginThread();
    LockLeftHeldFromRecordOnly();
    LockRecordsRoundTrip();
    GpuWait();
    GpuPollInGame();
    EngineLoopInTask();
    CodeShapes();
    RecordsInOwnProcess();
    EndlessLoading();
    RecordLeads();
    std::printf("%d check(s) failed\n", g_failed);
    return g_failed ? 1 : 0;
}
