#pragma once

// Everything a capture collects. The analysis reads only this, so it can be tested with
// hand-made snapshots.

#include "common/Protocol.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dd::helper
{
    enum class ModuleKind
    {
        kOther,
        kGame,             // the game executable
        kSystem,           // Windows
        kRuntime,          // C/C++ runtimes shared by everything
        kGraphics,         // Direct3D / DXGI and GPU drivers
        kGraphicsInjector, // d3d11.dll / dxgi.dll / ENB / ReShade in the game folder
        kOverlay,          // Steam, Discord, RivaTuner, recording tools
        kScriptExtender,   // SKSE / F4SE runtime
        kGameLibrary,      // libraries that ship with the game (Bink, Steam API, NVIDIA GameWorks)
        kModTool,          // Mod Organizer's virtual file system
        kSelf,             // Dump Diver
        kPlugin,           // SKSE / F4SE plugins and other mod DLLs
        kTestHarness,      // test tools that drive the game (never a culprit)
    };

    struct Module
    {
        std::wstring  name;      // file name
        std::wstring  path;      // as the game sees it
        std::wstring  realPath;  // the file actually mapped (Mod Organizer's real mod folder)
        std::wstring  modName;   // Mod Organizer mod folder, if any
        std::wstring  version;
        std::uint64_t base = 0;
        std::uint64_t size = 0;
        ModuleKind    kind = ModuleKind::kOther;

        bool Contains(std::uint64_t address) const { return address >= base && address < base + size; }
    };

    struct Sample
    {
        std::vector<std::uint64_t> frames;  // frames[0] is the instruction pointer
    };

    struct Registers
    {
        std::uint64_t rip = 0, rsp = 0, rbp = 0, rbx = 0, rsi = 0, rdi = 0;
        std::uint64_t r12 = 0, r13 = 0, r14 = 0, r15 = 0;
        std::uint64_t rax = 0, rcx = 0, rdx = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;
    };

    // A game record (TESForm) found in a thread's registers or stack.
    struct FormInfo
    {
        std::uint64_t             address = 0;
        std::uint32_t             formId = 0;
        std::uint8_t              formType = 0;
        std::wstring              className;  // from the game's RTTI: TESNPC, Character, TESObjectCELL...
        std::vector<std::wstring> files;      // plugins with this record, in load order: defined in the first,
                                              // last changed by the last
        int                       baseForm = -1;  // references: their base object
        int                       cell = -1;      // references: their cell
        std::wstring              problem;        // broken data found in the record, e.g. a face NPC loop
    };

    struct FormRef
    {
        int           form = -1;     // index into Snapshot::forms
        std::uint32_t samples = 0;   // samples it was seen in
        std::uint32_t offset = 0;    // 0 = a register, else bytes above RSP (smaller = nearer the running code)
    };

    struct ValueStat
    {
        std::uint32_t samples = 0;
        std::uint32_t offset = 0xFFFFFFFF;
    };

    struct ThreadData
    {
        std::uint32_t       tid = 0;
        std::wstring        name;
        std::uint64_t       startAddress = 0;
        std::uint64_t       cpuTime100ns = 0;   // user + kernel time spent during the sample window
        std::uint64_t       contextSwitches = 0; // during the sample window
        std::uint32_t       state = 0;           // KTHREAD_STATE at the end of the window
        std::uint32_t       waitReason = 0;
        bool                isMain = false;
        bool                ranHeartbeat = false;
        std::vector<Sample> samples;
        Registers           registers;   // from the last sample
        std::vector<std::uint8_t> stackTop; // the top of the stack from rsp, last sample
        std::vector<FormRef> forms;         // game records it was working on, most often seen first
    };

    enum class WctObject : std::uint32_t
    {
        kCriticalSection = 1,
        kSendMessage,
        kMutex,
        kAlpc,
        kCom,
        kThreadWait,
        kProcessWait,
        kThread,
        kComActivation,
        kUnknown,
        kSocketIo,
        kSmbIo,
    };

    struct WctNode
    {
        WctObject     type = WctObject::kUnknown;
        std::uint32_t processId = 0;  // thread nodes
        std::uint32_t threadId = 0;   // thread nodes
        std::uint32_t waitTime = 0;   // thread nodes, ms
        std::uint32_t contextSwitches = 0;
        std::uint32_t status = 0;
        std::wstring  objectName;
    };

    struct WaitChain
    {
        std::uint32_t        threadId = 0;
        bool                 cycle = false;
        std::vector<WctNode> nodes;  // nodes[0] is the thread itself
    };

    struct Dialog
    {
        std::uint32_t threadId = 0;
        std::wstring  title;
        std::wstring  text;
        std::wstring  className;
        bool          visible = false;
    };

    // A lock held by another thread, found near a thread that sleeps in a retry loop.
    struct LockHit
    {
        std::uint32_t waiterTid = 0;
        std::uint64_t lockAddress = 0;
        std::uint32_t ownerTid = 0;
        std::uint32_t count = 0;
        bool          readWrite = false;  // BSReadWriteLock held for writing
        bool          inGameData = false; // inside the game executable's data, not the heap
        std::wstring  foundIn;            // "RBX", "RSP+0x28"
        bool          ownerAlive = true;  // false: the owner thread has exited, nothing will release it
        bool          broken = false;     // writer flag with no count: unlocked in the wrong order
    };

    // From the game plugin: an engine lock a thread still held when a message handler returned or
    // when the thread exited (see LockRecord in Protocol.h), and what the lock looked like at capture.
    struct LockRecordInfo
    {
        std::uint64_t  qpc = 0;
        std::uint64_t  lockAddress = 0;
        std::uint64_t  code = 0;  // the handler, or the thread's start address
        std::uint32_t  threadId = 0;
        LockRecordKind kind = LockRecordKind::kNone;
        std::uint32_t  lockWord = 0;
        std::uint32_t  messageType = 0;
        std::uint64_t  current = 0;  // the lock's 8 bytes at capture
    };

    struct TaskInFlight
    {
        std::uint32_t threadId = 0;
        std::uint64_t startQpc = 0;
        std::uint64_t runAddress = 0;
        std::uint64_t callerAddress = 0;
        std::uint64_t queuedQpc = 0;
        std::uint32_t kind = 0;
    };

    struct EventRecord
    {
        std::uint64_t qpc = 0;
        EventType     type = EventType::kNone;
        std::uint32_t threadId = 0;
        std::uint64_t a = 0;
    };

    enum class CaptureReason
    {
        kFreeze,
        kStartup,
        kManual,
        kEndlessLoading,
    };

    struct Snapshot
    {
        Game          game = Game::kUnknown;
        CaptureReason reason = CaptureReason::kFreeze;
        std::uint32_t processId = 0;
        std::uint32_t mainThreadId = 0;
        std::uint32_t heartbeatThreadId = 0;
        std::uint32_t runtimeVersion = 0;
        std::uint32_t stateFlags = 0;
        std::uint64_t qpcFrequency = 1;
        std::uint64_t captureQpc = 0;
        std::uint64_t heartbeatQpc = 0;
        std::uint64_t loadStartQpc = 0;
        double        heartbeatAgeSeconds = 0;
        double        sampleSeconds = 0;
        bool          windowHung = false;
        bool          windowForeground = false;
        bool          recovered = false;  // the heartbeat came back during the capture
        bool          taskTracking = false;
        bool          loadingWatch = false;
        std::uint32_t lockWatch = 0;  // bit 0: message handlers checked, bit 1: thread exits checked
        double        loadingScreenSeconds = 0;  // how long the current loading screen has been up; 0 = none
        std::wstring  exePath;
        std::wstring  timeText;  // local time of the capture

        std::vector<Module>       modules;
        std::vector<ThreadData>   threads;
        std::vector<WaitChain>    chains;
        std::vector<Dialog>       dialogs;
        std::vector<LockHit>      locks;
        std::vector<TaskInFlight> tasks;
        std::vector<EventRecord>  events;
        std::vector<FormInfo>     forms;
        std::vector<LockRecordInfo> lockRecords;

        std::map<std::uint64_t, std::wstring> symbols;       // address -> "module!function+0x12"
        std::map<std::uint64_t, bool>         gpuPollSites;  // return address in the game -> polls a GPU query

        int ModuleAt(std::uint64_t address) const
        {
            for (std::size_t i = 0; i < modules.size(); ++i) {
                if (modules[i].Contains(address))
                    return static_cast<int>(i);
            }
            return -1;
        }

        const ThreadData* Thread(std::uint32_t tid) const
        {
            for (const auto& t : threads) {
                if (t.tid == tid)
                    return &t;
            }
            return nullptr;
        }

        double Seconds(std::uint64_t from, std::uint64_t to) const
        {
            if (!from || to < from || !qpcFrequency)
                return 0;
            return static_cast<double>(to - from) / static_cast<double>(qpcFrequency);
        }
    };
}
